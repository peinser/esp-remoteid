"""DroneCAN wire-format regression test against the reference pydronecan package.

Usage: python3 dronecan_test.py <path to the dronecan_test harness>

Receive: frames encoded by pydronecan (dronecan.remoteid.*, ArmingStatus) go
through the firmware's decoder; every field must match, and messages about other
aircraft, from other nodes, or with a bad transfer CRC must be ignored.
Transmit: the firmware's NodeStatus, ArmStatus and GetNodeInfo frames must
decode with pydronecan, CRC included.
"""
import subprocess
import sys

import dronecan
from dronecan import transport

HARNESS = sys.argv[1]
R = dronecan.dronecan.remoteid
ODID_EPOCH_2019 = 1546300800
failures = 0


def expect(name, got, want):
    global failures
    ok = abs(float(got) - want) < 1e-3 if isinstance(want, float) else str(got) == str(want)
    if not ok:
        failures += 1
        print(f"MISMATCH {name}: got {got!r}, want {want!r}")


def frames_for(msg, src=10, tid=3):
    t = transport.Transfer(payload=msg, source_node_id=src, transfer_id=tid, transfer_priority=24,
                           service_not_message=False)
    return ["%08x %s" % (f.message_id, bytes(f.bytes).hex()) for f in t.to_frames()]


def records(output, kind):
    """Decoded messages of one kind as {field: value} dicts."""
    result = []
    for line in output:
        parts = line.split()
        if parts and parts[0] == kind:
            result.append(dict(p.split("=", 1) for p in parts[1:]))
    return result


# ---- Receive -------------------------------------------------------------------

location = R.Location(status=2, direction=35999, speed_horizontal=1234, speed_vertical=-321,
                      latitude=509622900, longitude=-44549770, altitude_barometric=123.25, altitude_geodetic=-45.5,
                      height_reference=1, height=67.75, horizontal_accuracy=10, vertical_accuracy=4,
                      barometer_accuracy=3, speed_accuracy=2, timestamp=1234.5, timestamp_accuracy=5)
system = R.System(operator_location_type=1, classification_type=1, operator_latitude=-123456789,
                  operator_longitude=1800000000, area_count=1, area_radius=250, area_ceiling=120.5, area_floor=-1000.0,
                  category_eu=2, class_eu=3, operator_altitude_geo=56.25, timestamp=200000000)
basic_id = R.BasicID(id_type=1, ua_type=2)
basic_id.uas_id = list(b"1ABC5XYZ12")
operator_id = R.OperatorID(operator_id_type=0)
operator_id.operator_id = list(b"FIN87astrdge12k8-xyz")
self_id = R.SelfID(description_type=0)
self_id.description = list(b"Survey flight, 23 chars")
other_aircraft = R.Location(status=2, latitude=1, longitude=1)
other_aircraft.id_or_mac = list(b"\x01\x02\x03\x04\x05\x06")
arming = dronecan.uavcan.equipment.safety.ArmingStatus

lines = []
for msg in (location, system, basic_id, operator_id, self_id, other_aircraft):
    lines += frames_for(msg)
lines += frames_for(location, src=11)                       # another node: ignored (bound to node 10)
corrupt = frames_for(location, tid=4)
can_id, data = corrupt[-1].split()
corrupt[-1] = can_id + " " + bytes([int(data[:2], 16) ^ 0xFF]).hex() + data[2:]
lines += corrupt                                            # bad transfer CRC: ignored
lines += frames_for(arming(status=0), tid=5) + frames_for(arming(status=0), tid=6)  # reported once
lines += frames_for(arming(status=255), src=11, tid=7)      # another node arming: ignored
lines += frames_for(arming(status=255), tid=8)              # the bound autopilot arms

output = subprocess.run([HARNESS, "rx"], input="\n".join(lines) + "\n", capture_output=True,
                        text=True, check=True).stdout.splitlines()

