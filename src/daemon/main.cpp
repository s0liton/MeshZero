// SPDX-License-Identifier: GPL-3.0-only
//
// meshzerod: sits between the radio daemon (which accepts a single API client)
// and any number of API clients. It keeps a copy of the radio's configuration
// handshake so every client can be answered independently, fans live packets
// out to all of them, and records text messages so the on-device UI has a
// history even when it was not running.

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/history.h"
#include "common/proto.h"
#include "common/util.h"
#include "daemon/ble.h"
#include "daemon/hw.h"

using namespace mz;

namespace {

constexpr uint32_t kNonceConfigOnly = 69420;
constexpr uint32_t kNonceNodesOnly  = 69421;
constexpr uint64_t kCacheFreshMs    = 20000;
constexpr uint64_t kSyncTimeoutMs   = 20000;
constexpr uint64_t kHeartbeatMs     = 60000;
constexpr uint64_t kReconnectMs     = 1000;
constexpr size_t kMaxClientBacklog  = 4 * 1024 * 1024;
constexpr size_t kMaxBleBacklog     = 4096;  // messages waiting for a phone to read them
constexpr uint64_t kPairingMs       = 120000;  // how long a new phone may pair after it was asked for

volatile sig_atomic_t g_stop = 0;
int g_verbose                = 0;

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

const char* env_or(const char* name, const char* fallback)
{
    const char* v = getenv(name);
    return v && v[0] ? v : fallback;
}

void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

// Snapshot of everything the radio sends in answer to want_config, kept as the
// encoded FromRadio payloads so they can be replayed byte-for-byte.
struct ConfigCache {
    std::string my_info;
    std::string metadata;
    std::string deviceui;
    std::map<int, std::string> channels;        // by channel index
    std::map<int, std::string> configs;         // by Config payload variant
    std::map<int, std::string> module_configs;  // by ModuleConfig payload variant
    std::map<uint32_t, std::string> nodes;      // by node number
    std::vector<std::string> files;
    std::vector<uint32_t> node_order;
    uint32_t my_num = 0;
    bool valid      = false;

    void clear() { *this = ConfigCache{}; }
};

struct Client {
    int fd = -1;
    FrameParser parser;
    std::string out;
    std::string peer;
    bool want_config    = false;
    uint32_t nonce      = 0;
    bool closing        = false;
    bool local          = false;  // connected from this device
    bool wants_control  = false;  // has asked about the service settings
    uint64_t connected  = 0;
    // A phone on Bluetooth has no socket: it is known by the name the system's
    // Bluetooth daemon gives it, and fetches its messages one by one.
    std::string ble;
    std::deque<std::string> ble_out;
    bool ble_gone = false;  // it disconnected by itself
};

// Requests to the service itself travel as packets to node 0 on the private
// port with this prefix. They are accepted from this device only and are
// never passed on to the radio.
constexpr char kControlPrefix[] = "mz:";

std::string access_path()
{
    return history_dir() + "/network-access";
}

bool read_network_access()
{
    FILE* f = fopen(access_path().c_str(), "r");
    if (!f) return false;
    const int ch = fgetc(f);
    fclose(f);
    return ch == '1';
}

bool write_flag(const std::string& path, bool on)
{
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fputs(on ? "1\n" : "0\n", f);
    return fclose(f) == 0;
}

bool write_network_access(bool on)
{
    return write_flag(access_path(), on);
}

bool exists(const std::string& path)
{
    return access(path.c_str(), F_OK) == 0;
}

// Whether the radio keeps running while no app is using it. Off unless
// switched on: while it runs, other apps cannot use the LoRa cap.
std::string background_path()
{
    return history_dir() + "/run-in-background";
}

bool read_background()
{
    FILE* f = fopen(background_path().c_str(), "r");
    if (!f) return false;
    const int ch = fgetc(f);
    fclose(f);
    return ch == '1';
}

// Present while the app is to ask the user whether to run in the background,
// after the update that switched it off for them.
std::string background_ask_path()
{
    return history_dir() + "/background-ask";
}

// Sets running in the background as the user chose it, which also answers
// the question above.
bool write_background(bool on)
{
    unlink(background_ask_path().c_str());
    return write_flag(background_path(), on);
}

// Up to 0.2.0 running in the background was on unless switched off, kept in a
// file named "background". The update switches it off for everybody; whoever
// had it on is asked in the app, once, whether to have it back.
void migrate_background()
{
    if (exists(background_path())) return;
    const std::string old_path = history_dir() + "/background";
    bool was_on                = false;
    if (FILE* f = fopen(old_path.c_str(), "r")) {
        was_on = fgetc(f) != '0';
        fclose(f);
    } else {
        // Never switched, so it was on wherever the radio has run before.
        was_on = exists(history_dir() + "/radio-status") || exists(history_dir() + "/radio") || exists(history_path());
    }
    if (was_on && !write_flag(background_ask_path(), true)) {
        logf("cannot save %s: %s", background_ask_path().c_str(), strerror(errno));
    }
    if (!write_flag(background_path(), false)) {
        logf("cannot save %s: %s", background_path().c_str(), strerror(errno));
        return;
    }
    unlink(old_path.c_str());
    if (was_on) logf("running in the background is now off until the user turns it back on");
}

// Bluetooth is off unless switched on. With the PIN on, as it is unless
// switched off, a phone has to pair with a code shown in the app.
std::string bluetooth_path()
{
    return history_dir() + "/bluetooth";
}

std::string bluetooth_pin_path()
{
    return history_dir() + "/bluetooth-pin";
}

// The name the service was last offered under, so that it can be offered
// again before the radio has been asked who it is.
std::string bluetooth_name_path()
{
    return history_dir() + "/bluetooth-name";
}

// The addresses of the phones that have used the service over Bluetooth, one
// per line: forgetting pairings is about these, not about a headset the
// device is paired with for something else.
std::string bluetooth_phones_path()
{
    return history_dir() + "/bluetooth-phones";
}

std::vector<std::string> read_phones()
{
    std::vector<std::string> out;
    if (FILE* f = fopen(bluetooth_phones_path().c_str(), "r")) {
        char buf[64];
        while (fgets(buf, sizeof(buf), f)) {
            const std::string line = trim(buf);
            if (!line.empty()) out.push_back(line);
        }
        fclose(f);
    }
    return out;
}

bool write_phones(const std::vector<std::string>& phones)
{
    FILE* f = fopen(bluetooth_phones_path().c_str(), "w");
    if (!f) return false;
    for (const std::string& p : phones) fprintf(f, "%s\n", p.c_str());
    return fclose(f) == 0;
}

// The phones among the devices the Bluetooth daemon has a pairing with.
std::vector<Ble::Device> paired_phones()
{
    const std::vector<std::string> known = read_phones();
    std::vector<Ble::Device> out;
    for (Ble::Device& d : Ble::devices()) {
        if (d.paired && std::find(known.begin(), known.end(), d.address) != known.end()) out.push_back(std::move(d));
    }
    return out;
}

// Forgets the pairing with every phone; returns how many there were. Each
// phone has to forget this device too before it can pair again.
int forget_phones()
{
    int count = 0;
    for (const Ble::Device& d : paired_phones()) {
        if (Ble::remove_device(d.path)) ++count;
    }
    write_phones({});
    return count;
}

// Left by 'meshzerod bluetooth pair' for the service to find: let a new phone pair now.
std::string bluetooth_pair_path()
{
    return history_dir() + "/bluetooth-pair";
}

bool read_flag(const std::string& path, bool fallback)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return fallback;
    const int ch = fgetc(f);
    fclose(f);
    return ch == '1' ? true : ch == '0' ? false : fallback;
}

std::string read_line(const std::string& path)
{
    std::string out;
    if (FILE* f = fopen(path.c_str(), "r")) {
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), f)) out = trim(buf);
        fclose(f);
    }
    return out;
}

