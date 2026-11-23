// SPDX-License-Identifier: GPL-3.0-only
#include "ui/mesh.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/util.h"

namespace mz {
namespace {

constexpr uint64_t kReconnectMs   = 1500;
constexpr uint64_t kSyncTimeoutMs = 45000;
constexpr uint64_t kHeartbeatMs   = 30000;
constexpr size_t kMaxChatMsgs     = 500;
constexpr size_t kMaxPacketLog    = 200;
// Automatic requests for a node's info. A node ignores one when it sent its
// info less than five minutes ago, hence the first retry. Each further one
// waits twice as long.
constexpr uint64_t kInfoRetryMs    = 6 * 60 * 1000;
constexpr uint64_t kInfoRetryMaxMs = 60 * 60 * 1000;
// Requests before giving up on a node. After that it is asked at most once an
// hour, and only while something still calls for it.
constexpr int kInfoTries        = 4;
constexpr int kKeyPatience      = 3;      // unanswered requests before a chat stops saying it is waiting
constexpr uint64_t kInfoGapMs   = 10000;  // between any two automatic requests, to any nodes
// After a message from a node we know nothing about, the radio asks it once by
// itself; give that answer time to arrive first.
constexpr uint64_t kInfoGraceMs = 30000;
// Channel utilisation (percent) above which only requests someone is waiting
// on go out, the level at which the firmware holds back its own.
constexpr float kBusyChannelPct    = 25.0f;
constexpr size_t kChannelAskMax    = 8;          // unknown senders asked about when a channel is opened
constexpr uint32_t kChannelAskAgeS = 2 * 3600;   // ... if heard within this long

uint64_t info_backoff(int tries)
{
    if (tries <= 0) return 0;
    const uint64_t wait = kInfoRetryMs << (tries - 1 < 4 ? tries - 1 : 4);
    return wait < kInfoRetryMaxMs ? wait : kInfoRetryMaxMs;
}

int hops_of(const meshtastic_MeshPacket& p)
{
    return p.hop_start != 0 && p.hop_start >= p.hop_limit ? static_cast<int>(p.hop_start - p.hop_limit) : -1;
}

}  // namespace

Mesh::Mesh()
{
    std::random_device rd;
    id_seq_ = rd();
    load_read_state();
}

Mesh::~Mesh()
{
    if (fd_ >= 0) close(fd_);
}

void Mesh::configure(const std::string& host, uint16_t port)
{
    host_ = host;
    port_ = port;
}

uint32_t Mesh::next_id()
{
    // Random high bits with a rolling counter, like the reference clients.
    static std::mt19937 rng(std::random_device{}());
    id_seq_          = (id_seq_ + 1) & 0x3ff;
    const uint32_t id = (static_cast<uint32_t>(rng()) << 10) | id_seq_;
    return id == 0 ? 1 : id;
}

void Mesh::set_state(LinkState s)
{
    if (state_ == s) return;
    state_ = s;
    ++rev_link;
    UiEvent ev;
    ev.type = UiEvent::LinkChanged;
    events.push_back(ev);
}

std::string Mesh::radio_note()
{
    if (ready()) return "";
    const uint64_t now = now_ms();
    if (cap_checked_ms_ == 0 || now - cap_checked_ms_ > 1000) {
        cap_checked_ms_ = now;
        std::ifstream in(history_dir() + "/radio-status");
        radio_note_.clear();
        std::getline(in, radio_note_);
    }
    return radio_note_;
}

std::string Mesh::offline_text()
{
    const std::string note = radio_note();
    if (note == "nocap") return "no LoRa cap";
    if (note == "starting" || note == "stopped") return "starting radio";
    return "radio offline";
}

void Mesh::toast(const std::string& text)
{
    UiEvent ev;
    ev.type = UiEvent::Toast;
    ev.text = text;
    events.push_back(ev);
}

// --------------------------------------------------------------- transport

void Mesh::start_connect()
{
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res     = nullptr;
    if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0 || !res) {
        next_connect_ms_ = now_ms() + kReconnectMs;
        return;
    }
    fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ >= 0) {
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        fcntl(fd_, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (connect(fd_, res->ai_addr, res->ai_addrlen) < 0 && errno != EINPROGRESS) {
            close(fd_);
            fd_ = -1;
        }
    }
    freeaddrinfo(res);
    if (fd_ < 0) {
        next_connect_ms_ = now_ms() + kReconnectMs;
        return;
    }
    parser_.reset();
    out_.clear();
    set_state(LinkState::Connecting);
}

void Mesh::disconnect(const char* why)
{
    (void)why;
    if (fd_ >= 0) close(fd_);
    fd_              = -1;
    next_connect_ms_ = now_ms() + kReconnectMs;
    set_state(LinkState::Disconnected);
}

void Mesh::send_to_radio(const meshtastic_ToRadio& msg)
{
    if (fd_ < 0 || state_ == LinkState::Connecting || state_ == LinkState::Disconnected) return;
    out_ += frame_wrap(pb_encode_msg(meshtastic_ToRadio_fields, msg));
}

void Mesh::send_control(const std::string& request)
{
    meshtastic_ToRadio msg    = meshtastic_ToRadio_init_zero;
    msg.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket& p  = msg.packet;
    p.which_payload_variant   = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum         = meshtastic_PortNum_PRIVATE_APP;
    const std::string text    = "mz:" + request;
    if (text.size() > sizeof(p.decoded.payload.bytes)) return;
    memcpy(p.decoded.payload.bytes, text.data(), text.size());
    p.decoded.payload.size = static_cast<pb_size_t>(text.size());
    send_to_radio(msg);
}

void Mesh::delete_history(const ChatKey& key)
{
    send_control("clear=" + std::string(key.dm ? "d" : "c") + std::to_string(key.id));
    Chat& c   = chat(key);
    c.msgs.clear();
    c.unread  = 0;
    c.last_ts = 0;
    ++rev_chats;
}

void Mesh::delete_all_history()
{
    send_control("clear=all");
    for (auto& [key, c] : chats) {
        c.msgs.clear();
        c.unread  = 0;
        c.last_ts = 0;
    }
    ++rev_chats;
}

void Mesh::set_background(bool on)
{
    send_control(on ? "bg=1" : "bg=0");
}

void Mesh::set_bluetooth_pairing(bool open)
{
    send_control(open ? "btpair=1" : "btpair=0");
}

void Mesh::forget_bluetooth_phones()
{
    send_control("btforget");
}

