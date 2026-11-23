// SPDX-License-Identifier: GPL-3.0-only
// Radio status tab, packet log, help text and the tab factory.
#include <ctime>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "common/util.h"
#include "ui/app.h"

namespace mz {
std::vector<std::string> local_addresses()
{
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        // Container and virtual bridges are not reachable from other devices.
        const std::string name = i->ifa_name ? i->ifa_name : "";
        if (name.rfind("docker", 0) == 0 || name.rfind("veth", 0) == 0 || name.rfind("br-", 0) == 0) continue;
        char buf[32] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, buf, sizeof(buf));
        out.emplace_back(buf);
    }
    freeifaddrs(list);
    return out;
}

namespace {

std::string fmt(const char* format, double value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), format, value);
    return buf;
}

class RadioScreen : public Screen {
public:
    explicit RadioScreen(App& a) : Screen(a) { list_ = std::make_unique<ui::ListView>(root, 0, 0, kContentW, kContentH); }
    int tab() const override { return kTabRadio; }
    std::string hint() const override { return "Up/Down scroll   Tab next page"; }

    void refresh() override
    {
        std::vector<ui::ListItem> items;
        log_row_ = -1;
        const auto header = [&](const std::string& text) {
            ui::ListItem it;
            it.text   = text;
            it.header = true;
            items.push_back(it);
        };
        const auto row = [&](const std::string& name, const std::string& value, bool warn = false) {
            ui::ListItem it;
            it.text            = name;
            it.has_color       = true;
            it.color           = ui::c_dim();
            it.right           = ui::clean(value, true);
            it.has_right_color = true;
            it.right_color     = warn ? ui::c_warn() : ui::c_text();
            items.push_back(it);
        };

        header("Link");
        switch (mesh.state()) {
            case LinkState::Ready:
                row("Radio", "connected");
                break;
            case LinkState::Syncing:
                row("Radio", "starting...", true);
                break;
            default:
                row("Radio", mesh.offline_text(), true);
                break;
        }
        if (mesh.app_connection() == Mesh::kAppBluetooth) {
            row("App connection",
                mesh.bluetooth_conn > 0 ? "Bluetooth, " + (mesh.bluetooth_peer.empty() ? std::string("connected") : mesh.bluetooth_peer)
                                        : "Bluetooth, " + (mesh.bluetooth_name.empty() ? std::string("starting") : mesh.bluetooth_name),
                !mesh.bluetooth_ok);
        } else if (mesh.app_connection() != Mesh::kAppWifi) {
            row("App connection", "off");
        } else {
            const std::vector<std::string> addrs = local_addresses();
            if (addrs.empty()) row("App connection", "Wi-Fi, no network", true);
            for (const auto& a : addrs) row("App connection", a + ":4403");
        }

        if (mesh.ever_ready()) {
            header("This node");
            row("Name", mesh.long_name(mesh.my_num) + " (" + mesh.short_name(mesh.my_num) + ")");
            row("ID", node_id_str(mesh.my_num));
            if (mesh.has_metadata) row("Firmware", mesh.metadata.firmware_version);
            row("Role", role_name(mesh.config.device.role));

            header("LoRa");
            const auto& lora = mesh.config.lora;
            if (mesh.region_unset()) {
                row("Region", "not set", true);
            } else {
                row("Region", std::to_string(lora.region) == "0" ? "UNSET" : region_label(lora.region));
            }
            row("Modem preset", mesh.preset_name());
            row("Hop limit", std::to_string(lora.hop_limit));
            row("Transmit", !lora.tx_enabled ? "disabled" : lora.tx_power == 0 ? "max for region" : std::to_string(lora.tx_power) + " dBm",
                !lora.tx_enabled);
            int channels = 0;
            for (int i = 0; i < kMaxChannels; ++i) channels += mesh.channel_enabled(i) ? 1 : 0;
            row("Channels", std::to_string(channels) + "  (primary: " + mesh.channel_name(0) + ")");

            const meshtastic_NodeInfo* self = mesh.me();
            header("Activity");
            if (self && self->has_device_metrics) {
                const auto& d = self->device_metrics;
                if (d.has_channel_utilization) row("Channel utilization", fmt("%.1f%%", d.channel_utilization));
                if (d.has_air_util_tx) row("Air time (TX)", fmt("%.2f%%", d.air_util_tx));
            }
            if (mesh.has_local_stats) {
                const auto& s = mesh.local_stats;
                row("Packets received", std::to_string(s.num_packets_rx) + "  (" + std::to_string(s.num_packets_rx_bad) + " bad)");
                row("Packets sent", std::to_string(s.num_packets_tx) + "  (" + std::to_string(s.num_tx_relay) + " relayed)");
                row("Nodes online", std::to_string(s.num_online_nodes) + " of " + std::to_string(s.num_total_nodes));
                if (s.noise_floor != 0) row("Noise floor", std::to_string(s.noise_floor) + " dBm");
            } else {
                row("Nodes known", std::to_string(mesh.nodes.size()));
            }
            log_row_ = static_cast<int>(items.size());
            {
                ui::ListItem it;
                it.text      = "Packet log";
                it.has_color = true;
                it.color     = ui::c_accent();
                it.right     = std::to_string(mesh.packet_log.size()) + "  " LV_SYMBOL_RIGHT;
                items.push_back(it);
            }

            header("GPS");
            const auto mode = mesh.config.position.gps_mode;
            if (mesh.config.position.fixed_position) {
                row("Mode", "fixed position");
            } else if (mode != meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
                row("Mode", "off", true);
            } else {
                row("Mode", "enabled");
            }
            double lat, lon;
            if (self && Mesh::node_position(*self, lat, lon)) {
                char buf[48];
                snprintf(buf, sizeof(buf), "%.5f, %.5f", lat, lon);
                row("Position", buf);
                if (self->position.has_altitude) row("Altitude", altitude_str(self->position.altitude, ui::imperial()));
                if (self->position.sats_in_view) row("Satellites", std::to_string(self->position.sats_in_view));
                if (self->position.time) row("Fix age", age_str(self->position.time, now_s()));
            } else if (mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
                row("Position", "waiting for a fix", true);
            }
        }
        list_->set_items(items);
    }

