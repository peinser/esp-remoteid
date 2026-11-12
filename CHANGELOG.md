# Changelog

All notable changes to this firmware. Versions follow [Semantic Versioning](https://semver.org/): a major version changes behaviour or configuration that an existing deployment relies on.

## [2.0.0] - 2026-11-12

This release turns the module into an EU direct remote identification system that can sit behind an ArduPilot (or other MAVLink) autopilot or on an ArduPilot DroneCAN bus, adds a strict EU (EASA) compliance mode, and hardens the broadcast against stale, replayed or invalid input. The README's [EU compliance mapping](README.md#eu-compliance-mapping) maps every point of Regulation (EU) 2019/945, Annex Part 6 to the implementation, and [Conformity status](README.md#conformity-status) lists the evidence a product still needs (hardware RF tests, the final EN 4709-002 text, product documentation).

### Breaking changes

- **Local Ed25519 signatures now cover the authentication timestamp.** The signed data is the 125-byte message set followed by the page 0 timestamp (4 bytes, little-endian), so a recorded signature cannot be rebroadcast under a fresh timestamp. Verifiers written for 1.0.0 must append the timestamp.
- **Authentication is a choice of one source**: *Relay from MAVLink* (default when MAVLink input is enabled), *None*, or *Local Ed25519*. Pages of two sources are never mixed. An existing `CONFIG_REMOTEID_AUTH_ED25519=y` keeps local signing; a configuration with MAVLink input and no local signing now relays authentication pages received over MAVLink.
- **Broadcast intervals are limited to values that meet the required rates**: BLE 100 to 260 ms (was 100 to 5000), Wi-Fi Beacon and Wi-Fi NAN 100 to 1000 ms (was 100 to 5000). Out-of-range values in an existing `sdkconfig` fall back to the defaults (250 ms and 1000 ms).
- **BLE 5 Long Range sends Message Packs** (all messages in one advertisement) instead of single messages. This needs `CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE=251` (now in `sdkconfig.defaults`); the build fails with a lower value. Enabling BLE 4 and BLE 5 together needs `CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES >= 2`, also enforced at build time.
- **Timestamps are UTC or unknown, never uptime.** The System and authentication timestamps are 0 until UTC is known (from the autopilot), and a Location without a source timestamp is broadcast with an unknown time. 1.0.0 broadcast the uptime in these fields.
- **Identity input is validated.** An EU operator registration number entered in its full form (`FIN87astrdge12k8-xyz`) is checksum-verified and only its 16 public characters are stored and broadcast; a wrong check is rejected and reported. UAS IDs are validated per ID type, and ID type *None* or a reserved type is rejected. The default ID type for new configurations is *Serial Number* (ANSI/CTA-2063-A).

### Upgrade notes

- Re-run `idf.py menuconfig` (or rebuild from `sdkconfig.defaults`) so the new choices and ranges are applied: the authentication source, the BLE / Wi-Fi intervals and `CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE`.
- Update signature verifiers for the new signed data (see above).
- For EU operation, read [Strict EU (EASA) compliance mode](README.md#strict-eu-easa-compliance-mode) and the [minimum configuration checklist](README.md#minimum-configuration-checklist): the serial number moves to eFuse, which is irreversible to provision.

### Added

- **MAVLink integration**
  - USB Serial/JTAG transport (native USB) next to UART.
  - Status output: `HEARTBEAT` and `OPEN_DRONE_ID_ARM_STATUS`, so ArduPilot can block arming without a working Remote ID broadcast.
  - `OPEN_DRONE_ID_MESSAGE_PACK`, `OPEN_DRONE_ID_SELF_ID`, `OPEN_DRONE_ID_SYSTEM_UPDATE` and `OPEN_DRONE_ID_AUTHENTICATION` input.
  - Binding to the autopilot: arm state, emergency status and the UTC clock (`SYSTEM_TIME`) are taken only from the first autopilot `HEARTBEAT` (or the configured target system), never from a ground station.
  - Emergency status derived from the autopilot `HEARTBEAT`, latched until a power cycle by default.
  - Operator-location loop: a fixed operator location is republished to the autopilot so it stays fresh after the ground station disconnects.
- **DroneCAN for ArduPilot** (`DID_CANDRIVER`): `dronecan.remoteid.BasicID`, `Location`, `SelfID`, `System` and `OperatorID` in; `dronecan.remoteid.ArmStatus` and `uavcan.protocol.NodeStatus` out at 1 Hz; `uavcan.protocol.GetNodeInfo` responses; take-off capture from `uavcan.equipment.safety.ArmingStatus`. Static node ID (default 100), transfer CRC checks, multi-frame reassembly per node, bus-off recovery.
- **Identity source**: static (configured at build time) or runtime (from the autopilot or ground station).
- **Strict EU (EASA) compliance mode** (`CONFIG_REMOTEID_COMPLIANCE_STRICT`):
  - the ANSI/CTA-2063-A serial number is read from the write-protected eFuse user data block, provisioned at production, and Basic ID input is ignored;
  - only the full 20-character EU operator number is accepted;
  - the aircraft Location is taken only from the bound autopilot;
  - ready to arm needs a complete Location: position, height, time stamp, ground speed and, when moving, course;
  - at least one EN 4709-002 broadcast method (BLE 5 Long Range, Wi-Fi Beacon or Wi-Fi NAN) is required at build time;
  - no fixed configured position and no start-up delay;
  - the OTA server only with Secure Boot and a WPA2 password;
  - a boot warning when Secure Boot and flash encryption are not both enabled.
- **Pilot position and take-off point**: the take-off point is captured when the bound autopilot arms (MAVLink or DroneCAN) and broadcast whenever no pilot position is available, including when a live pilot position stops updating. A live pilot position expires when its source timestamp stops advancing (`CONFIG_REMOTEID_OPERATOR_LOCATION_EXPIRY_MS`, all inputs).
- **Readiness reporting** shared by MAVLink `ARM_STATUS`, DroneCAN `ArmStatus` and the status indicator, with specific reasons: missing or rejected operator ID, invalid serial, transmitters not running, stale or incomplete aircraft location, no pilot or take-off position, self-test failure.
- **Self-test**: start-up checks, and transmit deadlines per enabled transport (also before its first transmission and after a pause). A failure is broadcast as Remote ID system failure and blocks arming; good to arm is only reported once every enabled transport has transmitted.
- **Authentication relay** of MAVLink authentication pages, byte for byte, with a 10 s timeout.
- **Host regression tests** (`make host-test`): the state store, transmit deadlines, local signing and the DroneCAN wire format (against `pydronecan`).
- **Hardware test script** `tests/remoteid_test.py` for a flashed module over MAVLink.
- **Documentation**: EU compliance mapping, the legislation and standards checked (with versions), conformity status, ArduPilot setup for MAVLink and DroneCAN, eFuse serial provisioning.

### Changed

- **BLE 4 schedule**: Location and System every third slot (at least once per second up to the 260 ms maximum), the other messages and the authentication pages spread over a 9-slot cycle (at most 2.74 s).
- **Fixed-rate transmission** on every transport: the interval no longer stretches by the encode and signing time.
- **Aircraft location freshness** is judged by the source timestamp: only a new sample in order counts (cached repeats, samples behind the last one and alternating old samples are ignored), the hour rollover and long gaps are handled, and a stepped source clock is followed after five consecutive forward samples. Readiness needs a fresh sample from the last 3 s.
- **Failures at start-up** (NVS, signing key, MAVLink, DroneCAN, status indicator, lighting) no longer abort into a boot loop: Remote ID faults are reported through the self-test and broadcasting continues.
- **BLE transmit power**: the advertising sets no longer request 0 dBm over the configured level, and the power the controller selects is logged.

### Fixed

- Messages describing other aircraft (non-zero `id_or_mac`, or sent by a Remote ID receiver component) were accepted as this aircraft's data.
- MAVLink UART frames waited up to 1 s before being parsed.
- Binary UTM UUIDs and specific session IDs were truncated at the first zero byte.
- Broadcast intervals drifted below the required rates, and intervals up to 5000 ms could be configured.
- One field the encoder could not represent (for example an out-of-range ID type) stopped every transport; such updates are now rejected as a whole.
- The configured position was broadcast as barometric altitude.

### Removed

- The DroneCAN GNSS `Fix2` input and the custom `com.peinser.remoteid.Identity` message (both only on the development branch after 1.0.0), replaced by ArduPilot's `dronecan.remoteid` messages.
- DroneCAN listen-only and anonymous-transfer options: the module now takes part in the bus as a node.

### Known limitations

- Conformity of a product is not established yet; see [Conformity status](README.md#conformity-status).
- ArduPilot sends Location at a fixed 1 Hz, marginal against EN 4709-002's 1 s data-age rule.
- EN 4709-002 defines an add-on as a device with its own GNSS and barometer; this module relays the autopilot's data.
- Over DroneCAN the UTC clock is seeded from the ground station's time, forwarded by ArduPilot.
- Relayed authentication pages carry no set identifier, so a late page of an older set cannot be detected without verifying the signature.

## [1.0.0] - 2026-05-15

Initial release: Bluetooth 4 legacy, Bluetooth 5 Long Range, Wi-Fi Beacon and Wi-Fi NAN broadcast of the ASTM F3411 messages, static identity and position, MAVLink UART input, Ed25519 message set signing, OTA update server, status indicator and lighting outputs.

[2.0.0]: https://github.com/peinser/esp-remoteid/compare/v1.0.0...v2.0.0
[1.0.0]: https://github.com/peinser/esp-remoteid/releases/tag/v1.0.0