int Mesh::app_connection() const
{
    if (network_access < 0) return -1;
    return bluetooth == 1 ? kAppBluetooth : network_access == 1 ? kAppWifi : kAppOff;
}

void Mesh::set_app_connection(int mode)
{
    // Switching one on makes the service switch the other off.
    if (mode == kAppWifi) {
        send_control("net=1");
    } else if (mode == kAppBluetooth) {
        send_control("bt=1");
    } else {
        send_control("net=0");
        if (bluetooth == 1) send_control("bt=0");
    }
}

void Mesh::set_bluetooth_pin(bool on)
{
    send_control(on ? "btpin=1" : "btpin=0");
}

void Mesh::request_config()
{
    static std::mt19937 rng(std::random_device{}());
    do {
        nonce_ = static_cast<uint32_t>(rng());
    } while (nonce_ == 0 || nonce_ == 69420 || nonce_ == 69421);
    meshtastic_ToRadio req    = meshtastic_ToRadio_init_zero;
    req.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    req.want_config_id        = nonce_;
    send_to_radio(req);
    sync_start_ms_ = now_ms();
}

void Mesh::resync()
{
    if (state_ == LinkState::Ready) request_config();
}

void Mesh::poll()
{
    const uint64_t now = now_ms();
    if (fd_ < 0) {
        if (now >= next_connect_ms_) start_connect();
        return;
    }

    pollfd pfd{fd_, POLLIN, 0};
    if (state_ == LinkState::Connecting || !out_.empty()) pfd.events |= POLLOUT;
    if (::poll(&pfd, 1, 0) < 0) return;

    if (state_ == LinkState::Connecting) {
        if (pfd.revents & (POLLOUT | POLLERR | POLLHUP)) {
            int err       = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) {
                disconnect("connect failed");
                return;
            }
            set_state(LinkState::Syncing);
            last_rx_ms_ = last_hb_ms_ = now;
            request_config();
        }
        return;
    }

    if (pfd.revents & POLLIN) {
        uint8_t buf[4096];
        while (fd_ >= 0) {
            const ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n > 0) {
                last_rx_ms_ = now;
                parser_.feed(buf, static_cast<size_t>(n), [this](const std::string& p) { on_frame(p); });
            } else if (n == 0) {
                disconnect("closed");
                return;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else if (errno != EINTR) {
                disconnect("read error");
                return;
            }
        }
    } else if (pfd.revents & (POLLERR | POLLHUP)) {
        disconnect("hangup");
        return;
    }

    if (state_ == LinkState::Syncing && now - sync_start_ms_ > kSyncTimeoutMs) {
        disconnect("sync timeout");
        return;
    }
    if (now - last_hb_ms_ > kHeartbeatMs) {
        last_hb_ms_              = now;
        meshtastic_ToRadio hb    = meshtastic_ToRadio_init_zero;
        hb.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
        send_to_radio(hb);
    }

    if (has_active_ && active_.dm) want_info(active_.id, true, true);
    pump_info_asks();

    while (!out_.empty() && fd_ >= 0) {
        const ssize_t n = send(fd_, out_.data(), out_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out_.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            disconnect("write error");
            return;
        }
    }
}

// ----------------------------------------------------------------- frames

void Mesh::on_frame(const std::string& payload)
{
    meshtastic_FromRadio msg = meshtastic_FromRadio_init_zero;
    if (!pb_decode_msg(meshtastic_FromRadio_fields, payload, msg)) return;

    switch (msg.which_payload_variant) {
        case meshtastic_FromRadio_my_info_tag:
            if (my_num != 0 && my_num != msg.my_info.my_node_num) {
                // A different radio identity: everything cached belongs to someone else.
                nodes.clear();
                sensors.clear();
                chats.clear();
                info_asks_.clear();
                history_loaded_ = false;
            }
            my_info = msg.my_info;
            my_num  = msg.my_info.my_node_num;
            ++rev_nodes;
            break;
        case meshtastic_FromRadio_metadata_tag:
            metadata     = msg.metadata;
            has_metadata = true;
            ++rev_config;
            break;
        case meshtastic_FromRadio_node_info_tag:
            nodes[msg.node_info.num] = msg.node_info;
            ++rev_nodes;
            ++rev_chats;  // chats show the names
            break;
        case meshtastic_FromRadio_channel_tag:
            if (msg.channel.index >= 0 && msg.channel.index < kMaxChannels) {
                channels[msg.channel.index] = msg.channel;
                ensure_channel_chats();
                ++rev_config;
                ++rev_chats;
            }
            break;
        case meshtastic_FromRadio_config_tag: {
            const meshtastic_Config& c = msg.config;
            switch (c.which_payload_variant) {
                case meshtastic_Config_device_tag:
                    config.has_device = true;
                    config.device     = c.payload_variant.device;
                    break;
                case meshtastic_Config_position_tag:
                    config.has_position = true;
                    config.position     = c.payload_variant.position;
                    break;
                case meshtastic_Config_power_tag:
                    config.has_power = true;
                    config.power     = c.payload_variant.power;
                    break;
                case meshtastic_Config_network_tag:
                    config.has_network = true;
                    config.network     = c.payload_variant.network;
                    break;
                case meshtastic_Config_display_tag:
                    config.has_display = true;
                    config.display     = c.payload_variant.display;
                    break;
                case meshtastic_Config_lora_tag:
                    config.has_lora = true;
                    config.lora     = c.payload_variant.lora;
                    break;
                case meshtastic_Config_bluetooth_tag:
                    config.has_bluetooth = true;
                    config.bluetooth     = c.payload_variant.bluetooth;
                    break;
                case meshtastic_Config_security_tag:
                    config.has_security = true;
                    config.security     = c.payload_variant.security;
                    break;
                default:
                    break;
            }
            ++rev_config;
            break;
        }
        case meshtastic_FromRadio_moduleConfig_tag: {
            const meshtastic_ModuleConfig& c = msg.moduleConfig;
            switch (c.which_payload_variant) {
                case meshtastic_ModuleConfig_mqtt_tag:
                    modules.has_mqtt = true;
                    modules.mqtt     = c.payload_variant.mqtt;
                    break;
                case meshtastic_ModuleConfig_telemetry_tag:
                    modules.has_telemetry = true;
                    modules.telemetry     = c.payload_variant.telemetry;
                    break;
                case meshtastic_ModuleConfig_neighbor_info_tag:
                    modules.has_neighbor_info = true;
                    modules.neighbor_info     = c.payload_variant.neighbor_info;
                    break;
                case meshtastic_ModuleConfig_store_forward_tag:
                    modules.has_store_forward = true;
                    modules.store_forward     = c.payload_variant.store_forward;
                    break;
                case meshtastic_ModuleConfig_range_test_tag:
                    modules.has_range_test = true;
                    modules.range_test     = c.payload_variant.range_test;
                    break;
                default:
                    break;
            }
            ++rev_config;
            break;
        }
        case meshtastic_FromRadio_config_complete_id_tag:
            if (msg.config_complete_id == nonce_) {
                ensure_channel_chats();
                if (!history_loaded_) load_history();
                ever_ready_ = true;
                set_state(LinkState::Ready);
                send_control("net?");
                ++rev_nodes;
                ++rev_chats;
                ++rev_config;
            }
            break;
        case meshtastic_FromRadio_packet_tag:
            on_packet(msg.packet);
            break;
        case meshtastic_FromRadio_rebooted_tag:
            toast("Radio restarted");
            break;
        case meshtastic_FromRadio_clientNotification_tag:
            if (msg.clientNotification.message[0]) toast(msg.clientNotification.message);
            break;
        default:
            break;
    }
}