// Deletes the stored messages of one conversation, or of all of them, as the
// app asks: "c<channel index>", "d<node number>" or "all". The app works out
// conversations the same way when it loads the history.
void clear_history(const std::string& which)
{
    std::function<bool(const StoredMsg&)> drop;
    if (which == "all") {
        drop = [](const StoredMsg&) { return true; };
    } else if (which.size() > 1 && (which[0] == 'c' || which[0] == 'd')) {
        char* end              = nullptr;
        const unsigned long id = strtoul(which.c_str() + 1, &end, 10);
        if (*end != 0) return;
        const bool dm = which[0] == 'd';
        drop          = [dm, id](const StoredMsg& m) {
            if (m.to == kBroadcast) return !dm && m.channel == id;
            return dm && (m.outgoing ? m.to : m.from) == id;
        };
    } else {
        return;
    }
    if (history_remove(drop)) {
        logf("deleted the message history of %s", which.c_str());
    } else {
        logf("cannot rewrite %s: %s", history_path().c_str(), strerror(errno));
    }
}

class Proxy {
public:
    Proxy(std::string upstream_host, uint16_t upstream_port, std::string bind_addr, uint16_t listen_port)
        : upstream_host_(std::move(upstream_host)),
          upstream_port_(upstream_port),
          bind_addr_(std::move(bind_addr)),
          listen_port_(listen_port),
          rng_(std::random_device{}())
    {
    }

    bool start_listening();
    int listen_on(const std::string& bind_addr);
    void run();
    void set_network_access(bool on) { net_access_ = on; }
    void load_bluetooth()
    {
        bt_enabled_ = read_flag(bluetooth_path(), false) && !net_access_;
        bt_pin_     = read_flag(bluetooth_pin_path(), true);
        bt_name_    = read_line(bluetooth_name_path());
    }
    // Makes this process start and stop the radio daemon itself.
    void set_radio_command(std::string cmd) { radio_cmd_ = std::move(cmd); }
    void shutdown();

private:
    // upstream
    void upstream_connect();
    void upstream_close(const char* why);
    void upstream_send(const std::string& payload);
    void upstream_read();
    void upstream_flush();
    void start_sync();
    void finish_sync();
    void on_from_radio(const std::string& payload);
    void absorb_config_frame(const meshtastic_FromRadio& msg, const std::string& payload, ConfigCache& cache);
    void broadcast_changes(const ConfigCache& before, const ConfigCache& after);

    // clients
    void accept_client(int listen_fd);
    void client_read(Client& c);
    void client_flush(Client& c);
    void client_send(Client& c, const std::string& payload);
    void on_to_radio(Client& c, const std::string& payload);
    void serve_config(Client& c);
    void broadcast(const std::string& payload, const Client* except = nullptr);
    void drop_all_clients();
    void on_control(Client& c, const std::string& request);
    void send_control(Client& c);
    void apply_network_access(bool on);
    void watch_radio(bool asked);
    void watch_bluetooth();
    std::string bluetooth_name();
    Client& ble_client(const std::string& device);
    void send_control_line(Client& c, const std::string& setting);
    void tell_control_clients();
    void watch_apps();
    void start_radio();
    void stop_radio(const char* why);
    void reap_radio();

    // history
    void record_incoming(const meshtastic_MeshPacket& p);
    void record_outgoing(const meshtastic_MeshPacket& p);

    std::string upstream_host_;
    uint16_t upstream_port_;
    std::string bind_addr_;
    uint16_t listen_port_;

    std::vector<int> listen_fds_;
    bool net_access_ = false;  // listen on bind_addr_ instead of loopback only
    uint64_t last_access_check_ms_ = 0;
    uint64_t last_radio_watch_ms_  = 0;
    uint64_t last_client_ms_       = 0;
    bool background_               = false;

    // The radio daemon, when this process runs it (radio_cmd_ set).
    std::string radio_cmd_;
    pid_t radio_pid_           = -1;
    uint64_t radio_started_ms_ = 0;
    uint64_t radio_stop_ms_    = 0;  // when it was asked to stop, 0 if not
    uint64_t radio_retry_ms_   = 0;  // earliest next start
    uint64_t radio_backoff_ms_ = 0;
    uint64_t radio_idle_ms_    = 0;  // since when nobody has needed it
    int cap_misses_            = 0;
    bool legacy_cap_           = false; // the older model of the cap, no expander
    std::string radio_status_;
    int upstream_fd_ = -1;
    bool upstream_connecting_ = false;
    FrameParser upstream_parser_;
    std::string upstream_out_;
    uint64_t next_connect_ms_   = 0;
    uint64_t last_heartbeat_ms_ = 0;

    ConfigCache cache_;
    ConfigCache pending_;
    bool syncing_           = false;
    bool sync_announce_     = false;  // push differences to connected clients when this sync ends
    uint32_t sync_nonce_    = 0;
    uint64_t sync_start_ms_ = 0;
    uint64_t last_sync_ms_  = 0;
    uint64_t resync_at_ms_  = 0;

    Ble ble_;
    bool bt_enabled_       = false;
    bool bt_pin_           = true;
    bool bt_started_pin_   = true;   // the PIN setting the running service was started with
    bool bt_reported_      = false;  // what the apps were last told about it being on the air
    uint64_t bt_retry_ms_  = 0;
    std::string bt_name_;
    std::string bt_started_name_;
    std::string bt_passkey_;
    std::string bt_why_;
    // Who is using the radio from outside, for the app to show.
    bool apps_dirty_  = true;   // connections or pairings may have changed
    int bt_clients_   = 0;      // phones connected over Bluetooth
    int bt_paired_    = 0;      // phones paired
    uint64_t bt_pair_until_ms_ = 0;  // a new phone may pair until then, 0 for not
    void set_pairing_window(bool open);
    void patch_bluetooth_config(meshtastic_Config_BluetoothConfig& bt) const;
    int net_clients_  = 0;      // clients connected from other devices
    std::string bt_peer_;       // the name of a connected phone

    std::vector<std::unique_ptr<Client>> clients_;
    HistoryWriter history_;
    std::unordered_map<uint32_t, uint32_t> outgoing_dest_;  // packet id -> destination, for ack classification
    std::deque<uint32_t> outgoing_order_;                   // ids in outgoing_dest_, oldest first
    static constexpr size_t kMaxTrackedOutgoing = 512;
    std::mt19937 rng_;
};

int Proxy::listen_on(const std::string& bind_addr)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(listen_port_);
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        logf("invalid bind address %s", bind_addr.c_str());
        close(fd);
        return -1;
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(fd, 16) < 0) {
        logf("cannot listen on %s:%u: %s", bind_addr.c_str(), listen_port_, strerror(errno));
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    logf("API listening on %s:%u", bind_addr.c_str(), listen_port_);
    return fd;
}

// This device can always connect. Other devices can once network access is on.
bool Proxy::start_listening()
{
    for (int fd : listen_fds_) close(fd);
    listen_fds_.clear();
    const bool everywhere = net_access_ && bind_addr_ == "0.0.0.0";
    const int local       = listen_on(everywhere ? bind_addr_ : "127.0.0.1");
    if (local < 0) return false;
    listen_fds_.push_back(local);
    if (net_access_ && !everywhere && bind_addr_ != "127.0.0.1") {
        const int extra = listen_on(bind_addr_);
        if (extra < 0) return false;
        listen_fds_.push_back(extra);
    }
    return true;
}

// ---------------------------------------------------------------- upstream

