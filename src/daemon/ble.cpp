// SPDX-License-Identifier: GPL-3.0-only
#include "daemon/ble.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#include <poll.h>

#include "common/util.h"

#if MESHZERO_BLUETOOTH
#include <systemd/sd-bus.h>
#endif

namespace mz {

#if MESHZERO_BLUETOOTH

namespace {

// The same service a radio with Bluetooth of its own offers
// (BluetoothCommon.h in the firmware).
constexpr char kServiceUuid[] = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";

enum CharId { kToRadio = 0, kFromRadio, kFromNum, kLogRadio, kCharCount };
struct CharDef {
    const char* uuid;
    const char* path;
};
constexpr char kAppPath[]     = "/org/meshzero/gatt";
constexpr char kServicePath[] = "/org/meshzero/gatt/service0";
constexpr CharDef kChars[kCharCount] = {
    {"f75c76d2-129e-4dad-a1dd-7866124401e7", "/org/meshzero/gatt/service0/char0"},
    {"2c55e69e-4993-11ed-b878-0242ac120002", "/org/meshzero/gatt/service0/char1"},
    {"ed9da18c-a800-4f66-a670-aa7547e34453", "/org/meshzero/gatt/service0/char2"},
    {"5a3d6e49-06e6-4423-9944-e9de8cdf9547", "/org/meshzero/gatt/service0/char3"},
};
constexpr char kAdvPath[]   = "/org/meshzero/advertisement";
constexpr char kAgentPath[] = "/org/meshzero/agent";

constexpr char kBluez[]       = "org.bluez";
constexpr char kCharIface[]   = "org.bluez.GattCharacteristic1";
constexpr uint64_t kRetryMs   = 10000;

}  // namespace

struct Ble::Impl {
    struct CharCtx {
        Impl* self = nullptr;
        int id     = 0;
    };

    sd_bus* bus = nullptr;
    std::string adapter;
    std::string name;
    bool pin = true;
    Handlers h;

    CharCtx ctx[kCharCount];
    bool notifying[kCharCount] = {};
    uint32_t from_num  = 0;
    bool dirty         = false;  // from_num moved since the phones were last told
    bool app_ok        = false;
    bool adv_ok        = false;
    bool adv_wanted    = false;  // phones should be able to find it
    bool adv_pending   = false;  // asked for, not answered yet
    bool pairing       = false;  // a phone that is not paired yet may pair now
    bool registering   = false;
    uint64_t retry_ms  = 0;      // when to offer the service again, 0 for not
    std::string why;
    std::string passkey;
    std::map<std::string, std::string> current;  // per phone: the message it is reading

    // ---- what the bus asks of us -------------------------------------
    static int prop_service_uuid(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                                 sd_bus_error*)
    {
        return sd_bus_message_append(reply, "s", kServiceUuid);
    }
    static int prop_true(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*)
    {
        return sd_bus_message_append(reply, "b", 1);
    }
    static int prop_char_uuid(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* ud,
                              sd_bus_error*)
    {
        return sd_bus_message_append(reply, "s", kChars[static_cast<CharCtx*>(ud)->id].uuid);
    }
    static int prop_char_service(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                                 sd_bus_error*)
    {
        return sd_bus_message_append(reply, "o", kServicePath);
    }
    // Nothing can be read or written before the phone has paired: with a PIN
    // by typing the code, as the firmware demands, without one by agreeing.
    static int prop_char_flags(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* ud,
                               sd_bus_error*)
    {
        const CharCtx* c = static_cast<CharCtx*>(ud);
        const bool pin   = c->self->pin;
        int r            = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0) return r;
        if (c->id == kToRadio) {
            r = sd_bus_message_append(reply, "s", pin ? "encrypt-authenticated-write" : "encrypt-write");
        } else {
            r = sd_bus_message_append(reply, "s", pin ? "encrypt-authenticated-read" : "encrypt-read");
            if (r >= 0 && c->id != kFromRadio) r = sd_bus_message_append(reply, "s", "notify");
        }
        if (r < 0) return r;
        return sd_bus_message_close_container(reply);
    }
    static int prop_char_value(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* ud,
                               sd_bus_error*)
    {
        const CharCtx* c = static_cast<CharCtx*>(ud);
        uint8_t bytes[4] = {};
        size_t len       = 0;
        if (c->id == kFromNum) {
            const uint32_t n = c->self->from_num;
            for (int i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(n >> (8 * i));
            len = 4;
        }
        return sd_bus_message_append_array(reply, 'y', bytes, len);
    }
    static int prop_char_notifying(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* ud,
                                   sd_bus_error*)
    {
        const CharCtx* c = static_cast<CharCtx*>(ud);
        return sd_bus_message_append(reply, "b", c->self->notifying[c->id] ? 1 : 0);
    }

    // The options of a read or write: which phone, and where in the value.
    static int read_options(sd_bus_message* m, std::string& device, uint16_t& offset)
    {
        int r = sd_bus_message_enter_container(m, 'a', "{sv}");
        if (r < 0) return r;
        while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
            const char* key = nullptr;
            if ((r = sd_bus_message_read(m, "s", &key)) < 0) return r;
            if (strcmp(key, "device") == 0) {
                const char* path = nullptr;
                if ((r = sd_bus_message_read(m, "v", "o", &path)) < 0) return r;
                device = path;
            } else if (strcmp(key, "offset") == 0) {
                if ((r = sd_bus_message_read(m, "v", "q", &offset)) < 0) return r;
            } else if ((r = sd_bus_message_skip(m, "v")) < 0) {
                return r;
            }
            if ((r = sd_bus_message_exit_container(m)) < 0) return r;
        }
        if (r < 0) return r;
        return sd_bus_message_exit_container(m);
    }