void Mesh::ensure_channel_chats()
{
    for (int i = 0; i < kMaxChannels; ++i) {
        if (channel_enabled(i)) chat(ChatKey{false, static_cast<uint32_t>(i)});
    }
}

// ---------------------------------------------------------------- packets

void Mesh::log_packet(const meshtastic_MeshPacket& p, bool outgoing)
{
    PacketLogEntry e;
    e.ts       = p.rx_time ? p.rx_time : now_s();
    e.from     = outgoing ? my_num : p.from;
    e.to       = p.to;
    e.snr      = p.rx_snr;
    e.rssi     = p.rx_rssi;
    e.hops     = hops_of(p);
    e.outgoing = outgoing;
    if (p.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        e.portnum = p.decoded.portnum;
        if (p.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
            e.summary.assign(reinterpret_cast<const char*>(p.decoded.payload.bytes), p.decoded.payload.size);
        }
    } else {
        e.portnum = -1;
    }
    packet_log.push_back(std::move(e));
    while (packet_log.size() > kMaxPacketLog) packet_log.pop_front();
    ++rev_log;
}

void Mesh::on_packet(const meshtastic_MeshPacket& p)
{
    // Answers from the MeshZero service itself, not from the mesh.
    if (p.from == 0 && p.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p.decoded.portnum == meshtastic_PortNum_PRIVATE_APP) {
        const std::string text(reinterpret_cast<const char*>(p.decoded.payload.bytes), p.decoded.payload.size);
        if (text == "mz:net=1" || text == "mz:net=0") {
            network_access = text.back() == '1' ? 1 : 0;
        } else if (text == "mz:bg=1" || text == "mz:bg=0") {
            background = text.back() == '1' ? 1 : 0;
        } else if (text == "mz:bgask=1" || text == "mz:bgask=0") {
            ask_background = text.back() == '1';
        } else if (text.rfind("mz:btpin=", 0) == 0) {
            bluetooth_pin = atoi(text.c_str() + 9);
        } else if (text.rfind("mz:btname=", 0) == 0) {
            bluetooth_name = text.substr(10);
        } else if (text.rfind("mz:btok=", 0) == 0) {
            bluetooth_ok = text.back() == '1';
        } else if (text.rfind("mz:btwhy=", 0) == 0) {
            bluetooth_why = text.substr(9);
        } else if (text.rfind("mz:btkey=", 0) == 0) {
            bluetooth_key = text.substr(9);
        } else if (text.rfind("mz:btconn=", 0) == 0) {
            bluetooth_conn = atoi(text.c_str() + 10);
        } else if (text.rfind("mz:btpair=", 0) == 0) {
            const int left       = atoi(text.c_str() + 10);
            bluetooth_pair_until = left > 0 ? now_ms() + static_cast<uint64_t>(left) * 1000 : 0;
        } else if (text.rfind("mz:btpaired=", 0) == 0) {
            bluetooth_paired = atoi(text.c_str() + 12);
        } else if (text.rfind("mz:btpeer=", 0) == 0) {
            bluetooth_peer = text.substr(10);
        } else if (text.rfind("mz:netconn=", 0) == 0) {
            network_conn = atoi(text.c_str() + 11);
        } else if (text.rfind("mz:bt=", 0) == 0) {
            bluetooth = atoi(text.c_str() + 6);
        }
        ++rev_link;
        ++rev_config;
        return;
    }
    log_packet(p, false);

    // Anything we hear from a node proves it is alive.
    if (p.from != 0 && p.from != my_num) {
        meshtastic_NodeInfo& n = nodes[p.from];
        n.num                  = p.from;
        n.last_heard           = p.rx_time ? p.rx_time : now_s();
        if (p.rx_snr != 0) n.snr = p.rx_snr;
        n.via_mqtt = p.via_mqtt;
        // Until it tells us more, a node is reached on the channel it was heard on.
        if (!n.has_user && !p.pki_encrypted && p.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
            n.channel = p.channel;
        }
        const int hops = hops_of(p);
        if (hops >= 0) {
            n.has_hops_away = true;
            n.hops_away     = static_cast<uint8_t>(hops);
        }
        ++rev_nodes;
    }

    if (p.which_payload_variant != meshtastic_MeshPacket_decoded_tag) return;
    const meshtastic_Data& d = p.decoded;
    const uint8_t* data      = d.payload.bytes;
    const size_t len         = d.payload.size;

    switch (d.portnum) {
        case meshtastic_PortNum_TEXT_MESSAGE_APP:
            on_text(p);
            break;
        case meshtastic_PortNum_ROUTING_APP:
            on_routing(p);
            break;
        case meshtastic_PortNum_NODEINFO_APP: {
            meshtastic_User user = meshtastic_User_init_zero;
            if (p.from != 0 && pb_decode_msg(meshtastic_User_fields, data, len, user)) {
                meshtastic_NodeInfo& n = nodes[p.from];
                n.num                  = p.from;
                n.has_user             = true;
                n.user                 = user;
                n.channel              = p.channel;
                ++rev_nodes;
                ++rev_chats;
            }
            break;
        }
        case meshtastic_PortNum_POSITION_APP: {
            meshtastic_Position pos = meshtastic_Position_init_zero;
            if (p.from != 0 && pb_decode_msg(meshtastic_Position_fields, data, len, pos) && pos.has_latitude_i) {
                meshtastic_NodeInfo& n = nodes[p.from];
                n.num                  = p.from;
                n.has_position         = true;
                n.position             = pos;
                ++rev_nodes;
            }
            break;
        }
        case meshtastic_PortNum_TELEMETRY_APP: {
            meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
            if (p.from == 0 || !pb_decode_msg(meshtastic_Telemetry_fields, data, len, t)) break;
            if (t.which_variant == meshtastic_Telemetry_device_metrics_tag) {
                meshtastic_NodeInfo& n = nodes[p.from];
                n.num                  = p.from;
                n.has_device_metrics   = true;
                n.device_metrics       = t.variant.device_metrics;
                ++rev_nodes;
            } else if (t.which_variant == meshtastic_Telemetry_local_stats_tag && p.from == my_num) {
                local_stats     = t.variant.local_stats;
                has_local_stats = true;
                ++rev_nodes;
            } else if (t.which_variant == meshtastic_Telemetry_environment_metrics_tag) {
                Sensors& s = sensors[p.from];
                s.has_env  = true;
                s.env      = t.variant.environment_metrics;
                s.env_ts   = p.rx_time ? p.rx_time : now_s();
                ++rev_nodes;
            } else if (t.which_variant == meshtastic_Telemetry_power_metrics_tag) {
                Sensors& s  = sensors[p.from];
                s.has_power = true;
                s.power     = t.variant.power_metrics;
                s.power_ts  = p.rx_time ? p.rx_time : now_s();
                ++rev_nodes;
            } else if (t.which_variant == meshtastic_Telemetry_air_quality_metrics_tag) {
                Sensors& s = sensors[p.from];
                s.has_air  = true;
                s.air      = t.variant.air_quality_metrics;
                s.air_ts   = p.rx_time ? p.rx_time : now_s();
                ++rev_nodes;
            }
            break;
        }
        case meshtastic_PortNum_TRACEROUTE_APP: {
            const auto it = trace_ids_.find(d.request_id);
            if (it == trace_ids_.end()) break;
            meshtastic_RouteDiscovery r = meshtastic_RouteDiscovery_init_zero;
            if (!pb_decode_msg(meshtastic_RouteDiscovery_fields, data, len, r)) break;
            UiEvent ev;
            ev.type       = UiEvent::Trace;
            ev.trace.dest = it->second;
            for (pb_size_t i = 0; i < r.route_count; ++i) ev.trace.towards.push_back(r.route[i]);
            for (pb_size_t i = 0; i < r.snr_towards_count; ++i) ev.trace.snr_towards.push_back(r.snr_towards[i]);
            for (pb_size_t i = 0; i < r.route_back_count; ++i) ev.trace.back.push_back(r.route_back[i]);
            for (pb_size_t i = 0; i < r.snr_back_count; ++i) ev.trace.snr_back.push_back(r.snr_back[i]);
            events.push_back(std::move(ev));
            trace_ids_.erase(it);
            break;
        }
        default:
            break;
    }
}

