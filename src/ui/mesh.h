// SPDX-License-Identifier: GPL-3.0-only
// Client-side model of the mesh: talks the stream API over TCP and keeps the
// node database, channels, configuration and conversations the UI renders.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "common/history.h"
#include "common/proto.h"

namespace mz {

constexpr int kMaxChannels   = 8;
constexpr size_t kMaxTextLen = 200;

struct ChatKey {
    bool dm     = false;
    uint32_t id = 0;  // channel index, or peer node number for direct messages
    bool operator<(const ChatKey& o) const { return dm != o.dm ? dm < o.dm : id < o.id; }
    bool operator==(const ChatKey& o) const { return dm == o.dm && id == o.id; }
    bool operator!=(const ChatKey& o) const { return !(*this == o); }
};

struct Chat {
    ChatKey key;
    std::vector<StoredMsg> msgs;
    int unread         = 0;
    uint32_t last_ts   = 0;
    uint32_t last_read = 0;
};

struct PacketLogEntry {
    uint32_t ts   = 0;
    uint32_t from = 0;
    uint32_t to   = 0;
    int portnum   = 0;
    float snr     = 0;
    int rssi      = 0;
    int hops      = -1;
    bool outgoing = false;
    std::string summary;
};

struct TraceResult {
    uint32_t dest = 0;
    std::vector<uint32_t> towards;  // intermediate hops on the way out
    std::vector<int> snr_towards;   // in quarter dB, one more entry than hops when complete
    std::vector<uint32_t> back;
    std::vector<int> snr_back;
};

struct UiEvent {
    enum Type { NewMessage, Toast, Trace, LinkChanged } type = Toast;
    ChatKey chat;
    bool bell = false;  // NewMessage: it carries an alert bell
    bool seen = false;  // NewMessage: its conversation is the one on screen
    std::string text;
    TraceResult trace;
};

// Sensor readings a node has sent, kept while the app runs: the radio itself
// only remembers a node's device metrics.
struct Sensors {
    bool has_env = false;
    meshtastic_EnvironmentMetrics env{};
    uint32_t env_ts = 0;
    bool has_power = false;
    meshtastic_PowerMetrics power{};
    uint32_t power_ts = 0;
    bool has_air = false;
    meshtastic_AirQualityMetrics air{};
    uint32_t air_ts = 0;
};

enum class TelemetryKind { Device, Environment, Power, AirQuality };

// Whether a message text carries the alert bell character.
inline bool has_bell(const std::string& text)
{
    return text.find('\a') != std::string::npos;
}

enum class LinkState { Disconnected, Connecting, Syncing, Ready };

class Mesh {
public:
    Mesh();
    ~Mesh();

    void configure(const std::string& host, uint16_t port);
    void poll();
    LinkState state() const { return state_; }
    bool ready() const { return state_ == LinkState::Ready; }
    // True between the first successful sync and now, even across reconnects.
    bool ever_ready() const { return ever_ready_; }
    // Why the radio is down, as reported by the service: "nocap", "starting",
    // "stopped", or empty when unknown.
    std::string radio_note();
    bool cap_missing() { return radio_note() == "nocap"; }
    // Short text for the state above.
    std::string offline_text();
    void resync();

    // ---- state -------------------------------------------------------
    uint32_t my_num = 0;
    meshtastic_MyNodeInfo my_info{};
    meshtastic_DeviceMetadata metadata{};
    bool has_metadata = false;
    std::map<uint32_t, meshtastic_NodeInfo> nodes;
    std::map<uint32_t, Sensors> sensors;
    meshtastic_Channel channels[kMaxChannels]{};
    meshtastic_LocalConfig config{};
    meshtastic_LocalModuleConfig modules{};
    meshtastic_LocalStats local_stats{};
    bool has_local_stats = false;
    std::map<ChatKey, Chat> chats;
    std::deque<PacketLogEntry> packet_log;
    std::deque<UiEvent> events;