    static int reply_bytes(sd_bus_message* m, const std::string& value)
    {
        sd_bus_message* reply = nullptr;
        int r                 = sd_bus_message_new_method_return(m, &reply);
        if (r < 0) return r;
        static const uint8_t none = 0;
        r = sd_bus_message_append_array(reply, 'y', value.empty() ? static_cast<const void*>(&none) : value.data(),
                                        value.size());
        if (r >= 0) r = sd_bus_send(nullptr, reply, nullptr);
        sd_bus_message_unref(reply);
        return r;
    }

    static int on_read_value(sd_bus_message* m, void* ud, sd_bus_error* error)
    {
        const CharCtx* c = static_cast<CharCtx*>(ud);
        Impl* self       = c->self;
        std::string device;
        uint16_t offset = 0;
        const int r     = read_options(m, device, offset);
        if (r < 0) return r;
        std::string value;
        if (c->id == kFromRadio) {
            // A message longer than one packet is fetched in pieces; only a
            // read from the start moves on to the next message.
            std::string& current = self->current[device];
            if (offset == 0) current = self->h.on_read ? self->h.on_read(device) : std::string();
            value = current;
        } else if (c->id == kFromNum) {
            for (int i = 0; i < 4; ++i) value.push_back(static_cast<char>(self->from_num >> (8 * i)));
        }
        if (offset > value.size()) {
            return sd_bus_error_set(error, "org.bluez.Error.InvalidOffset", "Offset past the end of the value");
        }
        return reply_bytes(m, value.substr(offset));
    }

    static int on_write_value(sd_bus_message* m, void* ud, sd_bus_error* error)
    {
        const CharCtx* c = static_cast<CharCtx*>(ud);
        Impl* self       = c->self;
        const void* data = nullptr;
        size_t len       = 0;
        int r            = sd_bus_message_read_array(m, 'y', &data, &len);
        if (r < 0) return r;
        const std::string payload(static_cast<const char*>(data), len);
        std::string device;
        uint16_t offset = 0;
        if ((r = read_options(m, device, offset)) < 0) return r;
        if (c->id != kToRadio) return sd_bus_error_set(error, "org.bluez.Error.NotPermitted", "Not writable");
        // Every message arrives whole: the phones ask for packets large enough.
        if (offset != 0) return sd_bus_error_set(error, "org.bluez.Error.InvalidOffset", "Write in pieces");
        if ((r = sd_bus_reply_method_return(m, "")) < 0) return r;
        if (self->h.on_write) self->h.on_write(device, payload);
        return 1;
    }