void Mesh::on_text(const meshtastic_MeshPacket& p)
{
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
    m.hops     = hops_of(p);
    m.outgoing = p.from == my_num;  // sent through another client of the same radio
    m.text.assign(reinterpret_cast<const char*>(p.decoded.payload.bytes), p.decoded.payload.size);

    ChatKey key;
    if (p.to == kBroadcast) {
        key = ChatKey{false, p.channel};
    } else {
        key = ChatKey{true, m.outgoing ? p.to : p.from};
    }
    const bool incoming = !m.outgoing;
    // Sent from a phone or another app on this radio: its acknowledgement
    // comes past here too, so it gets a delivery status like our own.
    if (m.outgoing && p.want_ack && p.id != 0) {
        m.ack               = AckState::Pending;
        pending_acks_[p.id] = key;
        if (pending_acks_.size() > 256) pending_acks_.erase(pending_acks_.begin());
    }
    add_message(key, std::move(m), true);

    // A sender we have no name for, or no key to answer a direct message with.
    if (incoming) want_info(p.from, key.dm, false, kInfoGraceMs);
}

void Mesh::add_message(const ChatKey& key, StoredMsg msg, bool live)
{
    Chat& c = chat(key);
    if (msg.id != 0) {
        // The radio can deliver the same packet twice (e.g. after a resync).
        const size_t start = c.msgs.size() > 30 ? c.msgs.size() - 30 : 0;
        for (size_t i = start; i < c.msgs.size(); ++i) {
            if (c.msgs[i].id == msg.id && c.msgs[i].from == msg.from) return;
        }
    }
    if (msg.ts > c.last_ts) c.last_ts = msg.ts;
    const bool is_active = has_active_ && active_ == key;
    if (!msg.outgoing) {
        if (live && is_active) {
            c.last_read = msg.ts;
        } else if (msg.ts > c.last_read) {
            ++c.unread;
        }
    }
    // A message in the conversation on screen needs no notice, but its bell still rings.
    const bool bell = !msg.is_tapback() && has_bell(msg.text);
    if (live && !msg.outgoing && (!is_active || bell)) {
        UiEvent ev;
        ev.type = UiEvent::NewMessage;
        ev.chat = key;
        ev.bell = bell;
        ev.seen = is_active;
        ev.text = short_name(msg.from) + (msg.is_tapback() ? " reacted " : ": ") + msg.text;
        events.push_back(std::move(ev));
    }
    c.msgs.push_back(std::move(msg));
    if (c.msgs.size() > kMaxChatMsgs) c.msgs.erase(c.msgs.begin(), c.msgs.begin() + (c.msgs.size() - kMaxChatMsgs));
    ++rev_chats;
}

void Mesh::on_routing(const meshtastic_MeshPacket& p)
{
    if (p.decoded.request_id == 0) return;
    meshtastic_Routing routing = meshtastic_Routing_init_zero;
    if (!pb_decode_msg(meshtastic_Routing_fields, p.decoded.payload.bytes, p.decoded.payload.size, routing)) return;
    const int error =
        routing.which_variant == meshtastic_Routing_error_reason_tag ? static_cast<int>(routing.error_reason) : 0;

    const auto trace = trace_ids_.find(p.decoded.request_id);
    if (trace != trace_ids_.end() && error != 0) {
        toast("Traceroute failed: " + delivery_error_text(error));
        trace_ids_.erase(trace);
    }

    const auto it = pending_acks_.find(p.decoded.request_id);
    if (it == pending_acks_.end()) return;
    Chat& c = chat(it->second);
    for (auto m = c.msgs.rbegin(); m != c.msgs.rend(); ++m) {
        if (m->id != p.decoded.request_id || !m->outgoing) continue;
        if (m->ack == AckState::Acked) break;
        if (error != 0) {
            m->ack       = AckState::Failed;
            m->ack_error = error;
        } else if (p.from == m->to) {
            m->ack = AckState::Acked;
        } else {
            m->ack = AckState::Implicit;
        }
        ++rev_chats;
        break;
    }
}