    // Bumped whenever the matching part of the state changes.
    uint32_t rev_nodes  = 1;
    uint32_t rev_chats  = 1;
    uint32_t rev_config = 1;
    uint32_t rev_link   = 1;
    uint32_t rev_log    = 1;

    // ---- lookups -----------------------------------------------------
    const meshtastic_NodeInfo* node(uint32_t num) const;
    const meshtastic_NodeInfo* me() const { return node(my_num); }
    std::string short_name(uint32_t num) const;
    std::string long_name(uint32_t num) const;
    std::string channel_name(int index) const;
    std::string chat_title(const ChatKey& key) const;
    std::string preset_name() const;
    bool channel_enabled(int index) const;
    // Unread messages in the conversations that are not muted.
    int total_unread() const;
    bool region_unset() const;
    bool my_position(double& lat, double& lon) const;
    static bool node_position(const meshtastic_NodeInfo& n, double& lat, double& lon);

    // ---- conversations ----------------------------------------------
    Chat& chat(const ChatKey& key);
    void set_active_chat(const ChatKey* key);
    void mark_read(const ChatKey& key);
    // A muted conversation makes no sound and shows no notice, and is left out
    // of the unread count. The radio keeps it: with the channel, or for direct
    // messages with the node, so other apps on this radio see the same.
    bool muted(const ChatKey& key) const;
    // False when it cannot be set: no such channel, or a node the radio does
    // not know.
    bool set_muted(const ChatKey& key, bool muted);
    // Deletes the messages of one conversation, or of all, here and in the
    // history the service keeps.
    void delete_history(const ChatKey& key);
    void delete_all_history();
    bool send_text(const ChatKey& key, const std::string& text, uint32_t reply_id = 0);
    // Reacts to the message with this packet id with an emoji.
    bool send_tapback(const ChatKey& key, uint32_t target_id, const std::string& emoji);

    // ---- requests ----------------------------------------------------
    void traceroute(uint32_t dest);
    void request_position(uint32_t dest);
    void request_nodeinfo(uint32_t dest);
    bool has_key(uint32_t num) const;
    bool awaiting_key(uint32_t num) const;
    // Asks a node for its user record (names and public key) in the background,
    // retrying with growing pauses, until it is known. `need_key` also waits for
    // the public key. `user_waiting` marks a request someone is looking at the
    // screen for: it goes first and is sent even when the channel is busy.
    void want_info(uint32_t num, bool need_key, bool user_waiting, uint64_t delay_ms = 0);
    void request_telemetry(uint32_t dest, TelemetryKind kind = TelemetryKind::Device);

    // ---- administration (local node) ---------------------------------
    void admin(meshtastic_AdminMessage& msg, bool want_response = false);
    void begin_edit();
    void commit_edit();
    void set_owner(const meshtastic_User& user);
    void set_config(const meshtastic_Config& cfg);
    void set_module_config(const meshtastic_ModuleConfig& cfg);
    void set_channel(const meshtastic_Channel& channel);
    void set_favorite(uint32_t num, bool favorite);
    void set_ignored(uint32_t num, bool ignored);
    void remove_node(uint32_t num);
    void set_fixed_position(double lat, double lon, int alt);
    void remove_fixed_position();
    void reboot(int seconds = 2);
    void shutdown_radio(int seconds = 2);
    void factory_reset(bool full);
    void nodedb_reset();

    // ---- service ----------------------------------------------------
    // Whether other devices on the network may use the API: -1 unknown, 0, 1.
    int network_access = -1;
    // Whether the radio keeps running with the app closed: -1 unknown, 0, 1.
    int background = -1;
    void set_background(bool on);
    // Set while the user is to be asked whether to run in the background: the
    // update to 0.2.1 switched it off for them. set_background() answers it.
    bool ask_background = false;

