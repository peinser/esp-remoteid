#!/usr/bin/env python3
"""MAVLink test driver for the ESP32-S3 Remote ID firmware.

This feeds the firmware a realistic OpenDroneID MAVLink stream (so reception can
be verified in an OpenDroneID phone app) and concurrently checks the firmware's
own outbound messages (HEARTBEAT / OPEN_DRONE_ID_ARM_STATUS / SYSTEM_UPDATE).

It exercises every MAVLink feature the firmware implements:

  Input (firmware consumes)        Output (firmware produces)
  -------------------------        --------------------------
  HEARTBEAT (sysid adoption,       HEARTBEAT (MAV_TYPE_ODID)
            takeoff capture)       OPEN_DRONE_ID_ARM_STATUS
  SYSTEM_TIME (loop time seed)     OPEN_DRONE_ID_SYSTEM_UPDATE (fixed-operator loop)
  OPEN_DRONE_ID_BASIC_ID
  OPEN_DRONE_ID_OPERATOR_ID
  OPEN_DRONE_ID_SELF_ID
  OPEN_DRONE_ID_SYSTEM
  OPEN_DRONE_ID_SYSTEM_UPDATE
  OPEN_DRONE_ID_LOCATION
  OPEN_DRONE_ID_AUTHENTICATION
  OPEN_DRONE_ID_MESSAGE_PACK

Connect either directly to the device's USB Serial/JTAG port or to a
mavlink-router endpoint:

  python remoteid_test.py --device /dev/cu.usbmodem1101 drive --operator fixed
  python remoteid_test.py --device udpout:127.0.0.1:14555 drive --operator live
  python remoteid_test.py --device /dev/cu.usbmodem1101 selftest

See ./README.md for details.
"""

import argparse
import math
import os
import sys
import threading
import time

# OpenDroneID message IDs are > 255, so MAVLink v2 framing is mandatory.
os.environ.setdefault("MAVLINK20", "1")

try:
    from pymavlink import mavutil
    from pymavlink.dialects.v20 import ardupilotmega as mavlink
except ImportError:
    sys.exit("pymavlink is required: pip install -r requirements.txt")

if not hasattr(mavlink, "MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM_UPDATE"):
    sys.exit("This pymavlink build lacks the OpenDroneID messages; upgrade pymavlink.")

# ASTM/ODID epoch: 2019-01-01T00:00:00Z as a Unix timestamp.
ODID_EPOCH_2019 = 1546300800
ODID_MESSAGE_SIZE = 25

# Component IDs an OpenDroneID transmitter module uses (MAV_COMP_ID_ODID_TXRX_1..3),
# plus 100 which this project's example config happens to use. Output from any of
# these (or simply not our own component) is treated as coming from the device.
DEVICE_COMPONENTS = {100, 236, 237, 238}


def _bytes(value, length):
    """Encode a str/bytes into a fixed-length, null-padded byte field."""
    if isinstance(value, str):
        value = value.encode()
    return bytes(value[:length]).ljust(length, b"\x00")


def odid_timestamp_now():
    """Seconds since the ODID 2019 epoch (used by SYSTEM / SYSTEM_UPDATE)."""
    return int(time.time()) - ODID_EPOCH_2019


def hour_timestamp_now():
    """Seconds after the current UTC hour (used by LOCATION)."""
    return time.time() % 3600.0


# --------------------------------------------------------------------------- #
# Raw ASTM encoders for the OPEN_DRONE_ID_MESSAGE_PACK payload.
# The pack carries concatenated 25-byte ASTM messages (not nested MAVLink), so a
# few text-based message types are encoded here by hand. Byte 0 is
# (MessageType << 4) | ProtocolVersion; protocol version 2 (F3411-22a).
# --------------------------------------------------------------------------- #