// ----------------------------------------------------------------- sending

uint32_t Mesh::send_packet(uint32_t to, uint8_t channel, meshtastic_PortNum port, const uint8_t* payload, size_t len,
                           bool want_ack, bool want_response)
{
    if (!ready()) return 0;
    meshtastic_ToRadio msg    = meshtastic_ToRadio_init_zero;
    msg.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket& p  = msg.packet;
    p.to                      = to;
    p.channel                 = channel;
    p.id                      = next_id();
    p.want_ack                = want_ack;
    p.hop_limit               = config.has_lora && config.lora.hop_limit ? config.lora.hop_limit : 3;
    p.which_payload_variant   = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum         = port;
    p.decoded.want_response   = want_response;
    if (len > sizeof(p.decoded.payload.bytes)) return 0;
    memcpy(p.decoded.payload.bytes, payload, len);
    p.decoded.payload.size = static_cast<pb_size_t>(len);
    send_to_radio(msg);
    log_packet(p, true);
    return p.id;
}

bool Mesh::send_text(const ChatKey& key, const std::string& text, uint32_t reply_id)
{
    return send_message(key, text, reply_id, false);
}

bool Mesh::send_tapback(const ChatKey& key, uint32_t target_id, const std::string& emoji)
{
    if (target_id == 0 || emoji.empty()) return false;
    for (const StoredMsg& m : chat(key).msgs) {
        if (m.outgoing && m.is_tapback() && m.reply_id == target_id && m.text == emoji && m.ack != AckState::Failed) {
            toast("Already reacted with that");
            return false;
        }
    }
    return send_message(key, emoji, target_id, true);
}

// A tapback travels as a text message that carries only the emoji, flagged as
// one and pointing at the message it reacts to.
bool Mesh::send_message(const ChatKey& key, const std::string& text, uint32_t reply_id, bool tapback)
{
    if (!ready()) {
        toast("Radio offline");
        return false;
    }
    if (text.empty()) return false;
    if (region_unset()) {
        toast("Set your region first (Setup > Region)");
        return false;
    }
    const std::string body = utf8_truncate(text, kMaxTextLen);

    meshtastic_ToRadio msg    = meshtastic_ToRadio_init_zero;
    msg.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket& p  = msg.packet;
    const meshtastic_NodeInfo* peer = key.dm ? node(key.id) : nullptr;
    p.to                      = key.dm ? key.id : kBroadcast;
    p.channel                 = key.dm ? (peer ? peer->channel : 0) : static_cast<uint8_t>(key.id);
    p.id                      = next_id();
    p.want_ack                = true;
    p.hop_limit               = config.has_lora && config.lora.hop_limit ? config.lora.hop_limit : 3;
    p.which_payload_variant   = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum         = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p.decoded.reply_id        = reply_id;
    p.decoded.emoji           = tapback ? 1 : 0;
    memcpy(p.decoded.payload.bytes, body.data(), body.size());
    p.decoded.payload.size = static_cast<pb_size_t>(body.size());
    send_to_radio(msg);
    log_packet(p, true);

    StoredMsg m;
    m.ts       = now_s();
    m.id       = p.id;
    m.from     = my_num;
    m.to       = p.to;
    m.channel  = p.channel;
    m.outgoing = true;
    m.reply_id = reply_id;
    m.emoji    = tapback;
    m.pki      = peer && peer->has_user && peer->user.public_key.size > 0;
    m.ack      = AckState::Pending;
    m.text     = body;
    pending_acks_[p.id] = key;
    if (pending_acks_.size() > 256) pending_acks_.erase(pending_acks_.begin());
    add_message(key, std::move(m), true);

    if (key.dm) want_info(key.id, true, true);
    return true;
}

void Mesh::traceroute(uint32_t dest)
{
    meshtastic_RouteDiscovery r = meshtastic_RouteDiscovery_init_zero;
    const std::string bytes     = pb_encode_msg(meshtastic_RouteDiscovery_fields, r);
    const meshtastic_NodeInfo* n = node(dest);
    const uint32_t id = send_packet(dest, n ? n->channel : 0, meshtastic_PortNum_TRACEROUTE_APP,
                                    reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), true, true);
    if (id) {
        trace_ids_[id] = dest;
        toast("Traceroute sent to " + short_name(dest));
    } else {
        toast("Radio offline");
    }
}

void Mesh::request_position(uint32_t dest)
{
    meshtastic_Position pos = meshtastic_Position_init_zero;
    if (const meshtastic_NodeInfo* self = me(); self && self->has_position) pos = self->position;
    const std::string bytes      = pb_encode_msg(meshtastic_Position_fields, pos);
    const meshtastic_NodeInfo* n = node(dest);
    const uint32_t id = send_packet(dest, n ? n->channel : 0, meshtastic_PortNum_POSITION_APP,
                                    reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), true, true);
    toast(id ? "Position requested from " + short_name(dest) : "Radio offline");
}

void Mesh::request_nodeinfo(uint32_t dest)
{
    const uint32_t id = send_info_request(dest);
    // Count it, so an automatic request does not follow right behind.
    const auto it = info_asks_.find(dest);
    if (id && it != info_asks_.end()) {
        it->second.at  = now_ms();
        it->second.due = std::max(it->second.due, it->second.at + kInfoRetryMs);
    }
    toast(id ? "Node info requested from " + short_name(dest) : "Radio offline");
}

// A node info request carries our own user record and asks for theirs back.
uint32_t Mesh::send_info_request(uint32_t dest)
{
    meshtastic_User user = meshtastic_User_init_zero;
    if (const meshtastic_NodeInfo* self = me(); self && self->has_user) user = self->user;
    const std::string bytes      = pb_encode_msg(meshtastic_User_fields, user);
    const meshtastic_NodeInfo* n = node(dest);
    return send_packet(dest, n ? n->channel : 0, meshtastic_PortNum_NODEINFO_APP,
                       reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), true, true);
}

bool Mesh::has_key(uint32_t num) const
{
    const meshtastic_NodeInfo* n = node(num);
    return n && n->has_user && n->user.public_key.size > 0;
}