void Proxy::upstream_connect()
{
    upstream_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (upstream_fd_ < 0) return;
    set_nonblock(upstream_fd_);
    int one = 1;
    setsockopt(upstream_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(upstream_port_);
    inet_pton(AF_INET, upstream_host_.c_str(), &addr.sin_addr);
    const int rc = connect(upstream_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(upstream_fd_);
        upstream_fd_ = -1;
        return;
    }
    upstream_connecting_ = true;
    upstream_parser_.reset();
    upstream_out_.clear();
}

void Proxy::upstream_close(const char* why)
{
    if (upstream_fd_ < 0) return;
    if (!upstream_connecting_) logf("radio link lost (%s)", why);
    close(upstream_fd_);
    upstream_fd_         = -1;
    upstream_connecting_ = false;
    syncing_             = false;
    cache_.clear();
    next_connect_ms_ = now_ms() + kReconnectMs;
    // A radio restart invalidates everything clients learned; dropping them
    // makes them reconnect and resynchronise, exactly as with a real device.
    drop_all_clients();
}

void Proxy::upstream_send(const std::string& payload)
{
    if (upstream_fd_ < 0 || upstream_connecting_) return;
    upstream_out_ += frame_wrap(payload);
    upstream_flush();
}

void Proxy::upstream_flush()
{
    while (!upstream_out_.empty()) {
        const ssize_t n = send(upstream_fd_, upstream_out_.data(), upstream_out_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            upstream_out_.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            upstream_close("send failed");
            return;
        }
    }
}

void Proxy::start_sync()
{
    if (upstream_fd_ < 0 || upstream_connecting_ || syncing_) return;
    pending_.clear();
    syncing_       = true;
    sync_start_ms_ = now_ms();
    do {
        sync_nonce_ = static_cast<uint32_t>(rng_());
    } while (sync_nonce_ == 0 || sync_nonce_ == kNonceConfigOnly || sync_nonce_ == kNonceNodesOnly);
    meshtastic_ToRadio req   = meshtastic_ToRadio_init_zero;
    req.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    req.want_config_id        = sync_nonce_;
    upstream_send(pb_encode_msg(meshtastic_ToRadio_fields, req));
}

void Proxy::absorb_config_frame(const meshtastic_FromRadio& msg, const std::string& payload, ConfigCache& cache)
{
    switch (msg.which_payload_variant) {
        case meshtastic_FromRadio_my_info_tag:
            cache.my_info = payload;
            cache.my_num  = msg.my_info.my_node_num;
            break;
        case meshtastic_FromRadio_metadata_tag:
            cache.metadata = payload;
            break;
        case meshtastic_FromRadio_deviceuiConfig_tag:
            cache.deviceui = payload;
            break;
        case meshtastic_FromRadio_channel_tag:
            cache.channels[msg.channel.index] = payload;
            break;
        case meshtastic_FromRadio_config_tag:
            if (msg.config.which_payload_variant == meshtastic_Config_bluetooth_tag && Ble::supported()) {
                meshtastic_FromRadio patched = msg;
                patch_bluetooth_config(patched.config.payload_variant.bluetooth);
                cache.configs[msg.config.which_payload_variant] = pb_encode_msg(meshtastic_FromRadio_fields, patched);
                break;
            }
            cache.configs[msg.config.which_payload_variant] = payload;
            break;
        case meshtastic_FromRadio_moduleConfig_tag:
            cache.module_configs[msg.moduleConfig.which_payload_variant] = payload;
            break;
        case meshtastic_FromRadio_node_info_tag:
            if (cache.nodes.find(msg.node_info.num) == cache.nodes.end()) cache.node_order.push_back(msg.node_info.num);
            cache.nodes[msg.node_info.num] = payload;
            break;
        case meshtastic_FromRadio_fileInfo_tag:
            cache.files.push_back(payload);
            break;
        default:
            break;
    }
}

void Proxy::finish_sync()
{
    syncing_      = false;
    last_sync_ms_ = now_ms();
    pending_.valid = true;
    const bool first = !cache_.valid;
    ConfigCache before = std::move(cache_);
    cache_             = std::move(pending_);
    pending_.clear();
    if (first) {
        logf("radio ready: node %s, %zu nodes, %zu channels", node_id_str(cache_.my_num).c_str(), cache_.nodes.size(),
             cache_.channels.size());
    } else if (sync_announce_) {
        broadcast_changes(before, cache_);
    }
    sync_announce_ = false;
    for (auto& c : clients_) {
        if (c->want_config) serve_config(*c);
    }
}

// After a settings change made by one client, tell the others what moved.
void Proxy::broadcast_changes(const ConfigCache& before, const ConfigCache& after)
{
    const auto diff_map = [this](const std::map<int, std::string>& a, const std::map<int, std::string>& b) {
        for (const auto& [key, value] : b) {
            const auto it = a.find(key);
            if (it == a.end() || it->second != value) broadcast(value);
        }
    };
    diff_map(before.channels, after.channels);
    diff_map(before.configs, after.configs);
    diff_map(before.module_configs, after.module_configs);
    const auto mine_before = before.nodes.find(after.my_num);
    const auto mine_after  = after.nodes.find(after.my_num);
    if (mine_after != after.nodes.end() && (mine_before == before.nodes.end() || mine_before->second != mine_after->second)) {
        broadcast(mine_after->second);
    }
}

void Proxy::on_from_radio(const std::string& payload)
{
    meshtastic_FromRadio msg = meshtastic_FromRadio_init_zero;
    if (!pb_decode_msg(meshtastic_FromRadio_fields, payload, msg)) {
        if (g_verbose) logf("undecodable frame from radio (%zu bytes), forwarding", payload.size());
        broadcast(payload);
        return;
    }

    switch (msg.which_payload_variant) {
        case meshtastic_FromRadio_config_complete_id_tag:
            if (syncing_ && msg.config_complete_id == sync_nonce_) finish_sync();
            return;
        case meshtastic_FromRadio_my_info_tag:
        case meshtastic_FromRadio_metadata_tag:
        case meshtastic_FromRadio_deviceuiConfig_tag:
        case meshtastic_FromRadio_channel_tag:
        case meshtastic_FromRadio_config_tag:
        case meshtastic_FromRadio_moduleConfig_tag:
        case meshtastic_FromRadio_fileInfo_tag:
            if (syncing_) {
                absorb_config_frame(msg, payload, pending_);
            } else {
                absorb_config_frame(msg, payload, cache_);
                broadcast(payload);
            }
            return;
        case meshtastic_FromRadio_node_info_tag:
            if (syncing_) {
                absorb_config_frame(msg, payload, pending_);
            } else {
                // The radio pushes a fresh NodeInfo when it learns about a node.
                absorb_config_frame(msg, payload, cache_);
                broadcast(payload);
            }
            return;
        case meshtastic_FromRadio_packet_tag:
            record_incoming(msg.packet);
            broadcast(payload);
            return;
        case meshtastic_FromRadio_rebooted_tag:
            broadcast(payload);
            resync_at_ms_ = now_ms() + 500;
            return;
        default:
            broadcast(payload);
            return;
    }
}

void Proxy::upstream_read()
{
    uint8_t buf[4096];
    while (upstream_fd_ >= 0) {
        const ssize_t n = recv(upstream_fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            upstream_parser_.feed(buf, static_cast<size_t>(n), [this](const std::string& p) { on_from_radio(p); });
        } else if (n == 0) {
            upstream_close("closed by radio");
            return;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else if (errno != EINTR) {
            upstream_close(strerror(errno));
            return;
        }
    }
}

// ----------------------------------------------------------------- clients

void Proxy::accept_client(int listen_fd)
{
    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int fd  = accept(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) return;
        set_nonblock(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        auto c = std::make_unique<Client>();
        c->fd  = fd;
        char ip[32] = {};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        c->peer      = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
        sockaddr_in own{};
        socklen_t own_len = sizeof(own);
        getsockname(fd, reinterpret_cast<sockaddr*>(&own), &own_len);
        c->local = ntohl(own.sin_addr.s_addr) == INADDR_LOOPBACK && (ntohl(addr.sin_addr.s_addr) >> 24) == 127;
        c->connected = now_ms();
        logf("client connected: %s", c->peer.c_str());
        const bool radio_down = upstream_fd_ < 0 || upstream_connecting_;
        clients_.push_back(std::move(c));
        apps_dirty_ = true;
        if (radio_down) watch_radio(true);
    }
}

void Proxy::client_send(Client& c, const std::string& payload)
{
    if (c.closing) return;
    if (!c.ble.empty()) {
        c.ble_out.push_back(payload);
        if (c.ble_out.size() > kMaxBleBacklog) {
            logf("client %s is not reading, dropping it", c.peer.c_str());
            c.closing = true;
            return;
        }
        ble_.notify();
        return;
    }
    if (c.fd < 0) return;
    c.out += frame_wrap(payload);
    if (c.out.size() > kMaxClientBacklog) {
        logf("client %s is not reading, dropping it", c.peer.c_str());
        c.closing = true;
        return;
    }
    client_flush(c);
}

void Proxy::client_flush(Client& c)
{
    while (!c.out.empty() && c.fd >= 0) {
        const ssize_t n = send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.out.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            c.closing = true;
            c.out.clear();
            return;
        }
    }
}

void Proxy::broadcast(const std::string& payload, const Client* except)
{
    for (auto& c : clients_) {
        if (c.get() != except) client_send(*c, payload);
    }
}

void Proxy::drop_all_clients()
{
    for (auto& c : clients_) c->closing = true;
}

void Proxy::serve_config(Client& c)
{
    c.want_config          = false;
    const bool send_config = c.nonce != kNonceNodesOnly;
    const bool send_nodes  = c.nonce != kNonceConfigOnly;

    // Same order the firmware uses, so clients see nothing unusual. A request
    // for the nodes alone gets no my_info: the phone apps ask for the
    // configuration and the nodes in two rounds, and take a my_info in the
    // second one as the start of a new first round.
    if (send_config && !cache_.my_info.empty()) client_send(c, cache_.my_info);
    if (send_config && !cache_.deviceui.empty()) client_send(c, cache_.deviceui);
    const auto own = cache_.nodes.find(cache_.my_num);
    if (own != cache_.nodes.end()) client_send(c, own->second);
    if (send_config) {
        if (!cache_.metadata.empty()) client_send(c, cache_.metadata);
        for (const auto& [index, frame] : cache_.channels) client_send(c, frame);
        for (const auto& [variant, frame] : cache_.configs) client_send(c, frame);
        for (const auto& [variant, frame] : cache_.module_configs) client_send(c, frame);
    }
    if (send_nodes) {
        for (const uint32_t num : cache_.node_order) {
            if (num == cache_.my_num) continue;
            const auto it = cache_.nodes.find(num);
            if (it != cache_.nodes.end()) client_send(c, it->second);
        }
    }
    if (send_config) {
        for (const auto& frame : cache_.files) client_send(c, frame);
    }
    meshtastic_FromRadio done  = meshtastic_FromRadio_init_zero;
    done.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    done.config_complete_id    = c.nonce;
    client_send(c, pb_encode_msg(meshtastic_FromRadio_fields, done));
}

static bool admin_is_mutation(pb_size_t tag)
{
    switch (tag) {
        case meshtastic_AdminMessage_get_channel_request_tag:
        case meshtastic_AdminMessage_get_channel_response_tag:
        case meshtastic_AdminMessage_get_owner_request_tag:
        case meshtastic_AdminMessage_get_owner_response_tag:
        case meshtastic_AdminMessage_get_config_request_tag:
        case meshtastic_AdminMessage_get_config_response_tag:
        case meshtastic_AdminMessage_get_module_config_request_tag:
        case meshtastic_AdminMessage_get_module_config_response_tag:
        case meshtastic_AdminMessage_get_canned_message_module_messages_request_tag:
        case meshtastic_AdminMessage_get_canned_message_module_messages_response_tag:
        case meshtastic_AdminMessage_get_device_metadata_request_tag:
        case meshtastic_AdminMessage_get_device_metadata_response_tag:
        case meshtastic_AdminMessage_get_ringtone_request_tag:
        case meshtastic_AdminMessage_get_ringtone_response_tag:
        case meshtastic_AdminMessage_get_device_connection_status_request_tag:
        case meshtastic_AdminMessage_get_device_connection_status_response_tag:
        case meshtastic_AdminMessage_get_node_remote_hardware_pins_request_tag:
        case meshtastic_AdminMessage_get_node_remote_hardware_pins_response_tag:
            return false;
        default:
            return true;
    }
}

void Proxy::on_to_radio(Client& c, const std::string& payload)
{
    meshtastic_ToRadio msg = meshtastic_ToRadio_init_zero;
    if (!pb_decode_msg(meshtastic_ToRadio_fields, payload, msg)) {
        upstream_send(payload);
        return;
    }
    switch (msg.which_payload_variant) {
        case meshtastic_ToRadio_want_config_id_tag:
            c.want_config = true;
            c.nonce       = msg.want_config_id;
            if (cache_.valid && !syncing_ && now_ms() - last_sync_ms_ < kCacheFreshMs) {
                serve_config(c);
            } else {
                start_sync();
            }
            return;
        case meshtastic_ToRadio_disconnect_tag:
            c.closing = true;
            return;
        case meshtastic_ToRadio_heartbeat_tag:
            // The radio answers a heartbeat with its queue status, which some
            // clients use to tell that the link is alive.
            upstream_send(payload);
            return;
        case meshtastic_ToRadio_packet_tag: {
            meshtastic_MeshPacket& p = msg.packet;
            if (p.which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
                upstream_send(payload);
                return;
            }
            if (p.to == 0 && p.decoded.portnum == meshtastic_PortNum_PRIVATE_APP) {
                const std::string text(reinterpret_cast<const char*>(p.decoded.payload.bytes), p.decoded.payload.size);
                if (text.rfind(kControlPrefix, 0) == 0) {
                    if (c.local) on_control(c, text.substr(sizeof(kControlPrefix) - 1));
                    return;
                }
            }
            bool reencode = false;
            if (p.id == 0) {
                // Give every packet an id so acknowledgements can be matched.
                do {
                    p.id = static_cast<uint32_t>(rng_());
                } while (p.id == 0);
                reencode = true;
            }
            if (p.decoded.portnum == meshtastic_PortNum_ADMIN_APP && (p.to == 0 || p.to == cache_.my_num)) {
                meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_zero;
                if (pb_decode_msg(meshtastic_AdminMessage_fields, p.decoded.payload.bytes, p.decoded.payload.size, admin) &&
                    admin_is_mutation(admin.which_payload_variant)) {
                    resync_at_ms_ = now_ms() + 1500;
                    // The Bluetooth page of a phone app: the pairing mode is ours
                    // to keep. Switching it off from there is not taken, as it
                    // would cut off the phone that asked.
                    if (admin.which_payload_variant == meshtastic_AdminMessage_set_config_tag &&
                        admin.set_config.which_payload_variant == meshtastic_Config_bluetooth_tag && Ble::supported()) {
                        const bool pin = admin.set_config.payload_variant.bluetooth.mode !=
                                         meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;
                        if (pin != bt_pin_) {
                            write_flag(bluetooth_pin_path(), pin);
                            logf("bluetooth: pairing %s, set from an app", pin ? "with a code" : "without a code");
                        }
                    }
                }
            }
            upstream_send(reencode ? pb_encode_msg(meshtastic_ToRadio_fields, msg) : payload);
            if (p.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
                record_outgoing(p);
                // Show the message to the other connected clients too; the
                // radio never echoes what it was asked to transmit.
                meshtastic_FromRadio echo  = meshtastic_FromRadio_init_zero;
                echo.which_payload_variant = meshtastic_FromRadio_packet_tag;
                echo.packet                = p;
                echo.packet.from           = cache_.my_num;
                echo.packet.rx_time        = now_s();
                if (echo.packet.to == 0) echo.packet.to = kBroadcast;
                broadcast(pb_encode_msg(meshtastic_FromRadio_fields, echo), &c);
            }
            return;
        }
        default:
            upstream_send(payload);
            return;
    }
}

// ------------------------------------------------------------------ radio

// Runs the radio daemon only while a LoRa cap is attached and somebody needs
// the radio, so the cap is free for other apps the rest of the time. Also
// leaves a note for the app on why the radio is down.
void Proxy::watch_radio(bool asked)
{
    if (radio_cmd_.empty()) return;
    const uint64_t now = now_ms();
    reap_radio();

    // With background running off, the radio is only needed while somebody is
    // connected, plus a moment so a reconnect does not restart it.
    if (!clients_.empty()) last_client_ms_ = now;
    const bool wanted = background_ || !clients_.empty() || (last_client_ms_ != 0 && now - last_client_ms_ < 10000);
    const bool up     = upstream_fd_ >= 0 && !upstream_connecting_;
    std::string status = up ? "ok" : "starting";

    if (radio_pid_ > 0) {
        if (radio_stop_ms_ != 0) {
            if (now - radio_stop_ms_ > 5000) kill(radio_pid_, SIGKILL);
        } else if (!wanted) {
            if (radio_idle_ms_ == 0) radio_idle_ms_ = now;
            if (now - radio_idle_ms_ > 2000) stop_radio("not needed");
        } else {
            radio_idle_ms_ = 0;
            const Cap cap = hw_cap_probe(false);
            if (legacy_cap_) {
                // The expander answers after all: this is the newer cap, taken
                // for the older one because it was slow to wake. Its antenna
                // switch is still off.
                if (cap == Cap::Present) {
                    logf("IO expander found, switching the antenna on");
                    legacy_cap_ = false;
                    hw_power(true);
                }
            } else {
                // One missed answer can be a bus glitch; three in a row means the cap is gone.
                cap_misses_ = cap == Cap::Absent ? cap_misses_ + 1 : 0;
                if (cap_misses_ >= 3) stop_radio("LoRa cap removed");
            }
        }
    } else if (!wanted) {
        status = "stopped";
    } else {
        if (asked) radio_retry_ms_ = 0;  // somebody just opened the app: try now
        const Cap cap = hw_cap_probe(false, false);
        if (cap == Cap::Absent) {
            status = "nocap";
        } else if (now >= radio_retry_ms_) {
            legacy_cap_ = cap == Cap::Legacy;
            start_radio();
        }
    }

    if (status != radio_status_) {
        radio_status_ = status;
        if (FILE* f = fopen((history_dir() + "/radio-status").c_str(), "w")) {
            fprintf(f, "%s\n", status.c_str());
            fclose(f);
        }
        if (status == "nocap") logf("no LoRa cap attached, waiting for one");
    }
}

void Proxy::start_radio()
{
    hw_power(true, !legacy_cap_);
    const pid_t pid = fork();
    if (pid < 0) {
        logf("cannot start the radio daemon: %s", strerror(errno));
        radio_retry_ms_ = now_ms() + 10000;
        return;
    }
    if (pid == 0) {
        // The daemon keeps this node's private key, so its files stay private.
        umask(0077);
        signal(SIGPIPE, SIG_DFL);
        execl(radio_cmd_.c_str(), radio_cmd_.c_str(), static_cast<char*>(nullptr));
        fprintf(stderr, "meshzerod: cannot run %s: %s\n", radio_cmd_.c_str(), strerror(errno));
        _exit(127);
    }
    radio_pid_        = pid;
    radio_started_ms_ = now_ms();
    radio_stop_ms_    = 0;
    radio_idle_ms_    = 0;
    cap_misses_       = 0;
    logf("radio daemon started (pid %d)", static_cast<int>(pid));
}

void Proxy::stop_radio(const char* why)
{
    if (radio_pid_ <= 0 || radio_stop_ms_ != 0) return;
    logf("stopping the radio daemon: %s", why);
    kill(radio_pid_, SIGTERM);
    radio_stop_ms_ = now_ms();
}

void Proxy::reap_radio()
{
    if (radio_pid_ <= 0) return;
    int status = 0;
    if (waitpid(radio_pid_, &status, WNOHANG) != radio_pid_) return;
    const uint64_t now = now_ms();
    if (radio_stop_ms_ != 0) {
        // Stopped on purpose: nothing to wait for before the next start.
        radio_backoff_ms_ = 0;
        radio_retry_ms_   = 0;
        hw_power(false);
    } else {
        // It died. Retry, slower each time unless it had been running fine.
        if (now - radio_started_ms_ > 60000) radio_backoff_ms_ = 0;
        radio_backoff_ms_ = radio_backoff_ms_ == 0 ? 10000 : std::min<uint64_t>(radio_backoff_ms_ * 2, 120000);
        radio_retry_ms_   = now + radio_backoff_ms_;
        logf("radio daemon exited (status %d), next try in %llu s", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
             static_cast<unsigned long long>(radio_backoff_ms_ / 1000));
    }
    radio_pid_     = -1;
    radio_stop_ms_ = 0;
}

void Proxy::shutdown()
{
    if (radio_pid_ <= 0) return;
    stop_radio("service stopping");
    for (int i = 0; i < 50 && waitpid(radio_pid_, nullptr, WNOHANG) != radio_pid_; ++i) usleep(100000);
    if (waitpid(radio_pid_, nullptr, WNOHANG) == 0) {
        kill(radio_pid_, SIGKILL);
        waitpid(radio_pid_, nullptr, 0);
    }
    radio_pid_ = -1;
    hw_power(false);
}

// ---------------------------------------------------------------- control

void Proxy::send_control_line(Client& c, const std::string& setting)
{
    meshtastic_FromRadio msg  = meshtastic_FromRadio_init_zero;
    msg.which_payload_variant = meshtastic_FromRadio_packet_tag;
    meshtastic_MeshPacket& p  = msg.packet;
    p.which_payload_variant   = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum         = meshtastic_PortNum_PRIVATE_APP;
    const std::string text    = kControlPrefix + setting;
    if (text.size() > sizeof(p.decoded.payload.bytes)) return;
    memcpy(p.decoded.payload.bytes, text.data(), text.size());
    p.decoded.payload.size = static_cast<pb_size_t>(text.size());
    client_send(c, pb_encode_msg(meshtastic_FromRadio_fields, msg));
}

void Proxy::send_control(Client& c)
{
    // bt: -1 where this build cannot do Bluetooth. btok: phones can find it now.
    for (const std::string& setting :
         {std::string("net=") + (net_access_ ? "1" : "0"), std::string("bg=") + (background_ ? "1" : "0"),
          std::string("bgask=") + (exists(background_ask_path()) ? "1" : "0"),
          std::string("bt=") + (!Ble::supported() ? "-1" : bt_enabled_ ? "1" : "0"),
          std::string("btpin=") + (bt_pin_ ? "1" : "0"), "btname=" + bt_name_,
          std::string("btok=") + (ble_.advertising() ? "1" : "0"), "btwhy=" + bt_why_, "btkey=" + bt_passkey_,
          "btconn=" + std::to_string(bt_clients_), "btpaired=" + std::to_string(bt_paired_), "btpeer=" + bt_peer_,
          "netconn=" + std::to_string(net_clients_),
          // Seconds left for a new phone to pair, 0 when none may.
          "btpair=" + std::to_string(bt_pair_until_ms_ > now_ms() ? (bt_pair_until_ms_ - now_ms() + 999) / 1000 : 0)}) {
        send_control_line(c, setting);
    }
}

void Proxy::tell_control_clients()
{
    for (auto& c : clients_) {
        if (c->wants_control) send_control(*c);
    }
}

// ---------------------------------------------------------------- bluetooth

// Lets a phone that is not paired yet pair for the next two minutes, or ends that.
void Proxy::set_pairing_window(bool open)
{
    if (open && !ble_.running()) return;
    if (!open && bt_pair_until_ms_ == 0) return;
    bt_pair_until_ms_ = open ? now_ms() + kPairingMs : 0;
    ble_.set_pairing(open);
    logf(open ? "bluetooth: a new phone can pair for the next %d s" : "bluetooth: pairing closed",
         static_cast<int>(kPairingMs / 1000));
    apps_dirty_ = true;
    tell_control_clients();
}

// The radio daemon has no Bluetooth of its own and reports it as off. The
// phone apps show that setting, so they are told what is true here instead.
void Proxy::patch_bluetooth_config(meshtastic_Config_BluetoothConfig& bt) const
{
    bt.enabled   = bt_enabled_;
    bt.mode      = bt_pin_ ? meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN
                           : meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;
    bt.fixed_pin = 0;
}

// Works out who is connected and paired after something changed, and tells
// the app when that is different from what it was last told.
void Proxy::watch_apps()
{
    if (!apps_dirty_) return;
    apps_dirty_ = false;
    int bt = 0, net = 0, paired = 0;
    std::string peer;
    std::vector<std::string> phones = read_phones();
    const size_t known              = phones.size();
    std::vector<Ble::Device> devices;
    if (bt_enabled_) devices = Ble::devices();
    for (auto& c : clients_) {
        if (c->closing) continue;
        if (c->ble.empty()) {
            if (!c->local) ++net;
            continue;
        }
        ++bt;
        for (const Ble::Device& d : devices) {
            if (d.path != c->ble) continue;
            if (peer.empty()) peer = d.name;
            // Remember the phone, so that its pairing can be told from others.
            if (d.paired && std::find(phones.begin(), phones.end(), d.address) == phones.end()) phones.push_back(d.address);
        }
    }
    if (phones.size() != known) write_phones(phones);
    for (const Ble::Device& d : devices) {
        if (d.paired && std::find(phones.begin(), phones.end(), d.address) != phones.end()) ++paired;
    }
    if (bt == bt_clients_ && net == net_clients_ && paired == bt_paired_ && peer == bt_peer_) return;
    // A phone has come in: that was what the pairing was opened for.
    const bool arrived = bt > bt_clients_ && bt_pair_until_ms_ != 0;
    bt_clients_        = bt;
    net_clients_       = net;
    bt_paired_         = paired;
    bt_peer_           = peer;
    if (arrived) {
        set_pairing_window(false);  // tells the apps as well
    } else {
        tell_control_clients();
    }
}

// What a radio with Bluetooth of its own calls itself: the short name and the
// last four digits of the node number.
std::string Proxy::bluetooth_name()
{
    std::string short_name;
    const auto own = cache_.nodes.find(cache_.my_num);
    if (own != cache_.nodes.end()) {
        meshtastic_FromRadio msg = meshtastic_FromRadio_init_zero;
        if (pb_decode_msg(meshtastic_FromRadio_fields, own->second, msg) &&
            msg.which_payload_variant == meshtastic_FromRadio_node_info_tag) {
            for (const char* p = msg.node_info.user.short_name; *p; ++p) {
                const bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
                if (plain) short_name.push_back(*p);
            }
        }
    }
    if (short_name.empty()) short_name = "Meshtastic";
    char tail[8];
    snprintf(tail, sizeof(tail), "_%04x", cache_.my_num & 0xffff);
    return short_name + tail;
}

Client& Proxy::ble_client(const std::string& device)
{
    for (auto& c : clients_) {
        if (c->ble == device && !c->closing) return *c;
    }
    auto c       = std::make_unique<Client>();
    c->ble       = device;
    // ".../dev_AA_BB_CC_DD_EE_FF" names the phone by its address.
    const size_t slash = device.rfind('/');
    c->peer            = "bluetooth " + (slash == std::string::npos ? device : device.substr(slash + 1));
    c->connected       = now_ms();
    logf("client connected: %s", c->peer.c_str());
    const bool radio_down = upstream_fd_ < 0 || upstream_connecting_;
    clients_.push_back(std::move(c));
    apps_dirty_ = true;
    if (radio_down) watch_radio(true);
    return *clients_.back();
}

// Offers the service while Bluetooth is switched on, and starts it over when
// the name or the PIN setting changed.
void Proxy::watch_bluetooth()
{
    if (cache_.valid) {
        const std::string name = bluetooth_name();
        if (name != bt_name_) {
            bt_name_ = name;
            if (FILE* f = fopen(bluetooth_name_path().c_str(), "w")) {
                fprintf(f, "%s\n", name.c_str());
                fclose(f);
            }
        }
    }
    const bool want = bt_enabled_ && Ble::supported() && !bt_name_.empty();
    if (ble_.running() && (!want || bt_started_name_ != bt_name_ || bt_started_pin_ != bt_pin_)) {
        ble_.stop();
        for (auto& c : clients_) {
            if (!c->ble.empty()) c->closing = c->ble_gone = true;
        }
        if (!want) logf("bluetooth off");
    }
    if (want && !ble_.running() && now_ms() >= bt_retry_ms_) {
        Ble::Handlers h;
        h.on_write = [this](const std::string& device, const std::string& payload) {
            on_to_radio(ble_client(device), payload);
        };
        h.on_read = [this](const std::string& device) {
            Client& c = ble_client(device);
            if (c.ble_out.empty()) return std::string();
            std::string next = std::move(c.ble_out.front());
            c.ble_out.pop_front();
            return next;
        };
        h.on_disconnect = [this](const std::string& device) {
            for (auto& c : clients_) {
                if (c->ble == device) c->closing = c->ble_gone = true;
            }
        };
        h.on_passkey = [this](const std::string& passkey) {
            // The code itself stays out of the log.
            bt_passkey_ = passkey;
            apps_dirty_ = true;
            logf(passkey.empty() ? "bluetooth pairing over" : "bluetooth pairing: showing the code in the app");
            for (auto& c : clients_) {
                if (c->wants_control) send_control_line(*c, "btkey=" + bt_passkey_);
            }
        };
        bt_started_name_ = bt_name_;
        bt_started_pin_  = bt_pin_;
        if (ble_.start(bt_name_, bt_pin_, std::move(h))) {
            logf("bluetooth on as %s, %s", bt_name_.c_str(), bt_pin_ ? "pairing with a code" : "pairing without a code");
        } else {
            bt_retry_ms_ = now_ms() + 10000;
        }
    }
    if (ble_.running()) {
        if (exists(bluetooth_pair_path())) {
            unlink(bluetooth_pair_path().c_str());
            set_pairing_window(true);
        }
        if (bt_pair_until_ms_ != 0 && now_ms() >= bt_pair_until_ms_) set_pairing_window(false);
        // Phones only need to find it to pair, or to come back once paired.
        ble_.set_advertising(bt_pair_until_ms_ != 0 || bt_paired_ > 0);
    } else if (bt_pair_until_ms_ != 0) {
        bt_pair_until_ms_ = 0;
    }
    const std::string why = want ? ble_.why() : "";
    const bool on_air     = ble_.advertising();
    if (why != bt_why_ || on_air != bt_reported_) {
        if (!why.empty() && why != bt_why_) logf("bluetooth: %s", why.c_str());
        if (on_air != bt_reported_) {
            logf(on_air ? "bluetooth: phones can find %s now" : "bluetooth: no longer advertising", bt_name_.c_str());
        }
        bt_why_      = why;
        bt_reported_ = on_air;
        tell_control_clients();
    }
}

void Proxy::on_control(Client& c, const std::string& request)
{
    c.wants_control = true;
    if (request.rfind("clear=", 0) == 0) {
        clear_history(request.substr(6));
        return;
    }
    if (request == "net=1" || request == "net=0") {
        const bool on = request == "net=1";
        // Other apps come in over the network or over Bluetooth, not both.
        if (on && bt_enabled_) {
            write_flag(bluetooth_path(), false);
            bt_enabled_ = false;
            watch_bluetooth();
        }
        if (!write_network_access(on)) logf("cannot save %s: %s", access_path().c_str(), strerror(errno));
        if (on != net_access_) {
            apply_network_access(on);  // tells every client that asked, this one included
            return;
        }
    }
    if (request == "btpair=1" || request == "btpair=0") {
        set_pairing_window(request.back() == '1');
        return;
    }
    if (request == "btforget") {
        const int count = forget_phones();
        logf("bluetooth: forgot the pairing with %d phone%s", count, count == 1 ? "" : "s");
        for (auto& other : clients_) {
            if (!other->ble.empty()) other->closing = true;
        }
        apps_dirty_ = true;
        return;
    }
    if (request == "bt=1" || request == "bt=0" || request == "btpin=1" || request == "btpin=0") {
        apps_dirty_ = true;
        const bool on = request.back() == '1';
        const bool pin = request[2] == 'p';
        const std::string path = pin ? bluetooth_pin_path() : bluetooth_path();
        if (!write_flag(path, on)) logf("cannot save %s: %s", path.c_str(), strerror(errno));
        (pin ? bt_pin_ : bt_enabled_) = on;
        if (resync_at_ms_ == 0) resync_at_ms_ = now_ms() + 500;
        if (!pin && on) {
            write_network_access(false);
            apply_network_access(false);
        }
        watch_bluetooth();
        for (auto& other : clients_) {
            if (other->wants_control && other.get() != &c) send_control(*other);
        }
    }
    if (request == "bg=1" || request == "bg=0") {
        background_ = request == "bg=1";
        if (!write_background(background_)) logf("cannot save %s: %s", background_path().c_str(), strerror(errno));
        logf("running in the background %s", background_ ? "on" : "off");
        watch_radio(true);
        for (auto& other : clients_) {
            if (other->wants_control && other.get() != &c) send_control(*other);
        }
    }
    send_control(c);
}

void Proxy::apply_network_access(bool on)
{
    if (on == net_access_) return;
    net_access_ = on;
    logf("network access %s", on ? "on" : "off");
    if (!start_listening()) {
        // Fall back to this device only so the app keeps working.
        net_access_ = false;
        start_listening();
    }
    for (auto& c : clients_) {
        if (!net_access_ && !c->local && c->ble.empty()) c->closing = true;
        if (c->wants_control) send_control(*c);
    }
}

void Proxy::client_read(Client& c)
{
    uint8_t buf[2048];
    while (c.fd >= 0 && !c.closing) {
        const ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.parser.feed(buf, static_cast<size_t>(n), [this, &c](const std::string& p) { on_to_radio(c, p); });
        } else if (n == 0) {
            c.closing = true;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else if (errno != EINTR) {
            c.closing = true;
        }
    }
}

// ----------------------------------------------------------------- history

void Proxy::record_outgoing(const meshtastic_MeshPacket& p)
{
    StoredMsg m;
    m.ts       = now_s();
    m.id       = p.id;
    m.from     = cache_.my_num;
    m.to       = p.to == 0 ? kBroadcast : p.to;
    m.channel  = p.channel;
    m.outgoing = true;
    m.pki      = p.pki_encrypted;
    m.emoji    = p.decoded.emoji != 0;
    m.reply_id = p.decoded.reply_id;
    m.ack      = AckState::Pending;
    m.text.assign(reinterpret_cast<const char*>(p.decoded.payload.bytes), p.decoded.payload.size);
    history_.append(m);
    // Forget only the oldest entries: clearing the whole map would leave every
    // recent message waiting on an acknowledgement that can no longer match.
    if (outgoing_dest_.find(p.id) == outgoing_dest_.end()) outgoing_order_.push_back(p.id);
    outgoing_dest_[p.id] = m.to;
    while (outgoing_order_.size() > kMaxTrackedOutgoing) {
        outgoing_dest_.erase(outgoing_order_.front());
        outgoing_order_.pop_front();
    }
}

void Proxy::record_incoming(const meshtastic_MeshPacket& p)
{
    if (p.which_payload_variant != meshtastic_MeshPacket_decoded_tag) return;
    if (p.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
        if (p.from == cache_.my_num) return;
        StoredMsg m;
        m.ts       = p.rx_time ? p.rx_time : now_s();
        m.id       = p.id;
        m.from     = p.from;
        m.to       = p.to;
        m.channel  = p.channel;
        m.pki      = p.pki_encrypted;
        m.emoji    = p.decoded.emoji != 0;
        m.reply_id = p.decoded.reply_id;
        m.snr      = p.rx_snr;
        m.rssi     = p.rx_rssi;
        m.hops     = p.hop_start >= p.hop_limit && p.hop_start != 0 ? static_cast<int>(p.hop_start - p.hop_limit) : -1;
        m.text.assign(reinterpret_cast<const char*>(p.decoded.payload.bytes), p.decoded.payload.size);
        history_.append(m);
    } else if (p.decoded.portnum == meshtastic_PortNum_ROUTING_APP && p.decoded.request_id != 0) {
        const auto it = outgoing_dest_.find(p.decoded.request_id);
        if (it == outgoing_dest_.end()) return;
        meshtastic_Routing routing = meshtastic_Routing_init_zero;
        if (!pb_decode_msg(meshtastic_Routing_fields, p.decoded.payload.bytes, p.decoded.payload.size, routing)) return;
        const int error =
            routing.which_variant == meshtastic_Routing_error_reason_tag ? static_cast<int>(routing.error_reason) : 0;
        AckState state;
        if (error != 0) {
            state = AckState::Failed;
        } else if (p.from == it->second) {
            state = AckState::Acked;
        } else {
            state = AckState::Implicit;
        }
        history_.append_ack(p.decoded.request_id, state, error);
    }
}

// -------------------------------------------------------------------- loop

void Proxy::run()
{
    while (!g_stop) {
        const uint64_t now = now_ms();
        if (now - last_access_check_ms_ > 1000) {
            // 'meshzerod network on|off' changes the setting from a shell.
            last_access_check_ms_ = now;
            apply_network_access(read_network_access());
            background_ = read_background();
            if (listen_fds_.empty()) start_listening();
            // 'meshzerod bluetooth on|off' does the same for Bluetooth.
            // With both switched on, as only editing the files can do, the network stays.
            const bool was_on = bt_enabled_, was_pin = bt_pin_;
            bt_enabled_ = read_flag(bluetooth_path(), false) && !net_access_;
            bt_pin_     = read_flag(bluetooth_pin_path(), true);
            if (bt_enabled_ != was_on || bt_pin_ != was_pin) {
                apps_dirty_ = true;
                // What the phone apps are told about Bluetooth has changed with it.
                if (resync_at_ms_ == 0) resync_at_ms_ = now + 500;
            }
            watch_bluetooth();
        }
        watch_apps();
        if (now - last_radio_watch_ms_ > (radio_status_ == "nocap" || (radio_pid_ > 0 && radio_stop_ms_ == 0 && background_) ? 5000 : 1000)) {
            last_radio_watch_ms_ = now;
            watch_radio(false);
        }
        if (upstream_fd_ < 0 && now >= next_connect_ms_) {
            upstream_connect();
            if (upstream_fd_ < 0) next_connect_ms_ = now + kReconnectMs;
        }
        if (upstream_fd_ >= 0 && !upstream_connecting_) {
            if (syncing_ && now - sync_start_ms_ > kSyncTimeoutMs) {
                upstream_close("configuration sync timed out");
            } else if (!cache_.valid && !syncing_) {
                start_sync();
            } else if (resync_at_ms_ != 0 && now >= resync_at_ms_ && !syncing_) {
                resync_at_ms_  = 0;
                sync_announce_ = true;
                start_sync();
            } else if (now - last_heartbeat_ms_ > kHeartbeatMs) {
                last_heartbeat_ms_        = now;
                meshtastic_ToRadio hb     = meshtastic_ToRadio_init_zero;
                hb.which_payload_variant  = meshtastic_ToRadio_heartbeat_tag;
                upstream_send(pb_encode_msg(meshtastic_ToRadio_fields, hb));
            }
        }

        std::vector<pollfd> fds;
        for (int fd : listen_fds_) fds.push_back({fd, POLLIN, 0});
        const size_t listeners = fds.size();
        if (upstream_fd_ >= 0) {
            short events = POLLIN;
            if (upstream_connecting_ || !upstream_out_.empty()) events |= POLLOUT;
            fds.push_back({upstream_fd_, events, 0});
        }
        const size_t first_client = fds.size();
        for (auto& c : clients_) {
            fds.push_back({c->fd, static_cast<short>(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0});
        }
        const size_t polled_clients = clients_.size();
        int wait_ms                 = 250;
        if (ble_.running()) {
            fds.push_back({ble_.fd(), ble_.events(), 0});
            const int bus_ms = ble_.timeout_ms();
            if (bus_ms >= 0 && bus_ms < wait_ms) wait_ms = bus_ms;
        }

        if (poll(fds.data(), fds.size(), wait_ms) < 0 && errno != EINTR) break;

        for (size_t i = 0; i < listeners; ++i) {
            if (fds[i].revents & POLLIN) accept_client(fds[i].fd);
        }

        if (first_client == listeners + 1 && upstream_fd_ == fds[listeners].fd) {
            const short re = fds[listeners].revents;
            if (upstream_connecting_) {
                if (re & (POLLOUT | POLLERR | POLLHUP)) {
                    int err       = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(upstream_fd_, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err != 0) {
                        upstream_close("connect failed");
                    } else {
                        upstream_connecting_ = false;
                        last_heartbeat_ms_   = now_ms();
                        logf("connected to radio daemon");
                        start_sync();
                    }
                }
            } else {
                if (re & POLLIN) upstream_read();
                if (upstream_fd_ >= 0 && (re & POLLOUT)) upstream_flush();
                if (upstream_fd_ >= 0 && (re & (POLLERR | POLLHUP)) && !(re & POLLIN)) upstream_close("hangup");
            }
        }

        for (size_t i = 0; i < polled_clients; ++i) {
            Client& c      = *clients_[i];
            const short re = fds[first_client + i].revents;
            if (re & POLLIN) client_read(c);
            if (re & POLLOUT) client_flush(c);
            if ((re & (POLLERR | POLLHUP | POLLNVAL)) && !(re & POLLIN)) c.closing = true;
        }

        // Phones on Bluetooth: their reads and writes, and telling them of what
        // the steps above queued for them.
        ble_.process();

        for (auto it = clients_.begin(); it != clients_.end();) {
            Client& c = **it;
            if (c.closing) {
                if (!c.out.empty()) client_flush(c);
                logf("client disconnected: %s", c.peer.c_str());
                if (!c.ble.empty() && !c.ble_gone) ble_.disconnect(c.ble);
                if (c.fd >= 0) close(c.fd);
                apps_dirty_ = true;
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& c : clients_) {
        if (c->fd >= 0) close(c->fd);
    }
    ble_.stop();
    if (upstream_fd_ >= 0) close(upstream_fd_);
    for (int fd : listen_fds_) close(fd);
}

void on_signal(int)
{
    g_stop = 1;
}

void usage()
{
    fprintf(stderr,
            "meshzerod - API multiplexer and hardware helper for MeshZero\n\n"
            "  meshzerod [serve] [--listen ADDR] [--port N] [--radio-host H] [--radio-port N] [--radio CMD] [-v]\n"
            "                                  --radio CMD: also run the radio daemon while the cap is attached\n"
            "  meshzerod network on|off|status let other devices on the network use the API\n"
            "  meshzerod hw on|off|status      control power and antenna switch of the LoRa cap\n"
            "  meshzerod hw present            exit 1 when no LoRa cap is attached\n"
            "  meshzerod background on|off|status keep the radio running when no app is using it (off by\n"
            "                                  default; while on, other apps cannot use the LoRa cap)\n"
            "  meshzerod bluetooth on|off|status|pair|forget\n"
            "                                  let phones use the radio over Bluetooth (early access);\n"
            "                                  pair lets a new phone pair for two minutes, forget drops\n"
            "                                  the pairing with every phone\n"
            "  meshzerod doctor                check that everything the radio needs is present\n\n"
            "Environment: MESHZERO_API_BIND, MESHZERO_API_PORT, MESHZERO_RADIO_PORT, MESHZERO_STATE_DIR\n");
}

}  // namespace

int main(int argc, char** argv)
{
    std::string bind_addr  = env_or("MESHZERO_API_BIND", "0.0.0.0");
    int listen_port        = atoi(env_or("MESHZERO_API_PORT", "4403"));
    std::string radio_host = env_or("MESHZERO_RADIO_HOST", "127.0.0.1");
    int radio_port         = atoi(env_or("MESHZERO_RADIO_PORT", "4404"));
    std::string radio_cmd;

    int i = 1;
    if (i < argc && argv[i][0] != '-') {
        const std::string cmd = argv[i++];
        if (cmd == "hw") {
            const std::string action = i < argc ? argv[i] : "status";
            if (action == "on") return hw_power(true) ? 0 : 1;
            if (action == "off") return hw_power(false) ? 0 : 1;
            if (action == "present") {
                const Cap cap = hw_cap_check(radio_port);
                printf("LoRa cap: %s\n", hw_cap_name(cap));
                return cap == Cap::Absent ? 1 : 0;
            }
            return hw_status();
        }
        if (cmd == "background") {
            const std::string action = i < argc ? argv[i] : "status";
            if ((action == "on" || action == "off") && !write_background(action == "on")) {
                fprintf(stderr, "cannot write %s: %s\nRun it as the service user: sudo -u meshzero meshzerod background %s\n",
                        background_path().c_str(), strerror(errno), action.c_str());
                return 1;
            }
            printf("Run in background: %s\n", read_background() ? "on (other apps cannot use the LoRa cap)"
                                                                : "off (radio stops when no app is using it)");
            return 0;
        }
        if (cmd == "network") {
            const std::string action = i < argc ? argv[i] : "status";
            if ((action == "on" || action == "off") && !write_network_access(action == "on")) {
                fprintf(stderr, "cannot write %s: %s\nRun it as the service user: sudo -u meshzero meshzerod network %s\n",
                        access_path().c_str(), strerror(errno), action.c_str());
                return 1;
            }
            if (action == "on" && read_flag(bluetooth_path(), false) && write_flag(bluetooth_path(), false)) {
                printf("Bluetooth: turned off, as only one of the two is on at a time\n");
            }
            printf("Network access: %s\n", read_network_access() ? "on" : "off (this device only)");
            return 0;
        }
        if (cmd == "bluetooth") {
            const std::string action = i < argc ? argv[i] : "status";
            if (!Ble::supported()) {
                printf("Bluetooth: not in this build\n");
                return action == "status" ? 0 : 1;
            }
            mkdir(history_dir().c_str(), 0755);
            if ((action == "on" || action == "off") && !write_flag(bluetooth_path(), action == "on")) {
                fprintf(stderr, "cannot write %s: %s\nRun it as the service user: sudo -u meshzero meshzerod bluetooth %s\n",
                        bluetooth_path().c_str(), strerror(errno), action.c_str());
                return 1;
            }
            if (action == "on" && read_network_access() && write_network_access(false)) {
                printf("Network access: turned off, as only one of the two is on at a time\n");
            }
            if (action == "pair") {
                if (!read_flag(bluetooth_path(), false)) {
                    printf("Bluetooth is off. Turn it on first: meshzerod bluetooth on\n");
                    return 1;
                }
                if (!write_flag(bluetooth_pair_path(), true)) {
                    fprintf(stderr, "cannot write %s: %s\n", bluetooth_pair_path().c_str(), strerror(errno));
                    return 1;
                }
                printf("A new phone can pair for the next %d seconds.\n", static_cast<int>(kPairingMs / 1000));
                return 0;
            }
            if (action == "forget") {
                const int count = forget_phones();
                printf("Forgot the pairing with %d phone%s. Forget this device on the phone too before pairing again.\n",
                       count, count == 1 ? "" : "s");
                return 0;
            }
            const std::string name = read_line(bluetooth_name_path());
            printf("Bluetooth: %s\n", !read_flag(bluetooth_path(), false) ? "off"
                                       : read_flag(bluetooth_pin_path(), true)
                                             ? "on, phones pair with a code shown in the app"
                                             : "on, phones pair without a code");
            if (!name.empty()) printf("Name: %s\n", name.c_str());
            for (const Ble::Device& d : paired_phones()) {
                printf("Paired phone: %s (%s)%s\n", d.name.c_str(), d.address.c_str(), d.connected ? ", connected" : "");
            }
            return 0;
        }
        if (cmd == "doctor") return hw_doctor(radio_port, listen_port);
        if (cmd != "serve") {
            usage();
            return 2;
        }
    }
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next     = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--listen") {
            bind_addr = next();
        } else if (a == "--port") {
            listen_port = atoi(next());
        } else if (a == "--radio-host") {
            radio_host = next();
        } else if (a == "--radio") {
            radio_cmd = next();
        } else if (a == "--radio-port") {
            radio_port = atoi(next());
        } else if (a == "-v") {
            g_verbose = 1;
        } else {
            usage();
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    mkdir(history_dir().c_str(), 0755);
    migrate_background();

    Proxy proxy(radio_host, static_cast<uint16_t>(radio_port), bind_addr, static_cast<uint16_t>(listen_port));
    proxy.set_network_access(read_network_access());
    proxy.set_radio_command(radio_cmd);
    proxy.load_bluetooth();
    if (!proxy.start_listening()) return 1;
    proxy.run();
    proxy.shutdown();
    return 0;
}
