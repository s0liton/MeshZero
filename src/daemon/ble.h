// SPDX-License-Identifier: GPL-3.0-only
// The Meshtastic Bluetooth service, offered through the system's Bluetooth
// daemon (BlueZ): the phone apps find and use this device the way they do a
// radio with Bluetooth of its own.
//
// A phone writes one ToRadio message at a time and reads FromRadio messages
// one at a time until it gets an empty one; a counter it is notified of tells
// it when there is more to read.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mz {

class Ble {
public:
    struct Handlers {
        // A message from the phone. `device` names the phone for as long as it is connected.
        std::function<void(const std::string& device, const std::string& payload)> on_write;
        // The next message for the phone, empty when there is none.
        std::function<std::string(const std::string& device)> on_read;
        std::function<void(const std::string& device)> on_disconnect;
        // The six digits to type on the phone while it pairs; empty once that is over.
        std::function<void(const std::string& passkey)> on_passkey;
    };

    Ble();
    ~Ble();
    Ble(const Ble&)            = delete;
    Ble& operator=(const Ble&) = delete;

    // Whether this build can do Bluetooth at all.
    static bool supported();

    // A device the system's Bluetooth daemon knows, phone or otherwise.
    struct Device {
        std::string path;     // what the daemon calls it, as in on_write
        std::string address;  // its own address once paired, which stays the same
        std::string name;
        bool paired    = false;
        bool connected = false;
    };
    static std::vector<Device> devices();
    // Makes the Bluetooth daemon forget a device, its pairing included.
    static bool remove_device(const std::string& path);

    // Starts offering the service under this name. A phone has to pair before
    // it can use it: with `pin` by typing a code shown here, without by just
    // agreeing on the phone. Nothing is advertised and no pairing is accepted
    // until set_advertising() and set_pairing() say so. False when the
    // system's Bluetooth cannot be reached; why() then says more.
    bool start(const std::string& name, bool pin, Handlers handlers);
    void stop();
    bool running() const;
    // Whether phones can find it: needed for a new phone to pair, and for a
    // paired one to come back.
    void set_advertising(bool on);
    // Whether a phone that is not paired yet may pair now.
    void set_pairing(bool open);
    // The service is registered with the Bluetooth daemon.
    bool ready() const;
    // Advertised and accepted by the Bluetooth daemon: phones can find it.
    bool advertising() const;
    const std::string& why() const;

    // For the caller's poll loop: the descriptor to wait on (-1 when stopped),
    // what to wait for, how long at most, and the work to do afterwards.
    int fd() const;
    short events() const;
    int timeout_ms() const;
    void process();

    // Tells the phones there is something new to read.
    void notify();
    // Ends a phone's connection, as a radio restarting would.
    void disconnect(const std::string& device);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mz