bool Mesh::awaiting_key(uint32_t num) const
{
    if (has_key(num)) return false;
    const auto it = info_asks_.find(num);
    return it != info_asks_.end() && it->second.need_key && it->second.tries <= kKeyPatience;
}

bool Mesh::info_complete(uint32_t num, bool need_key) const
{
    const meshtastic_NodeInfo* n = node(num);
    if (!n || !n->has_user || !n->user.short_name[0]) return false;
    // Licensed operators send in the clear and have no key, on either side.
    if (!need_key || n->user.public_key.size > 0 || n->user.is_licensed) return true;
    const meshtastic_NodeInfo* self = me();
    return self && self->has_user && self->user.is_licensed;
}

bool Mesh::channel_busy() const
{
    if (has_local_stats) return local_stats.channel_utilization > kBusyChannelPct;
    const meshtastic_NodeInfo* self = me();
    return self && self->has_device_metrics && self->device_metrics.has_channel_utilization &&
           self->device_metrics.channel_utilization > kBusyChannelPct;
}

void Mesh::want_info(uint32_t num, bool need_key, bool user_waiting, uint64_t delay_ms)
{
    if (num == 0 || num == my_num || num == kBroadcast) return;
    if (const meshtastic_NodeInfo* n = node(num); n && n->is_ignored) return;
    if (info_complete(num, need_key)) return;

    const uint64_t now     = now_ms();
    const auto [it, fresh] = info_asks_.try_emplace(num);
    InfoAsk& ask           = it->second;
    if (!fresh && ask.tries >= kInfoTries) {
        // Given up on: one more try, at most once an hour.
        if (now - ask.at < kInfoRetryMaxMs) return;
        ask.tries = kInfoTries - 1;
        ask.due   = now + delay_ms;
    }
    ask.need_key = ask.need_key || need_key;
    ask.urgent   = ask.urgent || user_waiting;
    const uint64_t due = std::max(now + delay_ms, ask.at + info_backoff(ask.tries));
    if (fresh || due < ask.due) ask.due = due;
}

// Opening a channel asks about the recent senders it can only show by number,
// as long as they are still around.
void Mesh::ask_channel_senders(const ChatKey& key)
{
    const Chat& c     = chat(key);
    const uint32_t now = now_s();
    std::vector<uint32_t> asked;
    for (auto m = c.msgs.rbegin(); m != c.msgs.rend() && asked.size() < kChannelAskMax; ++m) {
        if (m->outgoing || std::find(asked.begin(), asked.end(), m->from) != asked.end()) continue;
        const meshtastic_NodeInfo* n = node(m->from);
        if (!n || !n->last_heard || now - n->last_heard > kChannelAskAgeS || info_complete(m->from, false)) continue;
        asked.push_back(m->from);
        want_info(m->from, false, false);
    }
}

// Sends the most pressing automatic request that is due, one at a time.
void Mesh::pump_info_asks()
{
    const uint64_t now = now_ms();
    if (!ready() || info_asks_.empty() || now < next_info_ms_) return;

    const bool busy = channel_busy();
    uint32_t pick   = 0;
    const InfoAsk* best = nullptr;
    for (auto it = info_asks_.begin(); it != info_asks_.end();) {
        if (info_complete(it->first, it->second.need_key)) {
            it = info_asks_.erase(it);
            continue;
        }
        const InfoAsk& a = it->second;
        if (a.tries < kInfoTries && a.due <= now && (a.urgent || !busy) &&
            (!best || a.urgent > best->urgent || (a.urgent == best->urgent && a.due < best->due))) {
            best = &a;
            pick = it->first;
        }
        ++it;
    }
    if (!best) {
        next_info_ms_ = now + 1000;
        return;
    }
    if (!send_info_request(pick)) return;
    InfoAsk& a    = info_asks_[pick];
    a.at          = now;
    a.urgent      = false;
    a.due         = now + info_backoff(++a.tries);
    next_info_ms_ = now + kInfoGapMs;
    ++rev_chats;  // a chat may now say it is waiting
}

void Mesh::request_telemetry(uint32_t dest, TelemetryKind kind)
{
    // A node answers with the kind of readings the request names.
    meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
    const char* what       = "Telemetry";
    switch (kind) {
        case TelemetryKind::Device:
            t.which_variant = meshtastic_Telemetry_device_metrics_tag;
            break;
        case TelemetryKind::Environment:
            t.which_variant = meshtastic_Telemetry_environment_metrics_tag;
            what            = "Environment readings";
            break;
        case TelemetryKind::Power:
            t.which_variant = meshtastic_Telemetry_power_metrics_tag;
            what            = "Power readings";
            break;
        case TelemetryKind::AirQuality:
            t.which_variant = meshtastic_Telemetry_air_quality_metrics_tag;
            what            = "Air quality readings";
            break;
    }
    const std::string bytes      = pb_encode_msg(meshtastic_Telemetry_fields, t);
    const meshtastic_NodeInfo* n = node(dest);
    const uint32_t id = send_packet(dest, n ? n->channel : 0, meshtastic_PortNum_TELEMETRY_APP,
                                    reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), true, true);
    toast(id ? std::string(what) + " requested from " + short_name(dest) : "Radio offline");
}

// ------------------------------------------------------------------ admin

void Mesh::admin(meshtastic_AdminMessage& msg, bool want_response)
{
    const std::string bytes = pb_encode_msg(meshtastic_AdminMessage_fields, msg);
    if (bytes.empty()) return;
    send_packet(my_num, 0, meshtastic_PortNum_ADMIN_APP, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                true, want_response);
}

void Mesh::begin_edit()
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_begin_edit_settings_tag;
    m.begin_edit_settings     = true;
    admin(m);
}

void Mesh::commit_edit()
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_commit_edit_settings_tag;
    m.commit_edit_settings    = true;
    admin(m);
}

void Mesh::set_owner(const meshtastic_User& user)
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_set_owner_tag;
    m.set_owner               = user;
    admin(m);
    if (meshtastic_NodeInfo* self = const_cast<meshtastic_NodeInfo*>(me())) {
        snprintf(self->user.long_name, sizeof(self->user.long_name), "%s", user.long_name);
        snprintf(self->user.short_name, sizeof(self->user.short_name), "%s", user.short_name);
        self->has_user = true;
        ++rev_nodes;
    }
}