    bool on_key(const Key& key) override
    {
        if (list_->handle_key(key)) return true;
        if (key.k == K::Enter) {
            if (list_->selected() == log_row_) app.push(make_packet_log(app));
            return true;
        }
        return false;
    }

private:
    static std::string region_label(int region)
    {
        // Names follow the protobuf enum; kept short for the status row.
        static const char* names[] = {"UNSET",  "US",     "EU_433", "EU_868", "CN",      "JP",      "ANZ",     "KR",     "TW",
                                      "RU",     "IN",     "NZ_865", "TH",     "LORA_24", "UA_433",  "UA_868",  "MY_433", "MY_919",
                                      "SG_923", "PH_433", "PH_868", "PH_915", "ANZ_433", "KZ_433",  "KZ_863",  "NP_865", "BR_902",
                                      "ITU1_2M", "ITU2_2M", "EU_866", "EU_874", "EU_917", "EU_N_868", "ITU3_2M"};
        if (region >= 0 && region < static_cast<int>(sizeof(names) / sizeof(names[0]))) return names[region];
        return std::to_string(region);
    }

    std::unique_ptr<ui::ListView> list_;
    int log_row_ = -1;
};

class PacketLogScreen : public Screen {
public:
    explicit PacketLogScreen(App& a) : Screen(a)
    {
        list_ = std::make_unique<ui::ListView>(root, 0, 0, kContentW, kContentH);
        list_->set_prefix_width(34);
    }
    std::string title() const override { return LV_SYMBOL_LEFT " Packet log"; }
    std::string hint() const override { return follow_ ? "Up/Down browse   Esc back" : "Paused   Home: live   Esc back"; }

    void refresh() override
    {
        if (seen_rev_ == mesh.rev_log && built_) return;
        if (!follow_ && built_) return;
        seen_rev_ = mesh.rev_log;
        built_    = true;
        std::vector<ui::ListItem> items;
        for (auto e = mesh.packet_log.rbegin(); e != mesh.packet_log.rend(); ++e) {
            ui::ListItem it;
            const time_t t = e->ts;
            tm local{};
            localtime_r(&t, &local);
            char clock[8];
            strftime(clock, sizeof(clock), "%H:%M", &local);
            it.prefix           = clock;
            it.has_prefix_color = true;
            it.prefix_color     = ui::c_dim();
            const std::string to = e->to == kBroadcast ? "all" : mesh.short_name(e->to);
            std::string what     = e->portnum < 0 ? "encrypted" : portnum_name(e->portnum);
            for (auto& c : what) c = static_cast<char>(tolower(c));
            it.text = (e->outgoing ? "me" : mesh.short_name(e->from)) + " " LV_SYMBOL_RIGHT " " + to + "  " + what;
            if (!e->summary.empty()) it.text += ": " + e->summary;
            it.text = ui::clean(it.text, true);
            if (e->outgoing) {
                it.has_color = true;
                it.color     = ui::c_me();
            } else {
                char sig[32] = "";
                if (e->hops > 0) {
                    snprintf(sig, sizeof(sig), "%dh", e->hops);
                } else if (e->snr != 0 || e->rssi != 0) {
                    snprintf(sig, sizeof(sig), "%.0f/%d", e->snr, e->rssi);
                }
                it.right = sig;
            }
            items.push_back(std::move(it));
        }
        if (items.empty()) {
            ui::ListItem it;
            it.text      = "Nothing heard yet";
            it.has_color = true;
            it.color     = ui::c_dim();
            items.push_back(it);
        }
        list_->set_items(items);
    }

