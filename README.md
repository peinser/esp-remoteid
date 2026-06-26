ESP32-S3 Remote ID add-on module firmware targeting EU UAS Regulation compliance, built on ESP-IDF and OpenDroneID.

The firmware broadcasts the four mandatory message types required by EU Commission Delegated Regulation (EU) 2019/945 and implementing Regulation (EU) 2021/664, encoded per ASTM F3411-22a (OpenDroneID): Basic ID, Location/Vector, System, and Operator ID. Transmission supports Bluetooth LE legacy advertising and Wi-Fi Beacon advertisements. Payload encoding uses the official [`opendroneid-core-c`](https://github.com/opendroneid/opendroneid-core-c) library, included as a git submodule.

BLE uses service UUID `0xFFFA` and rotates one ODID message per advertisement. Wi-Fi Beacon uses the upstream OpenDroneID Message Pack Beacon frame builder and sends vendor information elements with the ASD-STAN OpenDroneID OUI.

Each BLE advertisement carries one ODID message as a standard Service Data AD structure:

```text
0xFFFA service data = 0x0D | message_counter | 25-byte OpenDroneID message
```

The `0xFFFA` UUID is assigned by the Bluetooth SIG for UAS Remote ID and is the universal identifier all compliant receivers scan for. The `0x0D` byte is the Bluetooth SIG Open Drone ID Application Code defined in ASTM F3411-22a. It is part of the standard, not specific to any platform.

Any compliant receiver can read these advertisements:

- **Android** apps such as [`opendroneid/receiver-android`](https://github.com/opendroneid/receiver-android) filter on the `0x0D` application code to identify Remote ID packets among general BLE traffic.
- **iOS** third-party apps use the `0xFFFA` service UUID for discovery; iOS 16 and later also includes native OS-level Remote ID detection.
- **Dedicated scanners** authority and enforcement hardware reads the same standardised packet format.

<p align="center">
  <img src=".github/images/esp32s3.png" width="320" alt="ESP32-S3 board with external BLE antenna running the Remote ID firmware, powered via USB">
  <br>
  <em>ESP32-S3 board with external BLE/WiFi antenna running the Remote ID firmware.</em>
</p>

## Configuration

All identity and position fields are set through ESP-IDF's Kconfig system. Run:

```sh
make menuconfig
```

and navigate to **ESP Remote ID**. The sections below explain what each option means and which values are legally required.

> **Note:** The firmware is protocol-compliant with ASTM F3411 and the EU UAS Regulation (EU) 2019/945 / 2021/664 broadcast requirements. Whether your specific operation is legal depends on your national CAA rules, aircraft registration, and operating category. This firmware does not substitute for regulatory advice.

### Required: UAS ID and Operator ID

| Option | Description |
|--------|-------------|
| **UAS ID** | Unique identifier for the aircraft, broadcast in the Basic ID message. Max 20 characters. |
| **Operator registration ID** | Your national pilot/operator registration number (e.g. an EASA number like `FIN87ASTRDGE12K8`), broadcast in the Operator ID message. Max 20 characters. |

Set both to your actual registration values before any flight. The placeholder defaults (`CHANGE_ME_*`) are not valid for operation and intentionally block all broadcasts until replaced or overridden by valid MAVLink OpenDroneID input.

### UAS ID type

| Drone type | Correct setting |
|------------|-----------------|
| Self-built, no manufacturer serial | **CAA Registration ID** use your national operator registration number as the UAS ID |
| Has a manufacturer serial (ANSI/CTA-2063-A) | **Serial Number** |

For most self-built drones the UAS ID and Operator ID will be the same string: your national operator registration number. For Belgium, you can find your CAA Registration ID in the portal of "Directoraat Generaal Luchtvaart" after obtaining the necessary licenses.

### UA type

Select the airframe type that matches your aircraft. **Helicopter / Multirotor** is the default and covers most DIY multirotors.

### EU equipment class and operation category

| Scenario | Class | Category |
|----------|-------|----------|
| Self-built drone (no CE class label) | **Undeclared** *(default)* | **Open** |
| Factory drone with CE class label | C0 - C6 as marked | Open / Specific / Certified |

Self-built drones do not carry a C-class label. Setting the class to anything other than **Undeclared** when no CE mark exists is incorrect. The default Kconfig values (`Undeclared` class, `Open` category) are the correct starting point for the vast majority of self-built aircraft.

### Position (Location and System messages)

The Location and System messages must be broadcast at 1 Hz regardless of whether position is known. Two modes are supported:

**No position available (default: `Broadcast a known takeoff position` = disabled)**

The messages are populated with the ASTM F3411 invalid sentinel values:

| Field | Sentinel value |
|-------|---------------|
| Latitude / Longitude | 0 |
| Altitude (baro, geo, height) | −1000 m |
| Speed (horizontal / vertical) | 255 / 63 |
| Direction | 361° |
| Timestamp | 0xFFFF |
| All accuracy fields | Unknown |
| Status | Undeclared |

This is protocol-compliant. The broadcast system is active and all four message types are transmitted; receivers will decode the messages and display the UAS ID while showing the position as unknown.

**Known takeoff position (`Broadcast a known takeoff position` = enabled)**

Enable this option and enter the takeoff coordinates. The firmware latches these values at compile time and broadcasts them as the UA and operator position (EU regulation permits using the takeoff location as the operator location for add-on modules).

| Option | Format | Example |
|--------|--------|---------|
| Takeoff latitude | Degrees × 10⁶ (signed) | `50962290` for 50.962290° N |
| Takeoff longitude | Degrees × 10⁶ (signed) | `4454977` for 4.454977° E |
| Takeoff altitude | Whole metres above WGS-84 ellipsoid | `50` |

Obtain the WGS-84 altitude from a GNSS receiver or an online tool such as [https://www.unavco.org/software/geodetic-utilities/geoid-height-calculator/geoid-height-calculator.html](https://www.unavco.org/software/geodetic-utilities/geoid-height-calculator/geoid-height-calculator.html).

### Transports

BLE 4 legacy is enabled by default. All other transports are disabled by default so receiver testing can isolate one transport at a time. BLE 4 legacy and BLE 5 Long Range can run simultaneously; each is an independent task on the same NimBLE host.

> **Regulatory note on BLE 5 Long Range:** EU regulation 2019/945 only recognises BLE 4 legacy advertising and Wi-Fi Beacon as approved DRI transports. BLE 5 Extended Advertising is defined in ASTM F3411-22a (referenced by FAA 14 CFR Part 89) but is not listed in the EU delegated regulation. A BLE 5 only configuration is therefore not compliant in the EU. BLE 5 is disabled by default for this reason.

| Option | Default | Description |
|--------|---------|-------------|
| `Enable Bluetooth LE legacy advertisements` | Enabled | Broadcasts OpenDroneID messages using BLE 4 legacy advertising on service UUID `0xFFFA`. Supported by all current receiver apps. |
| `Enable Bluetooth 5 Long Range (LE Coded PHY)` | Disabled | Broadcasts OpenDroneID messages using BLE 5 extended advertising with LE Coded PHY (S=8). Increases range significantly at the cost of a lower air data rate. Can run alongside BLE 4 legacy. Disabled by default: not an approved transport under EU 2019/945. Valid under ASTM F3411-22a (FAA). Most current receiver apps do not yet support BLE 5. |
| `BLE advertisement rotation interval` | `250 ms` | BLE payload rotation cadence for both BLE transports. Default keeps Location refreshed around 1 Hz while also rotating Basic ID, System, and Operator ID. |
| `Enable Wi-Fi Beacon advertisements` | Disabled | Broadcasts OpenDroneID Message Pack payloads in Wi-Fi Beacon vendor IEs. |
| `Enable Wi-Fi NAN advertisements` | Disabled | Broadcasts OpenDroneID Message Pack payloads in Wi-Fi NAN sync/action frames. |
| `Wi-Fi Remote ID channel` | `6` | 2.4 GHz channel used by the ESP32-S3 Wi-Fi transport. |
| `Wi-Fi TX power` | `20 dBm` | Converted internally to ESP-IDF quarter-dBm units. |
| `Wi-Fi Beacon SSID` | `OpenDroneID` | SSID embedded only in Beacon frames. Receivers should parse the vendor IE, not rely on this SSID. |
| `Wi-Fi Beacon TX interval` | `1000 ms` | Wi-Fi Beacon transmission cadence. |
| `Wi-Fi NAN TX interval` | `1000 ms` | Wi-Fi NAN sync/action transmission cadence. |
| `Enable onboard RGB status indicator` | Disabled | Drives a board-mounted addressable RGB LED as a local status indicator. |
| `Onboard RGB LED data GPIO` | `48` | Physical ESP32-S3 GPIO connected to the onboard WS2812/SK6812-style RGB LED data input. The UICPAL ESP32-S3-N16R8 board variant used by this firmware uses GPIO48; other variants may use GPIO33 or another pin. |
| `Onboard RGB LED brightness` | `16%` | Scales the local indicator LED brightness. |
| `Onboard RGB operational flash pattern` | `Drone beacon 1 Hz short flash` | Pattern used after transports have started and valid Remote ID identity is available. |
| `Enable external GPIO lighting outputs` | Disabled | Enables up to five simple GPIO outputs for external light trigger circuits. |
| `Startup delay before transmissions` | `10000 ms` | Development delay before starting BLE/Wi-Fi so there is time to attach the serial monitor. |

### Onboard RGB Indicator

The onboard RGB indicator is intended for local module status, not for driving external aviation lights. It uses the ESP32-S3 RMT peripheral to drive one addressable RGB LED, typically a WS2812/SK6812 device on the devboard. The UICPAL ESP32-S3-N16R8 board variant used by this firmware uses GPIO48 for the onboard RGB LED.

Status behavior:

| Firmware state | Onboard RGB behavior |
|----------------|----------------------|
| Waiting for Remote ID readiness or transport startup | Amber slow blink |
| Operational | Configured green flash pattern |
| Error state | Red fast blink |

Operational means the firmware has started its enabled transports and the state store has a valid Basic ID/UAS ID plus Operator ID. This prevents the indicator from showing the operational pattern while placeholder identity values are still blocking broadcasts.

The RGB data pin is not a suitable transistor trigger for external lights because addressable LEDs use a high-speed encoded data waveform. Use the external GPIO lighting outputs for that purpose.

### External GPIO Lighting

The external GPIO lighting module provides five independently configurable GPIO outputs. These outputs are logic-level triggers intended for transistor, MOSFET, relay-driver, or opto-isolator inputs. Do not power aircraft lights directly from ESP32 GPIO pins.

Each output has its own configuration:

| Setting | Description |
|---------|-------------|
| `Enable output N` | Enables this lighting output. |
| `Output N GPIO` | Physical ESP32-S3 GPIO used as the trigger signal. |
| `Output N active high` | Controls whether logical ON drives the pin high or low. |
| `Output N open drain` | Uses open-drain output mode for compatible external circuits. |
| `Output N operational pattern` | Pattern used when Remote ID transports are started and identity is ready. |
| `Output N pattern phase offset` | Staggers matching patterns across multiple outputs. |

Outputs remain off until valid Basic ID/UAS ID and Operator ID are available and enabled transports have started.

Available operational patterns:

- `Solid on`
- `Beacon 1 Hz short flash`
- `Beacon 1 Hz 50% duty`
- `Single strobe`
- `Double strobe`
- `Triple strobe`
- `Fast strobe`

Example setup:

```text
Output 0: GPIO 4, Double strobe, phase 0 ms
Output 1: GPIO 5, Double strobe, phase 500 ms
Output 2: GPIO 6, Beacon 1 Hz short flash
Output 3: disabled
Output 4: disabled
```

### MAVLink OpenDroneID Input

MAVLink input is available but disabled by default. When enabled, the firmware listens on a configured UART and accepts these MAVLink OpenDroneID messages:

- `OPEN_DRONE_ID_BASIC_ID`
- `OPEN_DRONE_ID_OPERATOR_ID`
- `OPEN_DRONE_ID_LOCATION`
- `OPEN_DRONE_ID_SYSTEM`
- `OPEN_DRONE_ID_SELF_ID`
- `OPEN_DRONE_ID_AUTHENTICATION`
- `OPEN_DRONE_ID_MESSAGE_PACK`

It also watches `HEARTBEAT` to capture the takeoff position when an autopilot first arms.

`OPEN_DRONE_ID_MESSAGE_PACK` is unpacked and each contained OpenDroneID message is applied as if it had arrived on its own, so a flight controller can deliver the full set in a single frame. Authentication pages received over the link are reassembled and broadcast verbatim, superseding any locally generated Ed25519 signature. Both transports carry all authentication pages; the BLE advertising schedule reserves a slot per page and skips any that are unpopulated without holding an empty advertising interval.

Incoming Basic ID and Operator ID updates feed the same readiness gate as Kconfig values. This allows a flight controller or companion computer to provide identity at runtime while keeping the placeholder Kconfig values in flash. Broadcasts still do not start until a valid Basic ID/UAS ID and Operator ID are present.

#### Making the flight controller emit OpenDroneID

Flight controllers do not send the `OPEN_DRONE_ID_*` messages by default; OpenDroneID has to be enabled and, on some firmware, compiled in.

**ArduPilot.** Set `DID_ENABLE = 1` and reboot. The OpenDroneID feature must be present in the firmware: many boards ship without it to save flash, so you may need a build with the "OpenDroneID" feature selected (via the [custom firmware build server](https://custom.ardupilot.org/)). Related parameters:

| Parameter | Purpose |
|-----------|---------|
| `DID_ENABLE` | Master enable for OpenDroneID. |
| `DID_MAVPORT` | Serial port index that carries the OpenDroneID MAVLink stream to this module (`-1` for none). |
| `DID_CANDRIVER` | DroneCAN driver index, if the module is on CAN instead of serial. |
| `DID_OPTIONS` | Bitmask; among other things it can require a healthy Remote ID module before arming (see arm gating below). |
| `DID_RATE_HZ` | Rate at which the dynamic messages (Location/System) are sent. |

With `DID_ENABLE=1`, ArduPilot sends Basic ID, Location, System, Operator ID, Self ID, and (when configured) Authentication, usually bundled in `OPEN_DRONE_ID_MESSAGE_PACK`. It also expects the module to talk back; see the status output section.

**PX4.** Build with the `remote_id` module included and enable it with `UXRCE`-independent parameter `RID_ENABLE = 1` (PX4's OpenDroneID driver). PX4 streams the same `OPEN_DRONE_ID_*` MAVLink messages on the configured MAVLink instance; ensure the serial port's MAVLink mode forwards the OpenDroneID stream. PX4 support is less mature than ArduPilot's and may require a recent main build; consult the PX4 OpenDroneID docs for your version.

Either way, this module consumes the standard messages once they are flowing; no module-side change is needed to switch between ArduPilot and PX4 sources.

| Option | Default | Description |
|--------|---------|-------------|
| `Enable MAVLink OpenDroneID UART input` | Disabled | Enables the MAVLink parser and state producer. |
| `MAVLink transport` | `UART` | Physical link: `UART`, or `USB Serial/JTAG` (native USB device) for a companion computer. |
| `MAVLink UART number` | `1` | ESP32-S3 UART peripheral used for input (UART transport only). This does not imply physical pins. |
| `MAVLink UART baud rate` | `57600` | UART baud rate. Common values are `57600`, `115200`, and `921600`. |
| `MAVLink UART RX GPIO` | `-1` | Physical ESP32-S3 pin routed to the selected UART RX signal. Connect this to the flight controller MAVLink TX pin. Must be configured when MAVLink input is enabled. |
| `MAVLink UART TX GPIO` | `-1` | Optional physical ESP32-S3 pin routed to the selected UART TX signal. Connect this to the flight controller MAVLink RX pin only for bidirectional wiring. Leave as `-1` for receive-only. |
| `Accepted MAVLink target system` | `0` | `0` accepts all systems; non-zero accepts that system or broadcast target `0`. |
| `Accepted MAVLink target component` | `0` | `0` accepts all components; non-zero accepts that component or broadcast target `0`. |
| `Send HEARTBEAT and OPEN_DRONE_ID_ARM_STATUS` | Disabled | Enables status output back to the flight controller. Requires the TX GPIO. |
| `Outgoing MAVLink system ID` | `0` | System ID for outgoing messages. `0` adopts the flight controller's system ID from its HEARTBEAT (uses `1` until one is seen). |
| `Outgoing MAVLink component ID` | `236` | Component ID for outgoing messages. `236`-`238` are the Open Drone ID transmitter component IDs. |
| `Status output rate (Hz)` | `1` | HEARTBEAT and ARM_STATUS transmit rate. |
| `Own the operator-location freshness loop` | Disabled | Aircraft-local operator-location ownership (see below). Requires the TX GPIO. |
| `Fixed operator-location republish period (ms)` | `2000` | SYSTEM_UPDATE cadence for a fixed operator location (0.5 Hz). |
| `Live operator-location expiry (ms)` | `5000` | Drop a live operator location if no GCS refresh arrives within this window. |

#### Status output and arm gating

When `Send HEARTBEAT and OPEN_DRONE_ID_ARM_STATUS` is enabled, the module periodically transmits two messages back to the flight controller (this requires bidirectional UART wiring, i.e. the TX GPIO connected to the flight controller's RX):

- `HEARTBEAT` with component type `MAV_TYPE_ODID`, so the flight controller and ground station see the Remote ID module as a live component of the vehicle.
- `OPEN_DRONE_ID_ARM_STATUS`, reporting `GOOD_TO_ARM` once a usable identity is present and broadcasts have started, or `PRE_ARM_FAIL_GENERIC` with an error string otherwise.

This is what lets ArduPilot treat the module as present and healthy. With the relevant `DID_OPTIONS` bit set, ArduPilot refuses to arm until it receives `GOOD_TO_ARM`, so the drone cannot take off without a working Remote ID broadcast. The module shares the flight controller's system ID (adopted automatically by default) and uses an Open Drone ID component ID, so the status appears as part of the same vehicle.

#### Operator-location freshness loop

OpenDroneID requires the operator location to be refreshed continuously (the autopilot treats a stale operator location as a pre-arm failure). A ground station can do this by streaming `OPEN_DRONE_ID_SYSTEM_UPDATE` at ~1 Hz, but that ties compliance to the GCS staying connected. With `Own the operator-location freshness loop` enabled, the firmware keeps the operator location fresh **on the aircraft**, so the UAS location stays autopilot-owned while the operator location becomes aircraft-local.

The mode is derived from the `operator_location_type` carried in the last full `OPEN_DRONE_ID_SYSTEM` message:

| Operator-location type | Firmware behaviour |
|------------------------|--------------------|
| `FIXED` / `TAKEOFF` | The firmware **republishes** `OPEN_DRONE_ID_SYSTEM_UPDATE` (broadcast `target=0/0`) at the configured period (0.5 Hz default), with the fixed position and a fresh timestamp, so the autopilot stays compliant even after the GCS disconnects. |
| `LIVE_GNSS` | The firmware **only consumes** the GCS's live `SYSTEM_UPDATE` stream and **never republishes** it (re-stamping a moving position would falsify freshness). If updates stop for longer than the expiry window, the operator location is dropped as stale. |

When stale or absent, the operator location is reported via `ARM_STATUS` (`PRE_ARM_FAIL_GENERIC`, "Operator location missing or stale") — this requires status output to also be enabled — and operator position is dropped from the BLE/Wi-Fi broadcast.

The republish timestamp (a Unix-2019 epoch) is seeded from the autopilot's `SYSTEM_TIME` message; until a valid `SYSTEM_TIME` has been seen, the loop holds off transmitting. The feature requires an outbound MAVLink path (the TX GPIO on the UART transport; inherent on USB Serial/JTAG) and is only available when MAVLink input is enabled.

#### Transport: UART or USB

The MAVLink link can run over a hardware UART or over the ESP32-S3 native USB port.

**UART** is the default and is the right choice when wiring directly to a flight-controller serial port (`DID_MAVPORT`). On ESP32-S3 the UART number selects the hardware UART block (`UART0`, `UART1`, or `UART2`); it does not uniquely select board pins. UART signals are routed through the ESP32 GPIO matrix, and many dev boards label only the programming/debug serial pins as `RX`/`TX`. Choose GPIOs that are exposed on your board and not already used by USB serial, flash/PSRAM, buttons, LEDs, or strapping functions. Receive-only wiring is enough unless status output is enabled:

```text
Flight controller TX  ->  ESP32-S3 MAVLink UART RX GPIO
Flight controller GND ->  ESP32-S3 GND
(status output also needs:  Flight controller RX <- ESP32-S3 MAVLink UART TX GPIO)
```

**USB Serial/JTAG** uses the ESP32-S3 built-in USB Serial/JTAG controller on the native USB port (GPIO19/20) as a USB CDC device, so no GPIO wiring is needed and the same single connection carries both directions (including status output). The peer must be a **USB host** - a companion computer or SBC - which sees the module as a serial device (for example `/dev/ttyACM0`) that can be fed to `mavlink-router`, MAVProxy, or any MAVLink endpoint. It does not connect directly to a flight controller's USB port, because that port is itself a USB device.

The ESP32-S3 has a single native USB interface. A dev board's second "USB" port is usually a USB-to-UART bridge that is electrically just `UART0`.

The native port is shared with logging, so this project disables the **secondary console** by default (`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` in `sdkconfig.defaults`); logs go to the primary `UART0` console instead. This keeps the native USB port free for the MAVLink transport, so no extra configuration is needed - the USB transport works out of the box. If the secondary console is re-enabled on USB Serial/JTAG while this transport is selected, log text would corrupt the MAVLink stream, so the build fails with an explanatory error.

To send logs over the native USB port instead (only when you are *not* using USB for MAVLink), re-enable a USB console with `idf.py menuconfig` -> **Component config -> ESP System Settings -> Channel for console secondary output -> USB Serial/JTAG Controller** (or set the primary console to it under **Channel for console output**).

### DroneCAN Input

DroneCAN input development has started and is disabled by default. This targets the DroneCAN protocol used by ArduPilot and PX4 DroneCAN peripherals and flight-controller buses. DroneCAN is the maintained UAVCAN v0-derived protocol; it is distinct from OpenCyphal/UAVCAN v1. The current milestone brings up the ESP32-S3 TWAI/CAN controller, receives DroneCAN extended CAN frames, reassembles supported multi-frame transfers, and decodes `uavcan.equipment.gnss.Fix2` plus a project-specific Remote ID identity message into the Remote ID state.

No extra git submodule is required for this first milestone; it uses ESP-IDF's built-in TWAI driver. A later milestone that decodes DroneCAN DSDL cleanly may add a small UAVCAN/DroneCAN support library or generated DSDL sources, but that should be decided when the target flight-controller messages are confirmed.

| Option | Default | Description |
|--------|---------|-------------|
| `Enable DroneCAN input` | Disabled | Starts the DroneCAN/TWAI receive task. |
| `DroneCAN RX GPIO` | `-1` | ESP32-S3 GPIO connected to the CAN transceiver RXD output. Must be configured when DroneCAN is enabled. |
| `DroneCAN TX GPIO` | `-1` | ESP32-S3 GPIO connected to the CAN transceiver TXD input. Must be configured because the TWAI peripheral owns both pins, even in listen-only mode. |
| `DroneCAN bitrate` | `1 Mbit/s` | CAN bus bitrate. Common DroneCAN setups use `1 Mbit/s`; match the flight controller bus. |
| `Local DroneCAN node ID` | `0` | Reserved for future active participation. Current implementation receives only and does not publish DroneCAN transfers. |
| `Accept anonymous DroneCAN transfers` | Enabled | Allows source node ID `0` message transfers. |
| `Use CAN listen-only mode` | Disabled | Sniffs the bus without ACKing frames. Leave disabled for normal FC-to-ESP operation. |
| `Remote ID identity DroneCAN data type ID` | `20000` | Vendor-specific DroneCAN data type ID for the custom Remote ID identity message. Publisher and receiver must match. |

Currently decoded messages:

| DroneCAN message | Data type ID | Remote ID fields updated |
|------------------|--------------|--------------------------|
| `uavcan.equipment.gnss.Fix2` | `1063` | Latitude, longitude, altitude, horizontal speed, vertical speed, track direction, fix status, and coarse accuracy flags. |
| `com.peinser.remoteid.Identity` | `20000` by default | Basic ID UAS ID/type, UA type, and Operator ID. |

The firmware also recognizes `uavcan.protocol.NodeStatus` (`341`) and `uavcan.equipment.gnss.Fix` (`1060`) in logs, but `Fix2` and the custom identity message are the messages that update Remote ID state in this milestone.

#### Custom Remote ID Identity Message

ArduPilot/PX4 DroneCAN does not define a standard Remote ID identity message equivalent to MAVLink `OPEN_DRONE_ID_BASIC_ID` and `OPEN_DRONE_ID_OPERATOR_ID`. This firmware therefore supports a project-specific DroneCAN message for publishers that you control.

Use this DSDL definition and configure its data type ID to match `REMOTEID_DRONECAN_REMOTEID_IDENTITY_DTID`. A suitable file path for a publisher DSDL tree is `com/peinser/remoteid/20000.Identity.uavcan`:

```text
# com.peinser.remoteid.Identity
# Default data type ID used by this firmware: 20000

uint8 id_type        # ASTM/OpenDroneID ODID_idtype_t value
uint8 ua_type        # ASTM/OpenDroneID ODID_uatype_t value
uint8[20] uas_id     # Null-padded ASCII, max 20 bytes
uint8[20] operator_id # Null-padded ASCII, max 20 bytes
```

The message is intentionally fixed-size so small publisher implementations do not need dynamic array serialization. If the text fields are shorter than 20 bytes, pad the remaining bytes with zero. If they are exactly 20 bytes, omit the null terminator.

ArduPilot/PX4 will not emit this message by default. A publisher must be added through flight-controller firmware changes, scripting support, or a companion node on the same DroneCAN bus.

By default the ESP32 TWAI controller runs in normal CAN mode. It does not transmit DroneCAN application messages, but it does acknowledge received CAN frames at the bus level. This matters on a two-node bus with only the flight controller and ESP module: without an ACKing receiver, the flight controller will treat its CAN transmissions as failed. Enable listen-only mode only when sniffing an already healthy bus with at least one other ACKing node.

Basic wiring with an external 3.3 V-compatible CAN transceiver:

```text
ESP32-S3                     CAN transceiver                 Flight controller CAN port
--------                     ---------------                 --------------------------
DroneCAN TX GPIO  -------->  TXD
DroneCAN RX GPIO  <--------  RXD
3V3              ----------  VCC       (if using a 3.3 V transceiver/module)
GND              ----------  GND  -------------------------  GND
                              CANH -------------------------  CANH / CAN_H / H
                              CANL -------------------------  CANL / CAN_L / L
```

Use a CAN transceiver that matches the ESP32-S3 IO voltage on `TXD`/`RXD`. The ESP32-S3 GPIOs are 3.3 V logic, so a 5 V CAN transceiver module is only safe if its digital IO side is explicitly 3.3 V compatible. Many bare CAN transceiver boards expose `VCC`, `GND`, `TXD`, `RXD`, `CANH`, and `CANL`; connect `CANH`/`CANL` only to the bus, never directly to ESP32 GPIO pins.

DroneCAN is a shared two-wire bus. The bus should be one physical trunk with short stubs to each node:

```text
120 ohm terminator                                             120 ohm terminator
       |                                                              |
       v                                                              v
CANH ===+======================+======================+===============+=== CANH
        |                      |                      |
        |                      |                      |
       FC CAN port            ESP32 CAN transceiver   Other DroneCAN node
        |                      |                      |
CANL ===+======================+======================+===============+=== CANL
```

Termination rules:

- Install exactly two 120 ohm terminators, one at each physical end of the CANH/CANL trunk.
- Do not add a third terminator on the ESP module if the flight controller and another end-of-bus device already provide termination.
- If the ESP transceiver is physically at one end of the bus, enable or install one 120 ohm terminator at the ESP end and one at the opposite end.
- With power off, a correctly terminated bus normally measures about 60 ohm between `CANH` and `CANL` because the two 120 ohm terminators are in parallel.
- Keep the ESP transceiver stub short. If possible, place it on the trunk rather than at the end of a long branch.

For Android app testing, use one transport at a time because many receiver apps do not display whether a detected aircraft came from BLE or Wi-Fi.

### Minimum configuration checklist

Before flashing for any actual operation:

- [ ] Set **UAS ID** to your aircraft identifier (operator registration number for self-builds)
- [ ] Set **Operator registration ID** to your national CAA/EASA pilot registration number
- [ ] Set **UAS ID type** to **CAA Registration ID** (unless you have a CTA-2063-A serial)
- [ ] Set **UA type** to match your airframe
- [ ] Leave **EU equipment class** as **Undeclared** if your drone has no CE class label
- [ ] Set **EU operation category** to **Open** (or your authorised category)
- [ ] Optionally enable **Broadcast a known takeoff position** and enter your takeoff coordinates

Note: BLE and Wi-Fi transports wait for the store readiness gate before transmitting. The gate requires a usable Basic ID/UAS ID and Operator ID, either from Kconfig or from runtime producers such as MAVLink.

## Prerequisites

- ESP32-S3 board
- ESP-IDF environment, preferably the included devcontainer
- Initialized submodules:

```sh
git submodule update --init --recursive
```

For the optional BLE validation script on macOS:

```sh
python3 -m pip install bleak
```

## Development

Open the repository in the devcontainer. The container is based on Espressif's ESP-IDF image and includes `idf.py`, CMake, Ninja, `socat`, `clang-format`, and related firmware tooling.

Common commands inside the devcontainer:

```sh
make build
make flash
make monitor
```

The default ESP serial device inside the container is `/dev/ttyESP32`. Override it with `ESPPORT` if needed:

```sh
make flash ESPPORT=/dev/ttyACM0
```

## Transmit Power

OpenDroneID BLE reception range is not guaranteed by a fixed distance in the EU documents; practical range depends on transmitter power, receiver hardware, antenna design, enclosure, orientation, and RF environment. This firmware therefore makes BLE advertising TX power explicit and configurable.

The default is `+9 dBm`, the highest ESP32-S3 BLE level exposed by ESP-IDF:

```text
CONFIG_REMOTEID_BLE_TX_POWER_P9=y
```

Change it with `make menuconfig` under `ESP Remote ID -> BLE advertising TX power`, or by editing `sdkconfig.defaults` before regenerating `sdkconfig`. Keep the configured level within the limits of your board, antenna, enclosure, and local RF rules.

## (macOS) Serial Bridge

Docker on macOS does not expose `/dev/cu.*` serial devices directly to Linux containers. Use `socat` to bridge the ESP32-S3 USB serial device from the host into the devcontainer.

On the macOS host:

```sh
brew install socat
ls /dev/{cu,tty}.usb*
make bridge-host HOST_SERIAL=/dev/cu.usbmodemXXXX
```

Inside the devcontainer, in a second terminal:

```sh
make bridge-container
```

Keep both bridge commands running while flashing or monitoring through `/dev/ttyESP32`.

## BLE Validation

After flashing, verify the advertisements from macOS. Pass your configured takeoff coordinates to the `--near-lat` / `--near-lon` filters, or omit them to see all OpenDroneID advertisements:

```sh
python .dev/scripts/detect-opendroneid-ble.py --timeout 30
# or, to filter by proximity to a known location:
python .dev/scripts/detect-opendroneid-ble.py --timeout 30 --near-lat <lat> --near-lon <lon>
```

Expected output includes `app_code=0x0d` and message types such as:

- `type=0 (Basic ID)`
- `type=1 (Location)`
- `type=4 (System)`
- `type=5 (Operator ID)`

If the scanner sees valid advertisements but a phone app does not, check that Bluetooth and location permissions are granted and that the app supports BLE legacy Remote ID reception.

## Transport Test Modes

Use `make menuconfig` under **ESP Remote ID** within the `Component config` menu to switch transport combinations before building and flashing.

| Test mode | BLE | Wi-Fi Beacon | Wi-Fi NAN | Use case |
|-----------|-----|--------------|----------|----------|
| BLE-only | Enabled | Disabled | Disabled | Default Android BLE validation and macOS scanner validation. |
| Wi-Fi Beacon-only | Disabled | Enabled | Disabled | Confirms the receiver can detect Wi-Fi Beacon Remote ID without BLE/NAN ambiguity. |
| Wi-Fi NAN-only | Disabled | Disabled | Enabled | Confirms the receiver can detect Wi-Fi NAN Remote ID without BLE/Beacon ambiguity. |
| Dual Wi-Fi | Disabled | Enabled | Enabled | Tests Beacon and NAN together after each works independently. |
| All transports | Enabled | Enabled | Enabled | Broadcasts all enabled transports after independent validation. |

For Wi-Fi-only testing, flash the firmware and use an Android receiver that supports the specific Wi-Fi transport being tested. If the app does not show a source transport, enable only one Wi-Fi transport at a time so any detection has a known source.

The screenshot below shows the OpenDroneID Android app receiving live advertisements from the module. Current firmware blocks placeholder identifiers (`CHANGE_ME_UAS_ID`, `CHANGE_ME_OP_ID`), so replace these with your registration details or provide valid MAVLink OpenDroneID identity before testing reception.

<p align="center">
  <img src=".github/images/android-opendroneid.png" width="300" alt="OpenDroneID Android app showing one detected drone with Basic ID, Location (no position), System, and Operator ID messages">
</p>

## Authentication (F3411-22a Ed25519)

The firmware supports ASTM F3411-22a message set authentication using Ed25519 signatures. When enabled, the four-message set (BasicID + Location + System + OperatorID) is signed on every broadcast cycle and the 64-byte signature is broadcast across four authentication pages.

### How it works

Each broadcast cycle the firmware:

1. Encodes the four base ODID messages into their 25-byte wire format (100 bytes total)
2. Signs those bytes with the configured Ed25519 private key
3. Distributes the 64-byte signature across four authentication pages (page 0: 17 bytes + metadata, pages 1–3: 23/23/1 bytes)
4. Broadcasts the auth pages alongside the base messages

Receivers that have the corresponding public key can verify the signature over the message set they received.

### PKI and CA-signed certificates

The private key on the device is the leaf key of a standard PKI hierarchy:

```
Your CA root
    └── Device certificate  (public key + UAS ID/serial + CA signature)
            └── Ed25519 private key  (on device, used for signing)
```

The device certificate binds the public key to the drone's identity (UAS ID, operator, validity period) and is signed by your CA. A web service verifying a broadcast:

1. Reads the UAS ID from the BasicID message
2. Looks up the device certificate in your registry (keyed by UAS ID)
3. Verifies the Ed25519 signature in the auth pages using the certificate's public key
4. Verifies the certificate chain back to your CA root

The firmware only needs the private key. The certificate lives in your registry.

### Assumptions and limitations

- **No real-time clock.** The timestamp in auth page 0 is seconds since boot, not UTC. This satisfies the wire format but limits anti-replay protection. A GPS or NTP time source would provide a meaningful timestamp.
- **Private key security.** The key is only as safe as the flash it lives in. Without flash encryption the raw key bytes are readable directly off the chip. See [Flash encryption](#flash-encryption) below.

### Key generation

Generate an Ed25519 private key (PKCS#8 PEM format):

```sh
openssl genpkey -algorithm ed25519 -out device.pem
```

Extract the public key:

```sh
openssl pkey -in device.pem -pubout -out device_pub.pem
openssl pkey -in device.pem -pubout -outform DER | tail -c 32 | xxd -p -c 32
```

The public key is also logged at INFO level on every startup:

```
I (312) remoteid_auth: Ed25519 authentication enabled, public key: <64 hex chars>
```

To generate a CSR for CA signing (replace `UAS-ID-HERE` with the drone's UAS ID):

```sh
openssl req -new -key device.pem -out device.csr \
  -subj "/CN=UAS-ID-HERE/O=YourOrganisation"
```

Submit `device.csr` to your CA. Register the signed certificate in your verification service registry, keyed by UAS ID.

### Key sources

The firmware looks for the private key in this order:

1. **Compiled-in**: `REMOTEID_AUTH_PRIVATE_KEY_PEM` in Kconfig/sdkconfig. Used during development; takes priority when set.
2. **NVS**: NVS namespace `remoteid_auth`, key `private_key`. Used in production; the flash encryption hardware protects it at rest.

If neither source has a key the firmware aborts at startup with an error log.

### Development configuration

Format the private key for Kconfig (one line, `\n` as separator):

```sh
awk 'NF {printf "%s\\n", $0}' device.pem
```

Enable authentication and set the key in `menuconfig` under **ESP Remote ID → Authentication**, or set directly in `sdkconfig`:

```
CONFIG_REMOTEID_AUTH_ED25519=y
CONFIG_REMOTEID_AUTH_PRIVATE_KEY_PEM="-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYD...\n-----END PRIVATE KEY-----\n"
```

A dummy key and placeholder identity are provided in `sdkconfig.dev` for local development without a real CA:

```sh
export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dev"
idf.py build
```

Never flash `sdkconfig.dev` credentials to a production device.

### Production configuration (NVS provisioning)

Leave `REMOTEID_AUTH_PRIVATE_KEY_PEM` empty in the production build. Provision the key into NVS using the ESP-IDF partition generator before the device is sealed. Follow the procedure below our apply the procedure outlined by your PKI:

1. Generate a per-device key:

   ```sh
   openssl genpkey -algorithm ed25519 -out device.pem
   ```

2. Provision the key into the device NVS partition **before first boot**:

   ```sh
   make provision-key KEY_FILE=device.pem
   ```

   or with an explicit port:

   ```sh
   python .dev/scripts/provision_key.py device.pem --port /dev/ttyUSB0
   ```

   The script generates the NVS binary and flashes it in one step. To generate the binary without flashing (e.g. to inspect it or flash it manually later):

   ```sh
   python .dev/scripts/provision_key.py device.pem --output nvs.bin
   parttool.py -p /dev/ttyUSB0 write_partition --partition-name nvs --input nvs.bin
   ```

After first boot with flash encryption enabled the NVS partition is encrypted by the hardware and the plaintext binary is no longer accepted.

### Flash encryption

Flash encryption uses a hardware AES-256 key generated on first boot and stored in eFuse; it never leaves the chip. All flash reads and writes go through the hardware engine transparently; no firmware changes are required.

Enable it in `menuconfig` under **Security features → Enable flash encryption on boot**. Choose **Development** mode during bring-up (allows re-flashing via serial) and **Release** mode for production units (irreversible; serial flashing is disabled).

**Development mode workflow:**

```sh
idf.py flash             # first flash: plaintext, bootloader encrypts on first boot
idf.py encrypted-flash   # subsequent flashes: pre-encrypt before writing
```

**Production mode workflow:** flash the firmware and NVS partition once before sealing. After first boot the device can only be updated via OTA.

#### Release mode build guard

> **Warning:** Release mode permanently burns eFuses on first boot that disable serial flashing and JTAG. This cannot be undone without replacing the SoC. A device in Release mode without a working OTA path is unrecoverable.

To prevent accidental enabling, the build will fail if `CONFIG_FLASH_ENCRYPTION_MODE_RELEASE` is set without an explicit confirmation. In `menuconfig`, navigate to **ESP Remote ID → Flash encryption Release mode confirmation** and type `UNRECOVERABLE` exactly. The build will be blocked until this string is present.

## OTA update server

The firmware includes an optional over-the-air management server. When triggered, the device suspends normal Remote ID operation and starts a Wi-Fi access point with a lightweight HTTP server at `http://192.168.4.1`. The server provides endpoints for firmware updates, NVS key provisioning, factory reset, and OTA rollback.

OTA mode is required for updating sealed production devices that have flash encryption Release mode active (serial flashing is disabled after first boot in Release mode).

### Enabling OTA

Enable in `menuconfig` under **ESP Remote ID → OTA update server**:

| Option | Default | Description |
|--------|---------|-------------|
| `Enable OTA update server` | Disabled | Compile and include the OTA server. |
| `OTA trigger GPIO` | `-1` | GPIO sampled at boot; hold low (button to GND) to enter OTA mode. `-1` disables GPIO triggering. |
| `Always enter OTA mode on boot` | Disabled | Skip the GPIO check and always start the OTA server. Development only. |
| `OTA Wi-Fi AP SSID` | `RemoteID-OTA` | SSID broadcast while OTA mode is active. |
| `OTA Wi-Fi AP password` | *(empty)* | WPA2 passphrase (minimum 8 characters). Leave blank for an open AP. |
| `OTA Wi-Fi AP channel` | `6` | 2.4 GHz channel for the OTA access point. |
| `OTA HTTP server port` | `80` | TCP port for the HTTP management server. |

### Entering OTA mode

Wire a momentary push-button between the configured trigger GPIO and GND. Hold the button while resetting the device. The serial log will show:

```
I (nnn) remoteid_ota: OTA mode triggered (firmware 1.0.0), starting management server
I (nnn) remoteid_ota: OTA server ready on http://192.168.4.1:80, connect to SSID 'RemoteID-OTA' (WPA2)
I (nnn) remoteid_ota: Endpoints: GET /status  POST /update  POST /nvs  POST /factory-reset  POST /rollback
```

Connect to the AP (default SSID `RemoteID-OTA`) from a laptop or phone.

### Testing OTA updates

The steps below cover the full test cycle from first flash through update and rollback. All `make` commands assume the devcontainer with `ESPPORT` set and the device connected via the serial bridge.

#### Step 1: first flash

The OTA partition table must be on the device before the OTA server can function. A standard `make flash` writes everything required: bootloader, partition table, initial OTA data, and the firmware image into `ota_0`.

```sh
make flash
```

After boot the device runs normally from `ota_0`. The `ota_1` slot is empty and `rollback_possible` is `false`.

#### Step 2: enable and configure OTA

In `menuconfig` under **ESP Remote ID → OTA update server**:

1. Enable **Enable OTA update server**.
2. For bench testing without a physical button, also enable **Always enter OTA mode on boot**. This skips the GPIO check so every reset drops straight into OTA mode; disable it before deploying.
3. For hardware testing, set **OTA trigger GPIO** to the pin wired to your button and leave **Always enter OTA mode** disabled.
4. Optionally set a WPA2 passphrase under **OTA Wi-Fi AP password**.

Rebuild and reflash after any menuconfig change:

```sh
make flash
```

#### Step 3: enter OTA mode and verify the server

If **Always enter OTA mode** is enabled, reset the device. If using a GPIO trigger, hold the button while pressing reset. Confirm the server is up:

```sh
make ota-status
```

Expected output:

```json
{
    "firmware_version": "1.0.0",
    "idf_version": "v5.5.4",
    "running_partition": "ota_0",
    "next_partition": "ota_1",
    "rollback_possible": false,
    "free_heap": 215340
}
```

#### Step 4: build and upload a new firmware image

Make any change to the firmware (for example, bump the version string in `CMakeLists.txt` or add a log line), then build and upload:

```sh
make ota-flash
```

`make ota-flash` builds the firmware, reads `build/project_description.json` to locate the binary, and streams it to `POST /update`. The device validates the image and reboots into `ota_1` roughly 500 ms after the response is received. The terminal will show the curl response:

```json
{"status":"ok","message":"Update applied, rebooting"}
```

#### Step 5: confirm the update

Wait a few seconds for the device to reboot, then enter OTA mode again and query status:

```sh
make ota-status
```

`running_partition` should now be `ota_1` and `rollback_possible` should be `true`, confirming the previous slot is intact.

#### Step 6: test rollback

With `rollback_possible: true`, test rolling back to the previous firmware:

```sh
make ota-rollback
```

The device reboots into `ota_0`. Query status again to confirm `running_partition` is back to `ota_0` and `rollback_possible` is now `false` (the `ota_1` slot was marked invalid by the rollback).

#### Iterating further

Repeating steps 4 and 5 alternates between `ota_0` and `ota_1` on every successful update. Each update overwrites the slot that is not currently running, so there is always exactly one previous firmware available for rollback after a successful update.

If flash encryption Development mode is active, OTA update images are uploaded in plaintext over Wi-Fi and the OTA subsystem writes them encrypted to flash transparently. No `encrypted-flash` step is required for OTA updates.

### HTTP API

A machine-readable OpenAPI 3.0 specification for all endpoints is available at [`.dev/ota-openapi.yaml`](.dev/ota-openapi.yaml). Import it into any OpenAPI-compatible tool (Swagger UI, Insomnia, Postman, Bruno, etc.) for interactive testing against a live device.

All endpoints accept and return JSON (except `POST /update` which accepts a raw binary body).

#### `GET /status`

Returns firmware and partition information.

```sh
curl http://192.168.4.1/status
# or: make ota-status
```

```json
{
  "firmware_version": "1.0.0",
  "idf_version": "v5.5.4",
  "running_partition": "ota_0",
  "next_partition": "ota_1",
  "rollback_possible": false,
  "free_heap": 215000
}
```

#### `POST /update`

Streams a new firmware binary to the next OTA partition and reboots. The device validates the image before setting the boot partition.

```sh
# Using curl directly:
curl -X POST http://192.168.4.1/update \
    --data-binary @build/remoteid.bin \
    -H "Content-Type: application/octet-stream"

# Using make (locates the binary automatically from build/project_description.json):
make ota-flash
# Override target: make ota-flash OTA_HOST=http://192.168.4.1
```

#### `POST /nvs`

Writes a value to the device NVS. The primary use case is provisioning a new Ed25519 private key to a sealed device.

```sh
# Provision a private key (recommended via the provisioning script):
make ota-provision-key KEY_FILE=device.pem
# or: python .dev/scripts/provision_key.py device.pem --ota-url http://192.168.4.1

# Manual JSON example (string value):
curl -X POST http://192.168.4.1/nvs \
    -H "Content-Type: application/json" \
    -d '{"namespace":"remoteid_auth","key":"private_key","type":"string","value":"-----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----\n"}'

# Binary value (base64-encoded blob):
curl -X POST http://192.168.4.1/nvs \
    -H "Content-Type: application/json" \
    -d '{"namespace":"my_ns","key":"my_key","type":"blob","value":"<base64>"}'
```

Request body fields:

| Field | Type | Description |
|-------|------|-------------|
| `namespace` | string | NVS namespace (max 15 characters) |
| `key` | string | NVS key name (max 15 characters) |
| `type` | string | `"string"` or `"blob"` |
| `value` | string | Value to store; plain string or base64-encoded bytes for blobs |

#### `POST /rollback`

Marks the current firmware as invalid and reboots into the previous OTA partition. Requires an explicit confirmation string.

```sh
curl -X POST http://192.168.4.1/rollback \
    -H "Content-Type: application/json" \
    -d '{"confirm":"ROLLBACK"}'
# or: make ota-rollback
```

Returns HTTP 409 if there is no previous firmware to roll back to.

#### `POST /factory-reset`

Erases the NVS partition and reboots. All stored keys and configuration are lost.

```sh
curl -X POST http://192.168.4.1/factory-reset \
    -H "Content-Type: application/json" \
    -d '{"confirm":"FACTORY-RESET"}'
# or: make ota-factory-reset
```

> **Warning:** Factory reset permanently erases the NVS partition. If the device is in Release mode flash encryption and the provisioned private key was the only copy, it is gone. Back up private keys before performing a factory reset.

### Partition table

OTA requires a dual-partition layout. The firmware uses a custom `partitions.csv` instead of the single-app default:

| Name | Type | Size | Purpose |
|------|------|------|---------|
| `nvs` | data/nvs | 24 KB | NVS key-value store (private key, config) |
| `otadata` | data/ota | 8 KB | OTA boot slot tracking |
| `phy_init` | data/phy | 4 KB | RF calibration data |
| `ota_0` | app/ota_0 | 2 MB | First firmware slot |
| `ota_1` | app/ota_1 | 2 MB | Second firmware slot |

The first flash writes the firmware to `ota_0`. Subsequent OTA updates alternate between slots. `GET /status` shows which partition is active and which is next.

### OTA security notes

- The HTTP server has no authentication beyond the Wi-Fi AP password. Use a strong WPA2 passphrase in any environment with untrusted wireless neighbours.
- OTA mode is only active while triggered. Normal Remote ID operation resumes after every reboot without the trigger asserted.
- When flash encryption Release mode is active, the OTA server is the only way to update firmware or provisioned secrets after the device is sealed.
- The AP MAC address is randomised on every OTA boot to avoid persistent device identification while in management mode.

### BLE schedule with authentication

When authentication is enabled the BLE schedule extends from 8 to 12 slots (3 seconds per full cycle at 250 ms per slot). Auth pages 0–3 are appended after the base message rotation. The Location and System messages are refreshed immediately before signing so auth pages always cover the most recent state.

## Web Configurator (Cloudbuild)

A web-based firmware configurator allows users to configure and build the firmware without installing the ESP-IDF toolchain. The user fills in a form (UAS ID, operator ID, transports, position, etc.), and the backend generates a `sdkconfig` overlay, triggers a GitHub Actions build, and returns a ready-to-flash firmware ZIP.

Because the repository is public, GitHub Actions build minutes are free and unlimited.

### Architecture

```
Browser (web form)
    │  POST /build { uas_id, operator_id, transports, ... }
    ▼
Cloudbuild server (.dev/cloudbuild/)
    │  1. Validate inputs
    │  2. Render sdkconfig overlay
    │  3. Base64-encode overlay
    │  4. Generate correlation UUID (build_id)
    │  5. POST /actions/workflows/cloudbuild.yml/dispatches
    │     via GitHub API with { build-id, sdkconfig-overlay-b64 }
    │  6. Return { build_id }
    ▼
Browser polls GET /build/{build_id}/status
    │
    ▼
Cloudbuild server
    → lists GitHub Actions runs for cloudbuild.yml
    → matches run by name (run-name: cloudbuild-{build_id})
    → returns { status, conclusion, download_url? }
    │
    ▼ (completed + success)
Browser GET /build/{build_id}/download
    → server proxies artifact ZIP from GitHub (token never reaches browser)
```

The `run-name: cloudbuild-{build_id}` field on the workflow is what ties a dispatch to a specific poll without needing to store server-side state.

### Reusable action

The GitHub Actions build logic is extracted into a reusable workflow at [`.dev/actions/`](.dev/actions/). Publish that directory's contents into the `peinser/actions` repository and tag it `v1`. Any other ESP-IDF project can then use it:

```yaml
jobs:
  firmware:
    uses: peinser/actions/.github/workflows/esp-remoteid-build.yml@v1
    with:
      idf-version: release-v5.5
      target: esp32s3
    permissions:
      contents: read
```

### Self-hosting the configurator backend

The cloudbuild server is a Node.js 22 service at [`.dev/cloudbuild/`](.dev/cloudbuild/). It requires a GitHub fine-grained PAT with `actions: write` and `actions: read` on this repository.

```sh
cp .dev/cloudbuild/.env.example .dev/cloudbuild/.env
# Edit .env: set GITHUB_TOKEN, GITHUB_OWNER, GITHUB_REPO
docker compose -f .dev/cloudbuild/docker-compose.yml up -d
```

Point `ALLOWED_ORIGIN` at your web configurator frontend's origin. Put the service behind a reverse proxy (nginx, Caddy) for TLS before exposing it publicly.

For local development without Docker:

```sh
cd .dev/cloudbuild
cp .env.example .env   # fill in GITHUB_TOKEN
npm install
npm run dev            # runs on http://localhost:3000
```

### Publishing the reusable action

```sh
# Copy the contents of .dev/actions/ into the peinser/actions repository, then:
git tag v1
git push origin main v1
```

The cloudbuild.yml workflow in this repository references `peinser/actions/.github/workflows/esp-remoteid-build.yml@v1`. Create and tag that repository before triggering any cloudbuild dispatch.

### API reference

| Endpoint | Method | Description |
|---|---|---|
| `/healthz` | GET | Health check; returns `{"ok":true}` |
| `/build` | POST | Submit a build request; returns `{"build_id":"..."}` |
| `/build/:id/status` | GET | Poll build status; returns `{status, conclusion, run_url, download_url?}` |
| `/build/:id/download` | GET | Download firmware ZIP (proxied from GitHub) |

**POST /build request body:**

| Field | Type | Required | Description |
|---|---|---|---|
| `uas_id` | string | Yes | UAS ID, max 20 characters |
| `operator_id` | string | Yes | Operator registration ID, max 20 characters |
| `id_type` | string | Yes | `caa_reg` or `serial` |
| `ua_type` | string | Yes | `multirotor`, `aeroplane`, `hybrid_lift`, or `other` |
| `eu_category` | string | Yes | `undeclared`, `open`, `specific`, or `certified` |
| `eu_class` | string | Yes | `undeclared`, `c0` through `c6` |
| `transport_ble` | boolean | Yes | Enable BLE 4 legacy advertising |
| `transport_ble5` | boolean | Yes | Enable BLE 5 Long Range |
| `transport_wifi_beacon` | boolean | Yes | Enable Wi-Fi Beacon |
| `transport_wifi_nan` | boolean | Yes | Enable Wi-Fi NAN |
| `ble_tx_power` | string | No | `n12`, `n9`, `n6`, `n3`, `n0`, `p3`, `p6`, or `p9` |
| `ble_tx_interval_ms` | number | No | 100-5000 ms, default 250 |
| `has_position` | boolean | No | Broadcast a known takeoff position |
| `takeoff_lat_1e6` | number | If `has_position` | Latitude degrees x 10^6 |
| `takeoff_lon_1e6` | number | If `has_position` | Longitude degrees x 10^6 |
| `takeoff_alt_m` | number | If `has_position` | Altitude above WGS-84 ellipsoid in metres |
| `startup_delay_ms` | number | No | 0-60000 ms, defaults to 0 for cloudbuild |
| `ota_enable` | boolean | No | Include OTA update server |
| `ota_trigger_gpio` | number | No | GPIO for OTA trigger button (-1 to 48) |

## TODO

- [ ] DroneCAN inputs and state processing.
- [ ] Signed OTA, validate firmware binaries against stored public keys before flashing.