void Mesh::set_config(const meshtastic_Config& cfg)
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_set_config_tag;
    m.set_config              = cfg;
    admin(m);
    // Mirror locally so the UI reflects the change before the radio confirms.
    switch (cfg.which_payload_variant) {
        case meshtastic_Config_device_tag:
            config.device = cfg.payload_variant.device;
            break;
        case meshtastic_Config_position_tag:
            config.position = cfg.payload_variant.position;
            break;
        case meshtastic_Config_power_tag:
            config.power = cfg.payload_variant.power;
            break;
        case meshtastic_Config_network_tag:
            config.network = cfg.payload_variant.network;
            break;
        case meshtastic_Config_lora_tag:
            config.lora = cfg.payload_variant.lora;
            break;
        case meshtastic_Config_security_tag:
            config.security = cfg.payload_variant.security;
            break;
        default:
            break;
    }
    ++rev_config;
}

void Mesh::set_module_config(const meshtastic_ModuleConfig& cfg)
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_set_module_config_tag;
    m.set_module_config       = cfg;
    admin(m);
    switch (cfg.which_payload_variant) {
        case meshtastic_ModuleConfig_mqtt_tag:
            modules.mqtt = cfg.payload_variant.mqtt;
            break;
        case meshtastic_ModuleConfig_telemetry_tag:
            modules.telemetry = cfg.payload_variant.telemetry;
            break;
        case meshtastic_ModuleConfig_neighbor_info_tag:
            modules.neighbor_info = cfg.payload_variant.neighbor_info;
            break;
        case meshtastic_ModuleConfig_store_forward_tag:
            modules.store_forward = cfg.payload_variant.store_forward;
            break;
        case meshtastic_ModuleConfig_range_test_tag:
            modules.range_test = cfg.payload_variant.range_test;
            break;
        default:
            break;
    }
    ++rev_config;
}

void Mesh::set_channel(const meshtastic_Channel& channel)
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_set_channel_tag;
    m.set_channel             = channel;
    admin(m);
    if (channel.index >= 0 && channel.index < kMaxChannels) {
        channels[channel.index] = channel;
        ensure_channel_chats();
        ++rev_config;
        ++rev_chats;
    }
}

void Mesh::set_favorite(uint32_t num, bool favorite)
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    if (favorite) {
        m.which_payload_variant = meshtastic_AdminMessage_set_favorite_node_tag;
        m.set_favorite_node     = num;
    } else {
        m.which_payload_variant = meshtastic_AdminMessage_remove_favorite_node_tag;
        m.remove_favorite_node  = num;
    }
    admin(m);
    const auto it = nodes.find(num);
    if (it != nodes.end()) it->second.is_favorite = favorite;
    ++rev_nodes;
}

void Mesh::set_ignored(uint32_t num, bool ignored)
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    if (ignored) {
        m.which_payload_variant = meshtastic_AdminMessage_set_ignored_node_tag;
        m.set_ignored_node      = num;
    } else {
        m.which_payload_variant = meshtastic_AdminMessage_remove_ignored_node_tag;
        m.remove_ignored_node   = num;
    }
    admin(m);
    const auto it = nodes.find(num);
    if (it != nodes.end()) it->second.is_ignored = ignored;
    ++rev_nodes;
}

void Mesh::remove_node(uint32_t num)
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_remove_by_nodenum_tag;
    m.remove_by_nodenum       = num;
    admin(m);
    nodes.erase(num);
    sensors.erase(num);
    ++rev_nodes;
}

void Mesh::set_fixed_position(double lat, double lon, int alt)
{
    meshtastic_AdminMessage m       = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant         = meshtastic_AdminMessage_set_fixed_position_tag;
    m.set_fixed_position.has_latitude_i  = true;
    m.set_fixed_position.latitude_i      = static_cast<int32_t>(lat * 1e7);
    m.set_fixed_position.has_longitude_i = true;
    m.set_fixed_position.longitude_i     = static_cast<int32_t>(lon * 1e7);
    m.set_fixed_position.has_altitude    = true;
    m.set_fixed_position.altitude        = alt;
    m.set_fixed_position.time            = now_s();
    admin(m);
}

void Mesh::remove_fixed_position()
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_remove_fixed_position_tag;
    m.remove_fixed_position   = true;
    admin(m);
}

void Mesh::reboot(int seconds)
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_reboot_seconds_tag;
    m.reboot_seconds          = seconds;
    admin(m);
}

void Mesh::shutdown_radio(int seconds)
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_shutdown_seconds_tag;
    m.shutdown_seconds        = seconds;
    admin(m);
}

void Mesh::factory_reset(bool full)
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    if (full) {
        m.which_payload_variant = meshtastic_AdminMessage_factory_reset_device_tag;
        m.factory_reset_device  = 1;
    } else {
        m.which_payload_variant = meshtastic_AdminMessage_factory_reset_config_tag;
        m.factory_reset_config  = 1;
    }
    admin(m);
}

void Mesh::nodedb_reset()
{
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant   = meshtastic_AdminMessage_nodedb_reset_tag;
    m.nodedb_reset            = true;
    admin(m);
    const meshtastic_NodeInfo* self = me();
    if (self) {
        const meshtastic_NodeInfo keep = *self;
        nodes.clear();
        nodes[my_num] = keep;
        sensors.clear();
    }
    ++rev_nodes;
}

std::string delivery_error_text(int error)
{
    switch (error) {
        case meshtastic_Routing_Error_MAX_RETRANSMIT:
        case meshtastic_Routing_Error_TIMEOUT:
        case meshtastic_Routing_Error_NO_RESPONSE:
            return "no reply";
        case meshtastic_Routing_Error_NO_ROUTE:
            return "no route";
        case meshtastic_Routing_Error_NO_CHANNEL:
            return "recipient is not on this channel";
        case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
            // Our radio answers this by sending them our key, so a retry works.
            return "they did not have your key yet, send again";
        case meshtastic_Routing_Error_PKI_FAILED:
            return "no key for this node yet";
        case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT:
            return "duty cycle limit reached";
        case meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
            return "sending too fast";
        case meshtastic_Routing_Error_TOO_LARGE:
            return "message too long";
        case meshtastic_Routing_Error_NOT_AUTHORIZED:
            return "not authorized";
        default: {
            std::string name = routing_error_name(error);
            for (auto& c : name) c = c == '_' ? ' ' : static_cast<char>(tolower(c));
            return name;
        }
    }
}

// ----------------------------------------------------------------- lookups

const meshtastic_NodeInfo* Mesh::node(uint32_t num) const
{
    const auto it = nodes.find(num);
    return it == nodes.end() ? nullptr : &it->second;
}

std::string Mesh::short_name(uint32_t num) const
{
    const meshtastic_NodeInfo* n = node(num);
    if (n && n->has_user && n->user.short_name[0]) return n->user.short_name;
    char buf[8];
    snprintf(buf, sizeof(buf), "%04x", num & 0xffff);
    return buf;
}