    static int on_start_notify(sd_bus_message* m, void* ud, sd_bus_error*)
    {
        const CharCtx* c          = static_cast<CharCtx*>(ud);
        c->self->notifying[c->id] = true;
        return sd_bus_reply_method_return(m, "");
    }
    static int on_stop_notify(sd_bus_message* m, void* ud, sd_bus_error*)
    {
        const CharCtx* c          = static_cast<CharCtx*>(ud);
        c->self->notifying[c->id] = false;
        return sd_bus_reply_method_return(m, "");
    }

    // ---- advertisement -----------------------------------------------
    static int prop_adv_type(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*)
    {
        return sd_bus_message_append(reply, "s", "peripheral");
    }
    static int prop_adv_uuids(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                              sd_bus_error*)
    {
        return sd_bus_message_append(reply, "as", 1, kServiceUuid);
    }
    static int prop_adv_name(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* ud,
                             sd_bus_error*)
    {
        return sd_bus_message_append(reply, "s", static_cast<Impl*>(ud)->name.c_str());
    }
    static int on_adv_release(sd_bus_message* m, void* ud, sd_bus_error*)
    {
        // The Bluetooth daemon dropped it (adapter switched off, say): offer it again later.
        Impl* self     = static_cast<Impl*>(ud);
        self->adv_ok   = false;
        if (self->adv_wanted) self->retry_ms = now_ms() + kRetryMs;
        return sd_bus_reply_method_return(m, "");
    }

    // ---- pairing ------------------------------------------------------
    void set_passkey(const std::string& key)
    {
        if (key == passkey) return;
        passkey = key;
        if (h.on_passkey) h.on_passkey(passkey);
    }
    static int on_display_passkey(sd_bus_message* m, void* ud, sd_bus_error* error)
    {
        const char* device = nullptr;
        uint32_t passkey   = 0;
        uint16_t entered   = 0;
        const int r        = sd_bus_message_read(m, "ouq", &device, &passkey, &entered);
        if (r < 0) return r;
        Impl* self = static_cast<Impl*>(ud);
        if (!self->pairing || !self->pin) {
            // Nobody asked for a new phone to be let in: the code is not shown,
            // and the attempt is called off.
            sd_bus_call_method_async(self->bus, nullptr, kBluez, device, "org.bluez.Device1", "CancelPairing",
                                     on_ignored_reply, self, "");
            return sd_bus_error_set(error, "org.bluez.Error.Rejected", "Not pairing now");
        }
        char text[16];
        snprintf(text, sizeof(text), "%06u", passkey % 1000000);
        static_cast<Impl*>(ud)->set_passkey(text);
        return sd_bus_reply_method_return(m, "");
    }
    static int on_display_pincode(sd_bus_message* m, void* ud, sd_bus_error*)
    {
        const char* device = nullptr;
        const char* code   = nullptr;
        const int r        = sd_bus_message_read(m, "os", &device, &code);
        if (r < 0) return r;
        static_cast<Impl*>(ud)->set_passkey(code);
        return sd_bus_reply_method_return(m, "");
    }
    static int on_agent_ok(sd_bus_message* m, void*, sd_bus_error*)
    {
        return sd_bus_reply_method_return(m, "");
    }
    static int on_agent_done(sd_bus_message* m, void* ud, sd_bus_error*)
    {
        static_cast<Impl*>(ud)->set_passkey("");
        return sd_bus_reply_method_return(m, "");
    }
    static int on_agent_reject(sd_bus_message*, void*, sd_bus_error* error)
    {
        return sd_bus_error_set(error, "org.bluez.Error.Rejected", "Pair with the code shown on the device");
    }
    // Pairing without a code being typed: only where no PIN is asked for, and
    // only while a new phone is being let in.
    static int on_agent_confirm(sd_bus_message* m, void* ud, sd_bus_error* error)
    {
        const Impl* self = static_cast<Impl*>(ud);
        if (self->pin || !self->pairing) {
            return sd_bus_error_set(error, "org.bluez.Error.Rejected", "Not pairing now");
        }
        return sd_bus_reply_method_return(m, "");
    }