    // Bluetooth for the phone apps, run by the service: -2 unknown, -1 not in
    // this build, 0, 1. The rest is what the service last said about it.
    int bluetooth     = -2;
    int bluetooth_pin = -1;       // pairing with a code: -1 unknown, 0, 1
    bool bluetooth_ok = false;    // phones can find it right now
    std::string bluetooth_name;
    std::string bluetooth_why;    // why it is not working, when it is not
    std::string bluetooth_key;    // the code to type on a phone that is pairing
    int bluetooth_conn   = 0;     // phones connected over Bluetooth right now
    int bluetooth_paired = 0;     // phones paired
    std::string bluetooth_peer;   // the name of a connected phone
    int network_conn     = 0;     // apps connected from other devices over the network
    // A new phone may pair until then (as now_ms() counts), 0 when none may.
    uint64_t bluetooth_pair_until = 0;
    void set_bluetooth_pairing(bool open);
    void set_bluetooth_pin(bool on);
    // Drops the pairing with every phone. Each phone has to forget this
    // device as well before it can pair again.
    void forget_bluetooth_phones();
    // How other apps reach this radio; one way at a time, which the service
    // sees to. -1 while it is not known yet.
    enum AppConnection { kAppOff = 0, kAppWifi = 1, kAppBluetooth = 2 };
    int app_connection() const;
    void set_app_connection(int mode);

    void toast(const std::string& text);

private:
    void disconnect(const char* why);
    void start_connect();
    void send_to_radio(const meshtastic_ToRadio& msg);
    void request_config();
    void send_control(const std::string& request);
    uint32_t send_packet(uint32_t to, uint8_t channel, meshtastic_PortNum port, const uint8_t* payload, size_t len,
                         bool want_ack, bool want_response);
    bool send_message(const ChatKey& key, const std::string& text, uint32_t reply_id, bool tapback);
    void on_frame(const std::string& payload);
    void on_packet(const meshtastic_MeshPacket& p);
    void on_text(const meshtastic_MeshPacket& p);
    void on_routing(const meshtastic_MeshPacket& p);
    void add_message(const ChatKey& key, StoredMsg msg, bool live);
    void log_packet(const meshtastic_MeshPacket& p, bool outgoing);
    void ensure_channel_chats();
    void load_history();
    void load_read_state();
    void save_read_state();
    void set_state(LinkState s);
    uint32_t next_id();
    bool info_complete(uint32_t num, bool need_key) const;
    bool channel_busy() const;
    uint32_t send_info_request(uint32_t dest);
    void ask_channel_senders(const ChatKey& key);
    void pump_info_asks();

    std::string host_ = "127.0.0.1";
    uint16_t port_    = kDefaultApiPort;
    int fd_           = -1;
    LinkState state_  = LinkState::Disconnected;
    bool ever_ready_  = false;
    FrameParser parser_;
    std::string out_;
    uint64_t next_connect_ms_ = 0;
    uint64_t sync_start_ms_   = 0;
    uint64_t last_rx_ms_      = 0;
    uint64_t last_hb_ms_      = 0;
    uint32_t nonce_           = 0;
    uint32_t id_seq_          = 0;
    bool history_loaded_      = false;
    std::string radio_note_;
    uint64_t cap_checked_ms_  = 0;
    bool has_active_          = false;
    ChatKey active_;
    std::map<uint32_t, ChatKey> pending_acks_;  // packet id -> conversation
    std::map<uint32_t, uint32_t> trace_ids_;    // request id -> destination
    struct InfoAsk {
        uint64_t due  = 0;  // earliest time for the next request
        uint64_t at   = 0;  // when it was last asked
        int tries     = 0;
        bool need_key = false;
        bool urgent   = false;  // someone is waiting on the screen for it
    };
    std::map<uint32_t, InfoAsk> info_asks_;  // node -> automatic requests for its info
    uint64_t next_info_ms_ = 0;              // earliest time for any automatic request
};

const char* portnum_name(int portnum);
const char* routing_error_name(int error);
// Why a message was not delivered, in plain words.
std::string delivery_error_text(int error);
const char* hw_model_name(int model);
const char* role_name(int role);
// A modem preset's name as the radio firmware writes it, which is also the
// name of that preset's public channel: "LongFast", "LongMod", ...
std::string modem_preset_name(int preset);

}  // namespace mz