std::string Mesh::long_name(uint32_t num) const
{
    const meshtastic_NodeInfo* n = node(num);
    if (n && n->has_user && n->user.long_name[0]) return n->user.long_name;
    return node_id_str(num);
}

std::string Mesh::preset_name() const
{
    if (!config.has_lora) return "LongFast";
    if (!config.lora.use_preset) return "Custom";
    return modem_preset_name(config.lora.modem_preset);
}

std::string modem_preset_name(int preset)
{
    // As DisplayFormatters::getModemPresetDisplayName() in the radio firmware
    // MeshZero runs (2.7.26). It doesn't know the Very long, Lite and Narrow
    // presets: it runs them as LongFast, under the name "Invalid".
    switch (preset) {
        case meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST:
            return "LongFast";
        case meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW:
            return "LongSlow";
        case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW:
            return "MediumSlow";
        case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST:
            return "MediumFast";
        case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW:
            return "ShortSlow";
        case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST:
            return "ShortFast";
        case meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE:
            return "LongMod";
        case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO:
            return "ShortTurbo";
        case meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO:
            return "LongTurbo";
        default:
            return "Invalid";
    }
}

bool Mesh::channel_enabled(int index) const
{
    return index >= 0 && index < kMaxChannels && channels[index].role != meshtastic_Channel_Role_DISABLED;
}

std::string Mesh::channel_name(int index) const
{
    if (index < 0 || index >= kMaxChannels) return "?";
    const meshtastic_Channel& c = channels[index];
    if (c.has_settings && c.settings.name[0]) return c.settings.name;
    // An unnamed channel is known by the modem preset, as in every other client.
    if (c.role == meshtastic_Channel_Role_DISABLED) return "Channel " + std::to_string(index);
    return preset_name();
}

std::string Mesh::chat_title(const ChatKey& key) const
{
    return key.dm ? long_name(key.id) : "#" + channel_name(static_cast<int>(key.id));
}

int Mesh::total_unread() const
{
    int total = 0;
    for (const auto& [key, c] : chats) {
        if (!muted(key)) total += c.unread;
    }
    return total;
}

bool Mesh::muted(const ChatKey& key) const
{
    if (key.dm) {
        const meshtastic_NodeInfo* n = node(key.id);
        return n && n->is_muted;
    }
    if (key.id >= static_cast<uint32_t>(kMaxChannels)) return false;
    const meshtastic_ChannelSettings& s = channels[key.id].settings;
    return s.has_module_settings && s.module_settings.is_muted;
}

bool Mesh::set_muted(const ChatKey& key, bool mute)
{
    if (!ready()) return false;
    if (muted(key) == mute) return true;
    if (key.dm) {
        // The radio only has a switch to flip, and only for nodes it knows.
        const auto it = nodes.find(key.id);
        if (it == nodes.end()) return false;
        meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
        m.which_payload_variant   = meshtastic_AdminMessage_toggle_muted_node_tag;
        m.toggle_muted_node       = key.id;
        admin(m);
        it->second.is_muted = mute;
        ++rev_nodes;
        ++rev_chats;
        return true;
    }
    if (!channel_enabled(static_cast<int>(key.id))) return false;
    meshtastic_Channel ch                = channels[key.id];
    ch.settings.has_module_settings      = true;
    ch.settings.module_settings.is_muted = mute;
    set_channel(ch);
    return true;
}

bool Mesh::region_unset() const
{
    return config.has_lora && config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET;
}

bool Mesh::node_position(const meshtastic_NodeInfo& n, double& lat, double& lon)
{
    if (!n.has_position || !n.position.has_latitude_i || !n.position.has_longitude_i) return false;
    if (n.position.latitude_i == 0 && n.position.longitude_i == 0) return false;
    lat = n.position.latitude_i * 1e-7;
    lon = n.position.longitude_i * 1e-7;
    return true;
}

bool Mesh::my_position(double& lat, double& lon) const
{
    const meshtastic_NodeInfo* self = me();
    return self && node_position(*self, lat, lon);
}

// ----------------------------------------------------------- conversations

Chat& Mesh::chat(const ChatKey& key)
{
    const auto it = chats.find(key);
    if (it != chats.end()) return it->second;
    Chat& c = chats[key];
    c.key   = key;
    ++rev_chats;
    return c;
}

void Mesh::set_active_chat(const ChatKey* key)
{
    has_active_ = key != nullptr;
    if (key) {
        active_ = *key;
        mark_read(*key);
        if (key->dm) {
            want_info(key->id, true, true);
        } else {
            ask_channel_senders(*key);
        }
    }
}

void Mesh::mark_read(const ChatKey& key)
{
    Chat& c = chat(key);
    if (c.unread == 0 && c.last_read >= c.last_ts) return;
    c.unread    = 0;
    c.last_read = c.last_ts > now_s() ? c.last_ts : now_s();
    ++rev_chats;
    save_read_state();
}

void Mesh::load_history()
{
    history_loaded_ = true;
    for (StoredMsg& m : history_load()) {
        if (m.outgoing ? m.from != my_num : m.from == my_num) continue;
        ChatKey key;
        if (m.to == kBroadcast) {
            key = ChatKey{false, m.channel};
        } else {
            key = ChatKey{true, m.outgoing ? m.to : m.from};
        }
        // A message still pending from an earlier session will never be confirmed.
        if (m.outgoing && m.ack == AckState::Pending && now_s() > m.ts + 600) m.ack = AckState::None;
        add_message(key, std::move(m), false);
    }
}

void Mesh::load_read_state()
{
    std::ifstream in(user_state_dir() + "/read");
    std::string kind;
    uint32_t id = 0;
    uint32_t ts = 0;
    while (in >> kind >> id >> ts) {
        Chat& c     = chats[ChatKey{kind == "d", id}];
        c.key       = ChatKey{kind == "d", id};
        c.last_read = ts;
    }
}

void Mesh::save_read_state()
{
    const std::string dir = user_state_dir();
    make_dirs(dir);
    std::ofstream out(dir + "/read.tmp", std::ios::trunc);
    if (!out) return;
    for (const auto& [key, c] : chats) {
        if (c.last_read) out << (key.dm ? "d " : "c ") << key.id << ' ' << c.last_read << '\n';
    }
    out.close();
    rename((dir + "/read.tmp").c_str(), (dir + "/read").c_str());
}

}  // namespace mz