def encode_basic_id(uas_id, id_type, ua_type):
    b = bytearray(ODID_MESSAGE_SIZE)
    b[0] = (0 << 4) | 2                       # BASIC_ID
    b[1] = ((id_type & 0xF) << 4) | (ua_type & 0xF)
    b[2:22] = _bytes(uas_id, 20)
    return bytes(b)


def encode_operator_id(operator_id, operator_id_type=0):
    b = bytearray(ODID_MESSAGE_SIZE)
    b[0] = (5 << 4) | 2                        # OPERATOR_ID
    b[1] = operator_id_type & 0xFF
    b[2:22] = _bytes(operator_id, 20)
    return bytes(b)


def encode_self_id(description, description_type=0):
    b = bytearray(ODID_MESSAGE_SIZE)
    b[0] = (3 << 4) | 2                        # SELF_ID
    b[1] = description_type & 0xFF
    b[2:25] = _bytes(description, 23)
    return bytes(b)


# --------------------------------------------------------------------------- #
# Device output monitor
# --------------------------------------------------------------------------- #

class DeviceMonitor(threading.Thread):
    """Background reader that records the firmware's outbound messages."""

    def __init__(self, conn, our_component):
        super().__init__(daemon=True)
        self.conn = conn
        self.our_component = our_component
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.heartbeat = None        # (system_status, time)
        self.arm_status = None       # (status, error, time)
        self.system_update = None    # (lat, lon, alt, timestamp, time)
        self.counts = {"HEARTBEAT": 0, "ARM_STATUS": 0, "SYSTEM_UPDATE": 0}

    def _is_device(self, msg):
        comp = msg.get_srcComponent()
        return comp in DEVICE_COMPONENTS or comp != self.our_component

    def run(self):
        while not self.stop_event.is_set():
            msg = self.conn.recv_match(blocking=True, timeout=0.5)
            if msg is None:
                continue
            t = msg.get_type()
            now = time.time()
            with self.lock:
                if t == "HEARTBEAT" and getattr(msg, "type", None) == mavlink.MAV_TYPE_ODID:
                    self.heartbeat = (msg.system_status, now)
                    self.counts["HEARTBEAT"] += 1
                elif t == "OPEN_DRONE_ID_ARM_STATUS":
                    err = msg.error
                    if isinstance(err, (bytes, bytearray)):
                        err = bytes(err).decode(errors="replace")
                    err = err.split("\x00", 1)[0]
                    self.arm_status = (msg.status, err, now)
                    self.counts["ARM_STATUS"] += 1
                elif t == "OPEN_DRONE_ID_SYSTEM_UPDATE" and self._is_device(msg):
                    self.system_update = (
                        msg.operator_latitude, msg.operator_longitude,
                        msg.operator_altitude_geo, msg.timestamp, now,
                    )
                    self.counts["SYSTEM_UPDATE"] += 1

    def snapshot(self):
        with self.lock:
            return dict(
                heartbeat=self.heartbeat,
                arm_status=self.arm_status,
                system_update=self.system_update,
                counts=dict(self.counts),
            )

    def status_line(self):
        s = self.snapshot()
        now = time.time()
        parts = []
        hb = s["heartbeat"]
        parts.append(f"HB:{'ok' if hb and now - hb[1] < 3 else '--'}")
        arm = s["arm_status"]
        if arm:
            label = "GOOD_TO_ARM" if arm[0] == 0 else f"PRE_ARM_FAIL({arm[1]})"
            parts.append(f"ARM:{label}")
        else:
            parts.append("ARM:--")
        su = s["system_update"]
        if su:
            age = now - su[4]
            parts.append(f"SYS_UPD:{su[0]/1e7:.5f},{su[1]/1e7:.5f} {age:.1f}s ago")
        else:
            parts.append("SYS_UPD:--")
        return " | ".join(parts)

    def stop(self):
        self.stop_event.set()


# --------------------------------------------------------------------------- #
# Message senders
# --------------------------------------------------------------------------- #