    // ---- what the Bluetooth daemon tells us ----------------------------
    static int on_properties_changed(sd_bus_message* m, void* ud, sd_bus_error*)
    {
        Impl* self        = static_cast<Impl*>(ud);
        const char* iface = nullptr;
        if (sd_bus_message_read(m, "s", &iface) < 0 || strcmp(iface, "org.bluez.Device1") != 0) return 0;
        const char* path = sd_bus_message_get_path(m);
        int connected = -1, paired = -1;
        if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            if (sd_bus_message_read(m, "s", &key) < 0) return 0;
            int value = 0;
            if (strcmp(key, "Connected") == 0 && sd_bus_message_read(m, "v", "b", &value) >= 0) {
                connected = value;
            } else if (strcmp(key, "Paired") == 0 && sd_bus_message_read(m, "v", "b", &value) >= 0) {
                paired = value;
            } else if (sd_bus_message_skip(m, "v") < 0) {
                return 0;
            }
            sd_bus_message_exit_container(m);
        }
        if (paired == 1) self->set_passkey("");
        if (connected == 0 && path) {
            const std::string device = path;
            self->set_passkey("");
            self->current.erase(device);
            if (self->h.on_disconnect) self->h.on_disconnect(device);
        }
        return 0;
    }
    // The Bluetooth daemon restarted: it has forgotten everything we registered.
    static int on_bluez_owner(sd_bus_message* m, void* ud, sd_bus_error*)
    {
        Impl* self = static_cast<Impl*>(ud);
        const char *name = nullptr, *before = nullptr, *after = nullptr;
        if (sd_bus_message_read(m, "sss", &name, &before, &after) < 0) return 0;
        self->app_ok = self->adv_ok = false;
        self->adv_pending           = false;
        self->registering           = false;
        self->retry_ms              = after && after[0] ? now_ms() + 2000 : 0;
        if (!after || !after[0]) self->why = "the system's Bluetooth service is not running";
        return 0;
    }

    static int on_app_registered(sd_bus_message* m, void* ud, sd_bus_error*)
    {
        Impl* self        = static_cast<Impl*>(ud);
        self->registering = false;
        if (sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            // Left over from an earlier run of this very connection.
            if (e && e->name && strcmp(e->name, "org.bluez.Error.AlreadyExists") == 0) {
                self->app_ok = true;
                return 0;
            }
            self->why      = std::string("cannot offer the service: ") + (e && e->message ? e->message : "unknown error");
            self->retry_ms = now_ms() + kRetryMs;
            return 0;
        }
        self->app_ok = true;
        return 0;
    }
    static int on_adv_registered(sd_bus_message* m, void* ud, sd_bus_error*)
    {
        Impl* self        = static_cast<Impl*>(ud);
        self->adv_pending = false;
        if (sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            if (!(e && e->name && strcmp(e->name, "org.bluez.Error.AlreadyExists") == 0)) {
                self->why      = std::string("cannot advertise: ") + (e && e->message ? e->message : "unknown error");
                self->retry_ms = now_ms() + kRetryMs;
                return 0;
            }
        }
        self->adv_ok = true;
        self->why.clear();
        // Asked to stop while this was on its way.
        if (!self->adv_wanted) self->apply_advertising();
        return 0;
    }
    static int on_ignored_reply(sd_bus_message*, void*, sd_bus_error*)
    {
        return 0;
    }

    static const sd_bus_vtable* service_vtable();
    static const sd_bus_vtable* char_vtable();
    static const sd_bus_vtable* adv_vtable();
    static const sd_bus_vtable* agent_vtable();

    void register_all()
    {
        retry_ms    = 0;
        registering = true;
        // A powered-off adapter refuses everything else.
        sd_bus_call_method_async(bus, nullptr, kBluez, adapter.c_str(), "org.freedesktop.DBus.Properties", "Set",
                                 on_ignored_reply, this, "ssv", "org.bluez.Adapter1", "Powered", "b", 1);
        if (!app_ok) {
            sd_bus_call_method_async(bus, nullptr, kBluez, adapter.c_str(), "org.bluez.GattManager1",
                                     "RegisterApplication", on_app_registered, this, "oa{sv}", kAppPath, 0);
        }
        apply_advertising();
        // DisplayOnly: this side shows six digits and the phone types them.
        // NoInputNoOutput: the phone only asks its user to agree.
        sd_bus_call_method_async(bus, nullptr, kBluez, "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
                                 on_ignored_reply, this, "os", kAgentPath, pin ? "DisplayOnly" : "NoInputNoOutput");
        sd_bus_call_method_async(bus, nullptr, kBluez, "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent",
                                 on_ignored_reply, this, "o", kAgentPath);
    }

    // Puts the advertisement on the air or takes it off, as adv_wanted says.
    void apply_advertising()
    {
        if (!bus || adv_pending) return;
        if (adv_wanted && !adv_ok) {
            adv_pending = true;
            sd_bus_call_method_async(bus, nullptr, kBluez, adapter.c_str(), "org.bluez.LEAdvertisingManager1",
                                     "RegisterAdvertisement", on_adv_registered, this, "oa{sv}", kAdvPath, 0);
        } else if (!adv_wanted && adv_ok) {
            adv_ok = false;
            sd_bus_call_method_async(bus, nullptr, kBluez, adapter.c_str(), "org.bluez.LEAdvertisingManager1",
                                     "UnregisterAdvertisement", on_ignored_reply, this, "o", kAdvPath);
        }
    }
};