decoded = [line.split()[0] for line in output if line.split()[0] in ("LOCATION", "SYSTEM", "BASIC", "OPERATOR", "SELF")]
expect("decoded messages", decoded, ["LOCATION", "SYSTEM", "BASIC", "OPERATOR", "SELF"])
got = records(output, "LOCATION")[0]
for field in ("status", "direction", "speed_horizontal", "speed_vertical", "latitude", "longitude",
              "height_reference", "horizontal_accuracy", "vertical_accuracy", "barometer_accuracy",
              "speed_accuracy", "timestamp_accuracy"):
    expect(f"Location.{field}", got[field], getattr(location, field))
for field in ("altitude_barometric", "altitude_geodetic", "height", "timestamp"):
    expect(f"Location.{field}", got[field], float(getattr(location, field)))
got = records(output, "SYSTEM")[0]
for field in ("operator_location_type", "classification_type", "operator_latitude", "operator_longitude",
              "area_count", "area_radius", "category_eu", "class_eu", "timestamp"):
    expect(f"System.{field}", got[field], getattr(system, field))
for field in ("area_ceiling", "area_floor", "operator_altitude_geo"):
    expect(f"System.{field}", got[field], float(getattr(system, field)))
expect("BasicID.uas_id", records(output, "BASIC")[0]["uas_id"], (b"1ABC5XYZ12" + bytes(10)).hex())
expect("OperatorID.id", records(output, "OPERATOR")[0]["id"], b"FIN87astrdge12k8-xyz".hex())
expect("SelfID.desc", records(output, "SELF")[0]["desc"], b"Survey flight, 23 chars".hex())
expect("arm state", [line for line in output if line.startswith("ARMED")], ["ARMED 0", "ARMED 1"])
expect("clock seed", [line for line in output if line.startswith("CLOCK")],
       [f"CLOCK {(200000000 + ODID_EPOCH_2019) * 1000000}"])

# ---- Transmit ------------------------------------------------------------------


class Frame:
    def __init__(self, can_id, data):
        self.message_id = can_id
        self.bytes = bytearray(data)
        self.extended = True
        self.canfd = False
        self.ts_monotonic = 0
        self.ts_real = 0


tx_lines = subprocess.run([HARNESS, "tx"], capture_output=True, text=True, check=True).stdout.splitlines()
transfers, current = [], []
for line in tx_lines:
    _, can_id, data = line.split()
    frame = Frame(int(can_id, 16), bytes.fromhex(data))
    current.append(frame)
    if frame.bytes[-1] & 0x40:          # end of transfer
        transfers.append(current)
        current = []

decoded = []
for frames in transfers:
    t = transport.Transfer()
    t.from_frames(frames)               # raises on a bad CRC or toggle
    decoded.append(t)
expect("transmitted types", [dronecan.get_dronecan_data_type(t.payload).full_name for t in decoded],
       ["uavcan.protocol.NodeStatus", "dronecan.remoteid.ArmStatus",
        "uavcan.protocol.NodeStatus", "dronecan.remoteid.ArmStatus", "uavcan.protocol.GetNodeInfo"])
node_status = decoded[0].payload
expect("NodeStatus", (node_status.health, node_status.mode, node_status.uptime_sec), (0, 0, 5))
expect("ArmStatus good", decoded[1].payload.status, 0)
expect("ArmStatus failure", decoded[3].payload.status, 1)
expect("ArmStatus text", bytes(decoded[3].payload.error).decode(), "Operator ID not set")
info = decoded[4]
expect("GetNodeInfo destination", info.dest_node_id, 10)
expect("GetNodeInfo transfer ID", info.transfer_id, 7)
expect("GetNodeInfo name", bytes(info.payload.name).decode(), "com.peinser.remoteid")
expect("GetNodeInfo unique ID", bytes(info.payload.hardware_version.unique_id).hex(), "246f28010203" + "00" * 10)

# The reference decoder really checks the CRC of what the firmware sent.
frames = transfers[4]
frames[1].bytes[0] ^= 1
try:
    transport.Transfer().from_frames(frames)
    expect("corrupted GetNodeInfo rejected", "accepted", "rejected")
except Exception:
    pass

print(f"dronecan_test: {'FAILED' if failures else 'passed'}")
sys.exit(1 if failures else 0)