    bool on_key(const Key& key) override
    {
        if (key.k == K::Home) {
            follow_ = true;
            built_  = false;
            list_->select(0);
            return true;
        }
        if (list_->handle_key(key)) {
            follow_ = list_->selected() == 0;
            return true;
        }
        return false;
    }

private:
    std::unique_ptr<ui::ListView> list_;
    uint32_t seen_rev_ = 0;
    bool built_        = false;
    bool follow_       = true;
};

const char* const kHelp =
    "KEYS\n"
    "Arrows: move, hold to repeat (hold Fn only while typing)\n"
    "Enter: open, edit, send\n"
    "Esc: back. Hold it for 2 seconds to exit\n"
    "Tab: next page\n"
    "?: in Setup, explain the highlighted setting\n"
    "Settings: Esc asks to save\n\n"
    "MESSAGES\n"
    "Colored tick: delivered. Grey tick: relayed. Cross: failed.\n"
    "To message someone, open them in Nodes.\n"
    "Del on a chat in Chats, or Delete chat history in a message's menu, deletes its messages.\n"
    "M on a chat in Chats mutes it: no sound, no notice, and it is left out of the unread count. The radio "
    "keeps that, so the phone apps see it too.\n"
    "Ctrl+G while writing sends the message with an alert bell, which rings on the other side. Sounds in Setup "
    "chooses which messages make a sound here.\n\n"
    "NODES\n"
    "S changes the order. V chooses which nodes are listed: online, heard directly, favorites and so on. / finds "
    "a node by name or ID as you type; Esc brings all of them back. A node's page shows what its sensors last "
    "sent while MeshZero was open; Request telemetry asks for more.\n\n"
    "MAP\n"
    "On the radar, H makes it turn with the device, so what is ahead of you is at the top; the compass is "
    "calibrated the first time. M switches between the radar, a street map and a topographic map. Enter lets the arrows move the map; 0 "
    "brings it back around you. Units in Setup chooses metres or feet, also for the topographic map; the one in "
    "feet covers the United States. Maps are downloaded as you look at them and kept, so what you have seen "
    "works without a network. Map downloads in Setup turns that off and shows the folder they are kept in; map "
    "tiles copied there as street/zoom/x/y.png, topo/zoom/x/y.png or usgs/zoom/x/y.jpg are used too.\n\n"
    "OTHER APPS\n"
    "When you exit, the radio stops so other apps can use the LoRa cap, unless Run in background is on in Setup. "
    "To use the radio from other apps, set App connection in Setup. Wi-Fi is for MeshMonitor or a phone app on "
    "your network; the address is on the Radio page. Bluetooth (early access) is for a phone app when there is no "
    "Wi-Fi: choose Pair a new phone there, then pick this device in the phone app within two minutes. Only one "
    "of the two is on at a time.\n\n"
    "RADIO OFFLINE\n"
    "Power off, check the cap, power on. For details run:\n"
    "sudo -u meshzero meshzerod doctor\n\n"
    "ABOUT\n"
    "MeshZero " MESHZERO_VERSION;

}  // namespace

std::unique_ptr<Screen> make_radio(App& app)
{
    return std::make_unique<RadioScreen>(app);
}

std::unique_ptr<Screen> make_packet_log(App& app)
{
    return std::make_unique<PacketLogScreen>(app);
}

std::unique_ptr<Screen> make_help(App& app)
{
    return make_about_view(app, LV_SYMBOL_LEFT " Help", kHelp, "https://github.com/s0liton/MeshZero");
}

std::unique_ptr<Screen> make_tab(App& app, int tab)
{
    switch (tab) {
        case kTabNodes:
            return make_nodes(app);
        case kTabMap:
            return make_map(app);
        case kTabRadio:
            return make_radio(app);
        case kTabSetup:
            return make_setup(app);
        default:
            return make_chats(app);
    }
}

}  // namespace mz