const sd_bus_vtable* Ble::Impl::service_vtable()
{
    static const sd_bus_vtable table[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("UUID", "s", prop_service_uuid, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Primary", "b", prop_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_VTABLE_END,
    };
    return table;
}

const sd_bus_vtable* Ble::Impl::char_vtable()
{
    static const sd_bus_vtable table[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("UUID", "s", prop_char_uuid, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Service", "o", prop_char_service, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Flags", "as", prop_char_flags, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Value", "ay", prop_char_value, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Notifying", "b", prop_char_notifying, 0, 0),
        SD_BUS_METHOD("ReadValue", "a{sv}", "ay", on_read_value, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("WriteValue", "aya{sv}", "", on_write_value, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("StartNotify", "", "", on_start_notify, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("StopNotify", "", "", on_stop_notify, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END,
    };
    return table;
}

const sd_bus_vtable* Ble::Impl::adv_vtable()
{
    static const sd_bus_vtable table[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Type", "s", prop_adv_type, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ServiceUUIDs", "as", prop_adv_uuids, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LocalName", "s", prop_adv_name, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Discoverable", "b", prop_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_METHOD("Release", "", "", on_adv_release, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END,
    };
    return table;
}

const sd_bus_vtable* Ble::Impl::agent_vtable()
{
    static const sd_bus_vtable table[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Release", "", "", on_agent_done, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("DisplayPasskey", "ouq", "", on_display_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("DisplayPinCode", "os", "", on_display_pincode, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestPinCode", "o", "s", on_agent_reject, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestPasskey", "o", "u", on_agent_reject, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestConfirmation", "ou", "", on_agent_confirm, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestAuthorization", "o", "", on_agent_confirm, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("AuthorizeService", "os", "", on_agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Cancel", "", "", on_agent_done, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END,
    };
    return table;
}

Ble::Ble() : impl_(std::make_unique<Impl>()) {}

Ble::~Ble()
{
    stop();
}

bool Ble::supported()
{
    return true;
}

std::vector<Ble::Device> Ble::devices()
{
    std::vector<Device> out;
    sd_bus* bus = nullptr;
    if (sd_bus_open_system(&bus) < 0) return out;
    sd_bus_message* reply = nullptr;
    sd_bus_error error    = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus, kBluez, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &error, &reply,
                               "");
    // Every object with its interfaces and their properties: a{oa{sa{sv}}}.
    if (r >= 0) r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    while (r >= 0 && sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
        const char* path = nullptr;
        if (sd_bus_message_read(reply, "o", &path) < 0) break;
        Device d;
        d.path      = path;
        bool device = false;
        if (sd_bus_message_enter_container(reply, 'a', "{sa{sv}}") < 0) break;
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char* iface = nullptr;
            if (sd_bus_message_read(reply, "s", &iface) < 0) break;
            if (strcmp(iface, "org.bluez.Device1") != 0) {
                sd_bus_message_skip(reply, "a{sv}");
            } else {
                device = true;
                sd_bus_message_enter_container(reply, 'a', "{sv}");
                while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                    const char* key  = nullptr;
                    const char* text = nullptr;
                    int flag         = 0;
                    if (sd_bus_message_read(reply, "s", &key) < 0) break;
                    if (strcmp(key, "Address") == 0 && sd_bus_message_read(reply, "v", "s", &text) >= 0) {
                        d.address = text;
                    } else if (strcmp(key, "Alias") == 0 && sd_bus_message_read(reply, "v", "s", &text) >= 0) {
                        d.name = text;
                    } else if (strcmp(key, "Paired") == 0 && sd_bus_message_read(reply, "v", "b", &flag) >= 0) {
                        d.paired = flag != 0;
                    } else if (strcmp(key, "Connected") == 0 && sd_bus_message_read(reply, "v", "b", &flag) >= 0) {
                        d.connected = flag != 0;
                    } else if (strcmp(key, "Address") != 0 && strcmp(key, "Alias") != 0 && strcmp(key, "Paired") != 0 &&
                               strcmp(key, "Connected") != 0) {
                        sd_bus_message_skip(reply, "v");
                    }
                    sd_bus_message_exit_container(reply);
                }
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
        if (device) out.push_back(std::move(d));
    }
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_flush_close_unref(bus);
    return out;
}

bool Ble::remove_device(const std::string& path)
{
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return false;
    sd_bus* bus = nullptr;
    if (sd_bus_open_system(&bus) < 0) return false;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    const int r = sd_bus_call_method(bus, kBluez, path.substr(0, slash).c_str(), "org.bluez.Adapter1", "RemoveDevice",
                                     &error, nullptr, "o", path.c_str());
    sd_bus_error_free(&error);
    sd_bus_flush_close_unref(bus);
    return r >= 0;
}

bool Ble::start(const std::string& name, bool pin, Handlers handlers)
{
    stop();
    Impl& s   = *impl_;
    s.name    = name;
    s.pin     = pin;
    s.h       = std::move(handlers);
    s.why.clear();
    const char* adapter = getenv("MESHZERO_BT_ADAPTER");
    s.adapter           = adapter && adapter[0] ? adapter : "/org/bluez/hci0";

    // MESHZERO_BT_BUS=user puts the service on the user's bus, where a test can
    // play the Bluetooth daemon's part.
    const char* which = getenv("MESHZERO_BT_BUS");
    int r = which && strcmp(which, "user") == 0 ? sd_bus_open_user(&s.bus) : sd_bus_open_system(&s.bus);
    if (r < 0) {
        s.why = std::string("cannot reach the system bus: ") + strerror(-r);
        s.bus = nullptr;
        return false;
    }
    for (int i = 0; i < kCharCount; ++i) {
        s.ctx[i].self = &s;
        s.ctx[i].id   = i;
        s.notifying[i] = false;
    }
    r = sd_bus_add_object_manager(s.bus, nullptr, kAppPath);
    if (r >= 0) r = sd_bus_add_object_vtable(s.bus, nullptr, kServicePath, "org.bluez.GattService1", Impl::service_vtable(), &s);
    for (int i = 0; r >= 0 && i < kCharCount; ++i) {
        r = sd_bus_add_object_vtable(s.bus, nullptr, kChars[i].path, kCharIface, Impl::char_vtable(), &s.ctx[i]);
    }
    if (r >= 0) r = sd_bus_add_object_vtable(s.bus, nullptr, kAdvPath, "org.bluez.LEAdvertisement1", Impl::adv_vtable(), &s);
    if (r >= 0) r = sd_bus_add_object_vtable(s.bus, nullptr, kAgentPath, "org.bluez.Agent1", Impl::agent_vtable(), &s);
    if (r >= 0) {
        r = sd_bus_match_signal(s.bus, nullptr, kBluez, nullptr, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                Impl::on_properties_changed, &s);
    }
    if (r >= 0) {
        r = sd_bus_add_match(s.bus, nullptr,
                             "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                             "member='NameOwnerChanged',arg0='org.bluez'",
                             Impl::on_bluez_owner, &s);
    }
    if (r < 0) {
        s.why = std::string("cannot set up the Bluetooth service: ") + strerror(-r);
        stop();
        return false;
    }
    s.app_ok = s.adv_ok = false;
    s.adv_pending = s.adv_wanted = s.pairing = false;
    s.register_all();
    return true;
}

void Ble::stop()
{
    Impl& s = *impl_;
    if (!s.bus) return;
    // Closing the connection takes the service, the advertisement and the
    // pairing agent with it: the Bluetooth daemon drops what a client leaves behind.
    sd_bus_flush_close_unref(s.bus);
    s.bus    = nullptr;
    s.app_ok = s.adv_ok = false;
    s.registering       = false;
    s.retry_ms          = 0;
    s.current.clear();
    if (!s.passkey.empty()) {
        s.passkey.clear();
        if (s.h.on_passkey) s.h.on_passkey("");
    }
}

bool Ble::running() const
{
    return impl_->bus != nullptr;
}

bool Ble::advertising() const
{
    return impl_->bus && impl_->app_ok && impl_->adv_ok;
}

bool Ble::ready() const
{
    return impl_->bus && impl_->app_ok;
}

void Ble::set_advertising(bool on)
{
    impl_->adv_wanted = on;
    impl_->apply_advertising();
}

void Ble::set_pairing(bool open)
{
    impl_->pairing = open;
    if (!open) impl_->set_passkey("");
}

const std::string& Ble::why() const
{
    return impl_->why;
}

int Ble::fd() const
{
    return impl_->bus ? sd_bus_get_fd(impl_->bus) : -1;
}

short Ble::events() const
{
    if (!impl_->bus) return 0;
    const int e = sd_bus_get_events(impl_->bus);
    return e < 0 ? static_cast<short>(POLLIN) : static_cast<short>(e);
}

int Ble::timeout_ms() const
{
    if (!impl_->bus) return -1;
    uint64_t until = 0;
    if (sd_bus_get_timeout(impl_->bus, &until) < 0 || until == UINT64_MAX) return -1;
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000;
    return until <= now ? 0 : static_cast<int>((until - now) / 1000 + 1);
}

void Ble::process()
{
    Impl& s = *impl_;
    if (!s.bus) return;
    for (int guard = 0; guard < 256; ++guard) {
        const int r = sd_bus_process(s.bus, nullptr);
        if (r < 0) {
            // The bus itself is gone; the owner starts over when it next looks.
            s.why = std::string("lost the system bus: ") + strerror(-r);
            stop();
            return;
        }
        if (r == 0) break;
    }
    if (s.retry_ms != 0 && now_ms() >= s.retry_ms && !s.registering) s.register_all();
    if (s.dirty) {
        s.dirty = false;
        if (s.notifying[kFromNum]) {
            sd_bus_emit_properties_changed(s.bus, kChars[kFromNum].path, kCharIface, "Value", nullptr);
        }
    }
    sd_bus_flush(s.bus);
}

void Ble::notify()
{
    ++impl_->from_num;
    impl_->dirty = true;
}

void Ble::disconnect(const std::string& device)
{
    Impl& s = *impl_;
    if (!s.bus || device.empty()) return;
    s.current.erase(device);
    sd_bus_call_method_async(s.bus, nullptr, kBluez, device.c_str(), "org.bluez.Device1", "Disconnect",
                             Impl::on_ignored_reply, &s, "");
}

#else  // no Bluetooth in this build

struct Ble::Impl {
    std::string why = "this build has no Bluetooth support";
};

Ble::Ble() : impl_(std::make_unique<Impl>()) {}
Ble::~Ble() = default;
bool Ble::supported()
{
    return false;
}
std::vector<Ble::Device> Ble::devices()
{
    return {};
}
bool Ble::remove_device(const std::string&)
{
    return false;
}
bool Ble::start(const std::string&, bool, Handlers)
{
    return false;
}
void Ble::stop() {}
bool Ble::running() const
{
    return false;
}
bool Ble::advertising() const
{
    return false;
}
bool Ble::ready() const
{
    return false;
}
void Ble::set_advertising(bool) {}
void Ble::set_pairing(bool) {}
const std::string& Ble::why() const
{
    return impl_->why;
}
int Ble::fd() const
{
    return -1;
}
short Ble::events() const
{
    return 0;
}
int Ble::timeout_ms() const
{
    return -1;
}
void Ble::process() {}
void Ble::notify() {}
void Ble::disconnect(const std::string&) {}

#endif

}  // namespace mz
