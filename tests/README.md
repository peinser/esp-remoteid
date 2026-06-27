# Remote ID MAVLink test driver

`remoteid_test.py` drives the firmware over MAVLink the way a GCS/autopilot
would, so you can verify reception in an OpenDroneID phone app and check the
firmware's outbound messages. It exercises every MAVLink feature the firmware
implements (see the docstring in the script).

## Setup

```bash
cd tests
python3 -m venv .venv && source .venv/bin/activate   # optional
pip install -r requirements.txt
```

## Connecting

Point `--device` at either the firmware's serial/USB port or a mavlink-router
endpoint:

- USB Serial/JTAG (the native port carrying MAVLink): `--device /dev/cu.usbmodem1101`
  (macOS) or `/dev/ttyACM0` (Linux).
- UART transport: the UART/bridge serial device, plus `--baud`.
- Via mavlink-router: add a UDP/TCP out endpoint and use
  `--device udpout:127.0.0.1:14555` or `--device tcp:127.0.0.1:5760`.

The driver sends as system ID `1` by default so the firmware adopts it (matching
a typical vehicle), and addresses OpenDroneID messages to `target=0/0` (broadcast),
which the firmware always accepts. Override with `--source-system` / `--target-*`.

## Scenarios

```bash
# Drive a live, moving drone (the main one for phone testing).
# Sends identity + moving LOCATION + SYSTEM_TIME, with a fixed operator location.
python remoteid_test.py --device /dev/cu.usbmodem1101 drive --operator fixed

# Live (dynamic) operator location streamed at ~1 Hz instead of fixed.
python remoteid_test.py --device /dev/cu.usbmodem1101 drive --operator live

# Deliver identity via OPEN_DRONE_ID_MESSAGE_PACK (unpacked by the firmware).
python remoteid_test.py --device /dev/cu.usbmodem1101 pack

# Identity + AUTHENTICATION pages.
python remoteid_test.py --device /dev/cu.usbmodem1101 auth --auth-pages 3

# Watch the firmware's outbound HEARTBEAT / ARM_STATUS / SYSTEM_UPDATE only.
python remoteid_test.py --device /dev/cu.usbmodem1101 monitor

# Automated checks (exit code 0 = all passed).
python remoteid_test.py --device /dev/cu.usbmodem1101 selftest
```

Every scenario prints a one-line device status each second, e.g.:

```
[  12.0s] HB:ok | ARM:GOOD_TO_ARM | SYS_UPD:50.85000,4.35100 0.4s ago
```

- `HB` — an `MAV_TYPE_ODID` HEARTBEAT was seen recently.
- `ARM` — the firmware's `OPEN_DRONE_ID_ARM_STATUS` (`GOOD_TO_ARM` or `PRE_ARM_FAIL` + reason).
- `SYS_UPD` — a `SYSTEM_UPDATE` the firmware *republished itself* (fixed-operator loop).

## Phone integration test

1. Build/flash with BLE and/or Wi-Fi broadcast enabled (and a non-placeholder or
   empty identity so MAVLink can drive it).
2. Run `drive`. The firmware needs a valid Basic ID + Operator ID before it
   broadcasts; the driver supplies them, so the drone should appear in the
   OpenDroneID app within a second or two and then move along a circle.
3. Use `--lat/--lon/--radius/--speed` to place and shape the flight path.

## What the device must have enabled for each check

| Behaviour exercised | Requires firmware option |
|---------------------|--------------------------|
| Identity / Location / System / Self ID / Auth / Pack ingest | `CONFIG_REMOTEID_MAVLINK_INPUT` |
| `HEARTBEAT` + `ARM_STATUS` output | `CONFIG_REMOTEID_MAVLINK_STATUS_OUTPUT` |
| `SYSTEM_UPDATE` republish + live expiry + operator-location arm gating | `CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_LOOP` |

`selftest` degrades gracefully: checks for features that are disabled will simply
fail with an explanatory note rather than crash.