class Driver:
    def __init__(self, conn, args):
        self.conn = conn
        self.a = args
        self.id_or_mac = bytes(20)

    def heartbeat(self, armed=False):
        base = mavlink.MAV_MODE_FLAG_SAFETY_ARMED if armed else 0
        self.conn.mav.heartbeat_send(
            mavlink.MAV_TYPE_QUADROTOR, mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA,
            base, 0, mavlink.MAV_STATE_ACTIVE)

    def system_time(self):
        self.conn.mav.system_time_send(int(time.time() * 1e6), int(time.monotonic() * 1e3))

    def basic_id(self):
        self.conn.mav.open_drone_id_basic_id_send(
            self.a.target_system, self.a.target_component, self.id_or_mac,
            mavlink.MAV_ODID_ID_TYPE_SERIAL_NUMBER,
            mavlink.MAV_ODID_UA_TYPE_HELICOPTER_OR_MULTIROTOR,
            _bytes(self.a.uas_id, 20))

    def operator_id(self):
        self.conn.mav.open_drone_id_operator_id_send(
            self.a.target_system, self.a.target_component, self.id_or_mac,
            mavlink.MAV_ODID_OPERATOR_ID_TYPE_CAA, _bytes(self.a.operator_id, 20))

    def self_id(self):
        self.conn.mav.open_drone_id_self_id_send(
            self.a.target_system, self.a.target_component, self.id_or_mac,
            mavlink.MAV_ODID_DESC_TYPE_TEXT, _bytes(self.a.self_id, 23))

    def system(self, location_type):
        self.conn.mav.open_drone_id_system_send(
            self.a.target_system, self.a.target_component, self.id_or_mac,
            location_type, mavlink.MAV_ODID_CLASSIFICATION_TYPE_EU,
            int(self.a.operator_lat * 1e7), int(self.a.operator_lon * 1e7),
            1, 0, -1000.0, -1000.0,
            mavlink.MAV_ODID_CATEGORY_EU_OPEN, mavlink.MAV_ODID_CLASS_EU_CLASS_1,
            float(self.a.operator_alt), odid_timestamp_now())

    def system_update(self, lat, lon, alt):
        self.conn.mav.open_drone_id_system_update_send(
            self.a.target_system, self.a.target_component,
            int(lat * 1e7), int(lon * 1e7), float(alt), odid_timestamp_now())

    def location(self, lat, lon, alt, direction_deg, speed_h, speed_v=0.0):
        # MAVLink wire units: direction in cdeg, speeds in cm/s.
        self.conn.mav.open_drone_id_location_send(
            self.a.target_system, self.a.target_component, self.id_or_mac,
            mavlink.MAV_ODID_STATUS_AIRBORNE,
            int(direction_deg * 100) % 36000, int(speed_h * 100), int(speed_v * 100),
            int(lat * 1e7), int(lon * 1e7), -1000.0, float(alt),
            mavlink.MAV_ODID_HEIGHT_REF_OVER_TAKEOFF, float(alt),
            mavlink.MAV_ODID_HOR_ACC_3_METER, mavlink.MAV_ODID_VER_ACC_10_METER,
            mavlink.MAV_ODID_VER_ACC_UNKNOWN,
            mavlink.MAV_ODID_SPEED_ACC_3_METERS_PER_SECOND,
            hour_timestamp_now(), mavlink.MAV_ODID_TIME_ACC_1_0_SECOND)

    def authentication(self, length=64):
        # Split a dummy signature of `length` bytes across the correct number of
        # pages: page 0 carries 17 bytes, each later page 23. `length` and
        # `last_page_index` must agree or receivers reject the auth as incomplete.
        sig = bytes((i * 7) & 0xFF for i in range(length))
        extra = 0 if length <= 17 else -(-(length - 17) // 23)   # ceil division
        last = extra
        ts = int(time.time()) - ODID_EPOCH_2019
        for page in range(last + 1):
            if page == 0:
                data = _bytes(sig[:17], 23)
                self.conn.mav.open_drone_id_authentication_send(
                    self.a.target_system, self.a.target_component, self.id_or_mac,
                    mavlink.MAV_ODID_AUTH_TYPE_UAS_ID_SIGNATURE, 0, last, length, ts, data)
            else:
                off = 17 + (page - 1) * 23
                data = _bytes(sig[off:off + 23], 23)
                self.conn.mav.open_drone_id_authentication_send(
                    self.a.target_system, self.a.target_component, self.id_or_mac,
                    mavlink.MAV_ODID_AUTH_TYPE_UAS_ID_SIGNATURE, page, 0, 0, 0, data)

    def message_pack(self):
        msgs = [
            encode_basic_id(self.a.uas_id, mavlink.MAV_ODID_ID_TYPE_SERIAL_NUMBER,
                            mavlink.MAV_ODID_UA_TYPE_HELICOPTER_OR_MULTIROTOR),
            encode_operator_id(self.a.operator_id),
            encode_self_id(self.a.self_id),
        ]
        payload = b"".join(msgs).ljust(225, b"\x00")
        self.conn.mav.open_drone_id_message_pack_send(
            self.a.target_system, self.a.target_component, self.id_or_mac,
            ODID_MESSAGE_SIZE, len(msgs), payload)

    def identity(self):
        self.basic_id()
        self.operator_id()
        self.self_id()


# --------------------------------------------------------------------------- #
# Scenarios
# --------------------------------------------------------------------------- #

def moving_position(args, t):
    """Circular flight path of radius args.radius; returns full kinematic state
    (lat, lon, alt, direction_deg, speed_h, speed_v)."""
    omega = args.speed / max(args.radius, 1.0)        # rad/s
    theta = omega * t
    dlat = (args.radius / 111320.0) * math.cos(theta)
    dlon = (args.radius / (111320.0 * math.cos(math.radians(args.lat)))) * math.sin(theta)
    lat = args.lat + dlat
    lon = args.lon + dlon
    direction = (math.degrees(theta) + 90.0) % 360.0  # tangent to the circle
    alt = args.alt + 5.0 * math.sin(theta)
    speed_v = 5.0 * omega * math.cos(theta)           # d(alt)/dt, m/s (climb +)
    return lat, lon, alt, direction, args.speed, speed_v


def run_periodic(driver, monitor, args, per_tick):
    """Drive the firmware until interrupted, printing device status each second."""
    start = time.time()
    last_print = 0.0
    tick = 0
    try:
        while True:
            now = time.time() - start
            per_tick(driver, now, tick)
            if time.time() - last_print >= 1.0:
                print(f"[{now:6.1f}s] {monitor.status_line()}")
                last_print = time.time()
            tick += 1
            time.sleep(args.location_period)
    except KeyboardInterrupt:
        print("\nstopped.")


def scenario_drive(driver, monitor, args):
    print(f"Driving firmware: operator={args.operator}, location {args.location_rate} Hz. Ctrl-C to stop.")
    print("Open the OpenDroneID app; the drone should appear and move.\n")
    state = {"last_slow": -10.0}

    def per_tick(d, now, tick):
        d.location(*moving_position(args, now))
        # ~1 Hz: heartbeat, time seed, identity, operator location.
        if now - state["last_slow"] >= 1.0:
            state["last_slow"] = now
            d.heartbeat(armed=args.armed)
            d.system_time()
            d.identity()
            if args.operator == "fixed":
                d.system(mavlink.MAV_ODID_OPERATOR_LOCATION_TYPE_FIXED)
            elif args.operator == "live":
                d.system(mavlink.MAV_ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS)
                lat, lon, alt = moving_position(args, now)[:3]
                d.system_update(lat, lon, alt)   # live stream follows the drone

    run_periodic(driver, monitor, args, per_tick)


def scenario_pack(driver, monitor, args):
    print("Sending identity via OPEN_DRONE_ID_MESSAGE_PACK + individual LOCATION/SYSTEM.\n")

    def per_tick(d, now, tick):
        d.location(*moving_position(args, now))
        if tick % max(int(1.0 / args.location_period), 1) == 0:
            d.heartbeat()
            d.system_time()
            d.message_pack()
            d.system(mavlink.MAV_ODID_OPERATOR_LOCATION_TYPE_FIXED)

    run_periodic(driver, monitor, args, per_tick)


def scenario_auth(driver, monitor, args):
    print("Sending identity + AUTHENTICATION pages.\n")

    def per_tick(d, now, tick):
        d.location(*moving_position(args, now))
        if tick % max(int(1.0 / args.location_period), 1) == 0:
            d.heartbeat()
            d.system_time()
            d.identity()
            d.system(mavlink.MAV_ODID_OPERATOR_LOCATION_TYPE_FIXED)
            d.authentication(length=args.auth_length)

    run_periodic(driver, monitor, args, per_tick)


def scenario_monitor(driver, monitor, args):
    print("Monitoring device output only (sending nothing). Ctrl-C to stop.\n")
    try:
        while True:
            print(f"  {monitor.status_line()}  counts={monitor.snapshot()['counts']}")
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\nstopped.")


def scenario_selftest(driver, monitor, args):
    """Automated checks. Requires status output (and, for some, the operator-location loop)."""
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{(' - ' + detail) if detail else ''}")

    def wait_for(predicate, timeout, feeder):
        deadline = time.time() + timeout
        while time.time() < deadline:
            feeder()
            time.sleep(0.5)
            value = predicate()
            if value:
                return value
        return None

    print("Self-test (needs CONFIG_REMOTEID_MAVLINK_STATUS_OUTPUT; loop checks need the operator-location loop)\n")

    # 1. Device is alive and emitting HEARTBEAT.
    hb = wait_for(lambda: monitor.snapshot()["heartbeat"], 6, lambda: None)
    check("device HEARTBEAT (MAV_TYPE_ODID) received", hb is not None,
          "" if hb else "no ODID heartbeat - is status output enabled and TX wired?")

    # 2. With no identity yet, ARM_STATUS should be a pre-arm failure.
    arm = wait_for(lambda: monitor.snapshot()["arm_status"], 4, lambda: None)
    if arm is not None:
        check("ARM_STATUS reports PRE_ARM_FAIL before identity", arm[0] != 0,
              f"status={arm[0]} error='{arm[1]}'")

    # 3. After full identity + fixed operator location, ARM_STATUS -> GOOD_TO_ARM.
    def feed_fixed():
        driver.heartbeat()
        driver.system_time()
        driver.identity()
        driver.location(*moving_position(args, 0))
        driver.system(mavlink.MAV_ODID_OPERATOR_LOCATION_TYPE_FIXED)

    good = wait_for(
        lambda: (monitor.snapshot()["arm_status"] or (1,))[0] == 0, 8, feed_fixed)
    check("ARM_STATUS -> GOOD_TO_ARM after identity + operator location", bool(good))

    # 4. Operator-location loop: stop feeding, device should republish SYSTEM_UPDATE itself.
    base = monitor.snapshot()["counts"]["SYSTEM_UPDATE"]
    time.sleep(0.2)
    republished = wait_for(
        lambda: monitor.snapshot()["counts"]["SYSTEM_UPDATE"] > base, 8,
        lambda: None)  # feed nothing - any SYSTEM_UPDATE now is device-originated
    check("device republishes SYSTEM_UPDATE for a fixed operator location (loop)",
          bool(republished),
          "" if republished else "no self-republish - operator-location loop may be disabled")

    # 5. Live operator location expires when updates stop.
    def feed_live():
        driver.heartbeat()
        driver.system_time()
        driver.identity()
        driver.system(mavlink.MAV_ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS)
        driver.system_update(args.operator_lat, args.operator_lon, args.operator_alt)

    wait_for(lambda: (monitor.snapshot()["arm_status"] or (1,))[0] == 0, 6, feed_live)
    print("  (live operator set; stopping updates, waiting for expiry...)")
    expired = wait_for(
        lambda: (monitor.snapshot()["arm_status"] or (0,))[0] != 0, 15, lambda: None)
    check("live operator location expires -> PRE_ARM_FAIL when updates stop", bool(expired),
          "" if expired else "did not expire - check operator-location loop / expiry window")

    passed = sum(1 for _, ok, _ in results if ok)
    print(f"\n{passed}/{len(results)} checks passed.")
    return all(ok for _, ok, _ in results)


SCENARIOS = {
    "drive": scenario_drive,
    "pack": scenario_pack,
    "auth": scenario_auth,
    "monitor": scenario_monitor,
    "selftest": scenario_selftest,
}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--device", required=True,
                   help="serial port (/dev/cu.usbmodem1101) or mavlink-router endpoint "
                        "(udpout:host:port, tcp:host:port)")
    p.add_argument("--baud", type=int, default=115200, help="serial baud (ignored on USB CDC)")
    p.add_argument("--source-system", type=int, default=1,
                   help="our MAVLink system ID (1 = appear as the vehicle, so the device adopts it)")
    p.add_argument("--source-component", type=int, default=1, help="our MAVLink component ID")
    p.add_argument("--target-system", type=int, default=0, help="ODID target system (0 = broadcast)")
    p.add_argument("--target-component", type=int, default=0, help="ODID target component (0 = broadcast)")

    p.add_argument("--uas-id", default="PEINSER-TEST-0001", help="UAS serial number")
    p.add_argument("--operator-id", default="BEL-OP-TEST-12345", help="operator registration ID")
    p.add_argument("--self-id", default="Peinser RemoteID test", help="free-text self ID")

    p.add_argument("--lat", type=float, default=50.8503, help="flight path center latitude")
    p.add_argument("--lon", type=float, default=4.3517, help="flight path center longitude")
    p.add_argument("--alt", type=float, default=80.0, help="geodetic altitude (m)")
    p.add_argument("--radius", type=float, default=50.0, help="flight circle radius (m)")
    p.add_argument("--speed", type=float, default=5.0, help="ground speed (m/s)")
    p.add_argument("--operator-lat", type=float, default=50.8500, help="fixed operator latitude")
    p.add_argument("--operator-lon", type=float, default=4.3510, help="fixed operator longitude")
    p.add_argument("--operator-alt", type=float, default=-1000.0, help="operator geodetic alt (-1000 = unknown)")

    p.add_argument("--operator", choices=["fixed", "live", "none"], default="fixed",
                   help="operator-location mode for 'drive'")
    p.add_argument("--armed", action="store_true",
                   help="send an armed autopilot HEARTBEAT (exercises takeoff-position capture)")
    p.add_argument("--location-rate", type=float, default=2.0, help="LOCATION send rate (Hz)")
    p.add_argument("--auth-length", type=int, default=64,
                   help="authentication signature length in bytes for 'auth' (split across pages)")

    p.add_argument("scenario", choices=sorted(SCENARIOS), help="what to run")
    args = p.parse_args()
    args.location_period = 1.0 / max(args.location_rate, 0.1)

    print(f"Connecting to {args.device} ...")
    conn = mavutil.mavlink_connection(
        args.device, baud=args.baud, dialect="ardupilotmega",
        source_system=args.source_system, source_component=args.source_component)

    monitor = DeviceMonitor(conn, args.source_component)
    monitor.start()
    driver = Driver(conn, args)

    try:
        result = SCENARIOS[args.scenario](driver, monitor, args)
    finally:
        monitor.stop()

    if args.scenario == "selftest":
        sys.exit(0 if result else 1)


if __name__ == "__main__":
    main()
