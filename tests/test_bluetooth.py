#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Checks the Bluetooth service of meshzerod without a Bluetooth adapter.

The service is put on the user's D-Bus (MESHZERO_BT_BUS=user) and this test
plays the part of the system's Bluetooth daemon: it lists the service the way
BlueZ does and reads and writes its characteristics on behalf of two phones.

    tests/test_bluetooth.py build/host/meshzerod

Needs `pip install meshtastic dbus-fast` and a D-Bus session bus.
"""
import asyncio
import os
import struct
import subprocess
import sys
import tempfile
import time

from dbus_fast import BusType, Message, MessageType, Variant
from dbus_fast.aio import MessageBus
from meshtastic.protobuf import mesh_pb2, portnums_pb2

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = "/org/meshzero/gatt"
CHAR = "org.bluez.GattCharacteristic1"
TORADIO = "f75c76d2-129e-4dad-a1dd-7866124401e7"
FROMRADIO = "2c55e69e-4993-11ed-b878-0242ac120002"
FROMNUM = "ed9da18c-a800-4f66-a670-aa7547e34453"
PHONE_A = "/org/bluez/hci0/dev_AA_AA_AA_AA_AA_AA"
PHONE_B = "/org/bluez/hci0/dev_BB_BB_BB_BB_BB_BB"

failures = 0


def check(cond, what):
    global failures
    print(("ok: " if cond else "FAIL: ") + what, flush=True)
    if not cond:
        failures += 1


async def call(bus, dest, path, iface, member, signature="", body=None):
    reply = await bus.call(Message(destination=dest, path=path, interface=iface, member=member,
                                   signature=signature, body=body or []))
    if reply.message_type == MessageType.ERROR:
        raise RuntimeError("%s: %s" % (reply.error_name, reply.body))
    return reply.body


async def find_service(bus, pid):
    names = (await call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames"))[0]
    for name in names:
        if not name.startswith(":"):
            continue
        try:
            owner = (await call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                "GetConnectionUnixProcessID", "s", [name]))[0]
        except RuntimeError:
            continue
        if owner == pid:
            return name
    return None


async def read(bus, dest, path, phone, offset=0):
    options = {"device": Variant("o", phone), "offset": Variant("q", offset)}
    return bytes((await call(bus, dest, path, CHAR, "ReadValue", "a{sv}", [options]))[0])


async def write(bus, dest, path, phone, data):
    await call(bus, dest, path, CHAR, "WriteValue", "aya{sv}", [data, {"device": Variant("o", phone)}])


async def drain(bus, dest, path, phone):
    out = []
    while True:
        data = await read(bus, dest, path, phone)
        if not data:
            return out
        msg = mesh_pb2.FromRadio()
        msg.ParseFromString(data)
        out.append(msg)


async def run(daemon_pid):
    bus = await MessageBus(bus_type=BusType.SESSION).connect()
    dest = None
    for _ in range(50):
        dest = await find_service(bus, daemon_pid)
        if dest:
            try:
                await call(bus, dest, APP, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")
                break
            except RuntimeError:
                dest = None
        await asyncio.sleep(0.2)
    check(dest is not None, "the service is on the bus")
    if not dest:
        return

    objects = (await call(bus, dest, APP, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"))[0]
    services = {p: i["org.bluez.GattService1"] for p, i in objects.items() if "org.bluez.GattService1" in i}
    chars = {i[CHAR]["UUID"].value: (p, i[CHAR]) for p, i in objects.items() if CHAR in i}
    check(len(services) == 1 and list(services.values())[0]["UUID"].value == "6ba1b218-15a8-461f-9fa8-5dcae273eafd"
          and list(services.values())[0]["Primary"].value, "one primary Meshtastic service is listed")
    check({TORADIO, FROMRADIO, FROMNUM} <= set(chars), "ToRadio, FromRadio and FromNum are listed")
    check(chars[TORADIO][1]["Flags"].value == ["encrypt-write"] and chars[FROMRADIO][1]["Flags"].value == ["encrypt-read"]
          and chars[FROMNUM][1]["Flags"].value == ["encrypt-read", "notify"],
          "without a code they still take a paired, encrypted connection")
    check(all(c[1]["Service"].value == list(services)[0] for c in chars.values()), "they belong to the service")
    to_radio, from_radio, from_num = chars[TORADIO][0], chars[FROMRADIO][0], chars[FROMNUM][0]

    # Notifications reach BlueZ as changes of the counter's value.
    counts = []

    def on_message(msg):
        if msg.message_type == MessageType.SIGNAL and msg.member == "PropertiesChanged" and msg.path == from_num:
            value = msg.body[1].get("Value")
            if value is not None:
                counts.append(struct.unpack("<I", bytes(value.value))[0])

    bus.add_message_handler(on_message)
    await call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s",
               ["type='signal',member='PropertiesChanged',path='%s'" % from_num])
    await call(bus, dest, from_num, CHAR, "StartNotify")

    check(await read(bus, dest, from_radio, PHONE_A) == b"", "nothing to read before anything was asked")
    await write(bus, dest, to_radio, PHONE_A, mesh_pb2.ToRadio(want_config_id=4242).SerializeToString())
    await asyncio.sleep(1.5)
    config = await drain(bus, dest, from_radio, PHONE_A)
    kinds = [m.WhichOneof("payload_variant") for m in config]
    check(kinds and kinds[0] == "my_info" and kinds[-1] == "config_complete_id" and config[-1].config_complete_id == 4242,
          "a phone gets the whole configuration, ending with its own request number")
    check(kinds.count("node_info") == 9 and kinds.count("channel") == 8, "with every node and channel")
    told = [m.config.bluetooth for m in config if m.WhichOneof("payload_variant") == "config"
            and m.config.WhichOneof("payload_variant") == "bluetooth"]
    check(len(told) == 1 and told[0].enabled and told[0].mode == 2,
          "and is told Bluetooth is on without a PIN, as it is here, not what the radio daemon says")
    check(len(counts) > 0, "and was told there was something to read")
    check(struct.unpack("<I", await read(bus, dest, from_num, PHONE_A))[0] == counts[-1],
          "the counter reads as it was last announced")

    # A second phone is served on its own.
    await write(bus, dest, to_radio, PHONE_B, mesh_pb2.ToRadio(want_config_id=7).SerializeToString())
    await asyncio.sleep(1.0)
    check(await read(bus, dest, from_radio, PHONE_A) == b"", "the first phone gets nothing of a second phone's configuration")
    second = await drain(bus, dest, from_radio, PHONE_B)
    check(second and second[-1].config_complete_id == 7, "which gets all of it")

    # A long message is fetched in pieces without moving on to the next one.
    await write(bus, dest, to_radio, PHONE_A, mesh_pb2.ToRadio(want_config_id=9).SerializeToString())
    await asyncio.sleep(1.0)
    first = await read(bus, dest, from_radio, PHONE_A)
    rest = await read(bus, dest, from_radio, PHONE_A, offset=3)
    check(len(first) > 3 and rest == first[3:], "reading on from an offset continues the same message")
    await drain(bus, dest, from_radio, PHONE_A)

    # What one phone sends, the other sees, and the mesh answers both.
    packet = mesh_pb2.ToRadio()
    packet.packet.to = 0xFFFFFFFF
    packet.packet.want_ack = True
    packet.packet.decoded.portnum = portnums_pb2.TEXT_MESSAGE_APP
    packet.packet.decoded.payload = b"hello over bluetooth"
    await write(bus, dest, to_radio, PHONE_A, packet.SerializeToString())
    await asyncio.sleep(3.0)
    seen_b = await drain(bus, dest, from_radio, PHONE_B)
    seen_a = await drain(bus, dest, from_radio, PHONE_A)
    texts_b = [m.packet.decoded.payload for m in seen_b if m.WhichOneof("payload_variant") == "packet"
               and m.packet.decoded.portnum == portnums_pb2.TEXT_MESSAGE_APP]
    check(b"hello over bluetooth" in texts_b, "a message sent by one phone is shown to the other")
    acks = [m for m in seen_a if m.WhichOneof("payload_variant") == "packet"
            and m.packet.decoded.portnum == portnums_pb2.ROUTING_APP]
    check(len(acks) >= 1, "and its acknowledgement comes back to the sender")

    try:
        await write(bus, dest, from_radio, PHONE_A, b"x")
        check(False, "writing to FromRadio is refused")
    except RuntimeError:
        check(True, "writing to FromRadio is refused")
    bus.disconnect()


def main():
    daemon = sys.argv[1]
    if "DBUS_SESSION_BUS_ADDRESS" not in os.environ:
        print("no D-Bus session bus; skipping")
        return 0
    state = tempfile.mkdtemp(prefix="mzbt")
    for name, value in (("bluetooth", "1"), ("bluetooth-pin", "0"), ("bluetooth-name", "TEST_0001")):
        with open(os.path.join(state, name), "w") as f:
            f.write(value + "\n")
    radio = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "fakeradio.py"), "--port", "4445",
                              "--quiet-mesh"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    env = dict(os.environ, MESHZERO_STATE_DIR=state, MESHZERO_BT_BUS="user")
    proxy = subprocess.Popen([daemon, "serve", "--listen", "127.0.0.1", "--port", "4446", "--radio-port", "4445"],
                             env=env, stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(2.5)
        asyncio.run(run(proxy.pid))
    finally:
        proxy.terminate()
        radio.terminate()
        log = proxy.communicate(timeout=5)[1]
    check("client connected: bluetooth dev_AA_AA_AA_AA_AA_AA" in log, "the service logs the phone as a client")
    if failures:
        print(log)
    print("all bluetooth tests passed" if failures == 0 else "%d bluetooth checks failed" % failures)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
