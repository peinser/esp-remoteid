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
| **UAS ID** | The ANSI/CTA-2063-A serial number of the Remote ID system, broadcast in the Basic ID message. Max 20 characters. |
| **Operator registration ID** | Your national operator registration number. For an EU number enter the full form with its 3 secret characters, e.g. `FIN87astrdge12k8-xyz`. Max 20 characters. |

Set both to your actual values before any flight. The placeholder defaults (`CHANGE_ME_*`) are not valid for operation and intentionally block all broadcasts until replaced or overridden by valid MAVLink or DroneCAN input.

**EU operator registration number.** The number has 16 public characters (3-letter country code, 12 characters, 1 check character) and 3 secret characters that only the operator knows. Enter all 20 as `<16 public>-<3 secret>`, in Kconfig or from the ground station. The firmware verifies the check character against the secret part and then drops the secret: only the 16 public characters are stored and broadcast. A wrong secret or a typo is rejected, and the module stays not-ready.

### UAS ID type

In the EU the UAS ID is always the serial number: Regulation (EU) 2019/945 (Part 6 of the Annex) requires the Remote ID system to broadcast its own ANSI/CTA-2063-A serial number for an add-on module, or the serial number of the aircraft when the system is built in. The serial has the form `<4-character manufacturer code><length character 1-F><serial>`, using digits and upper-case letters without `O` and `I`, e.g. `1ABC5XYZ12`. The manufacturer code is issued by the ICAO through CTA.

Outside strict compliance mode the ID type can also be set to **CAA Registration ID** for testing, or for jurisdictions that accept a registration number as the UAS ID. That is not an EU-compliant configuration.

### Strict EU (EASA) compliance mode

`Strict EU (EASA) compliance mode` enforces the EU rules at runtime instead of only warning:

| Rule | Normal build | Strict build |
|------|--------------|--------------|
| UAS ID | Any ID type; Basic ID from MAVLink/DroneCAN replaces the configured one | Serial number configured at build time; Basic ID from MAVLink/DroneCAN is ignored, so the serial cannot be changed in operation |
| Malformed CTA-2063-A serial | Logged as a warning, still broadcast | Never broadcast; not-ready, error indicator, `Invalid UAS serial (CTA-2063-A)` reported to the autopilot |
| Operator ID | 16-character ID accepted unverified; a 20-character `-xyz` form is verified and stripped | Only the 20-character form, verified and stripped |
| UA type | Configured, or from Basic ID input | Configured at build time |

With the runtime identity source, a strict build still takes the operator ID and the System data (operator location, category, class) from the flight controller or ground station; only the serial and UA type are fixed.

The "cannot be modified" requirement also covers reflashing. A production device needs Secure Boot and flash encryption (see [Flash encryption](#flash-encryption)) and no unauthenticated OTA access (see [OTA security notes](#ota-security-notes)); otherwise anyone with physical or Wi-Fi access can install firmware with a different serial.

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

Enable this option and enter the takeoff coordinates. The firmware latches these values at compile time and broadcasts them as the UA and operator position (EU regulation permits using the takeoff location as the operator location for add-on modules). The configured altitude is geodetic (WGS-84), so the barometric altitude is broadcast as unknown. A fixed UA position does not follow the aircraft in flight; use it for bench testing, not as a substitute for live position input.

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
| `BLE advertisement rotation interval` | `250 ms` | BLE payload rotation cadence for both BLE transports (100 to 260 ms). Location and System are sent every third slot (every 750 ms at the default); Basic ID, Operator ID and Self ID once per 9-slot cycle, with the authentication pages spread between them. The 260 ms maximum keeps Location and System within 1 s and every other message within 3 s (see [BLE schedule with authentication](#ble-schedule-with-authentication)). |
| `Enable Wi-Fi Beacon advertisements` | Disabled | Broadcasts OpenDroneID Message Pack payloads in Wi-Fi Beacon vendor IEs. |
| `Enable Wi-Fi NAN advertisements` | Disabled | Broadcasts OpenDroneID Message Pack payloads in Wi-Fi NAN sync/action frames. |
| `Wi-Fi Remote ID channel` | `6` | 2.4 GHz channel used by the ESP32-S3 Wi-Fi transport. |
| `Wi-Fi TX power` | `20 dBm` | Converted internally to ESP-IDF quarter-dBm units. |
| `Wi-Fi Beacon SSID` | `OpenDroneID` | SSID embedded only in Beacon frames. Receivers should parse the vendor IE, not rely on this SSID. |
| `Wi-Fi Beacon TX interval` | `1000 ms` | Wi-Fi Beacon transmission cadence (100 to 1000 ms, fixed rate). Every frame carries the full message pack. |
| `Wi-Fi NAN TX interval` | `1000 ms` | Wi-Fi NAN sync/action transmission cadence (100 to 1000 ms, fixed rate). |
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

Operational means the same as `GOOD_TO_ARM`: a valid Basic ID/UAS ID and Operator ID, all enabled transports running without a reported fault and, when the position comes from MAVLink or DroneCAN, a Location received within the last 3 s. Anything else shows the waiting pattern; a latched Remote ID system failure shows the error pattern.

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

It also watches `HEARTBEAT` and `SYSTEM_TIME` from the autopilot. The module binds to the first `HEARTBEAT` from an autopilot component (`MAV_COMP_ID_AUTOPILOT1`) of the configured target system, or of any system when none is configured, and never re-binds. Arm state (takeoff capture, emergency status) and the UTC clock are taken only from that autopilot; a GCS or companion `SYSTEM_TIME` is ignored. The arm state is unknown until the first `HEARTBEAT`, so a module that reboots while the vehicle is armed does not capture its in-flight position as the takeoff point.

Only data about this aircraft is accepted. A message with a non-zero `id_or_mac`, or one sent by an Open Drone ID transmitter/receiver component (`236`-`238`), describes another aircraft seen by a Remote ID receiver and is ignored, including inside `OPEN_DRONE_ID_MESSAGE_PACK`. A `0/0` target alone does not make a message about this aircraft.

Every input path (MAVLink messages, message packs, DroneCAN, Kconfig) goes through the same validation before it changes the broadcast state:

- The UAS ID must match its ID type. Serial number and CAA registration IDs are printable ASCII; UTM UUIDs and specific session IDs are binary and stored byte for byte, including NUL bytes. ID type `None`, reserved types and empty IDs are rejected and never replace a valid identity.
- A full EU operator registration number entered with its secret part (`FIN87astrdge12k8-xyz`) is checksum-verified (Luhn mod 36), and only the 16 public characters are stored and broadcast. A wrong secret part is rejected. In strict compliance mode only this form is accepted, and Basic ID input is ignored (see [Strict EU (EASA) compliance mode](#strict-eu-easa-compliance-mode)).
- An update that the OpenDroneID encoder cannot represent (out-of-range enum, coordinate, speed or authentication page) is rejected as a whole instead of stopping the broadcast.

`OPEN_DRONE_ID_MESSAGE_PACK` is unpacked and each contained OpenDroneID message is applied as if it had arrived on its own, so a flight controller can deliver the full set in a single frame. Authentication pages (`OPEN_DRONE_ID_AUTHENTICATION`, or inside a message pack) are used only when the authentication source is **Relay from MAVLink**; see [Authentication](#authentication).

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

With `DID_ENABLE=1`, ArduPilot sends Location and System (operator location) at `DID_RATE_HZ`, and Basic ID, Self ID, Operator ID and System in rotation, as individual `OPEN_DRONE_ID_*` messages. Operator ID, Self ID, System and the operator location come from the ground station; Basic ID from the ground station or the `DID_UAS_ID` parameters. ArduPilot does not create Authentication messages: a ground station's `OPEN_DRONE_ID_AUTHENTICATION` reaches the module through ArduPilot's MAVLink routing. ArduPilot also expects the module to talk back; see the status output section.

**PX4.** Build with the `remote_id` module included and enable it with `UXRCE`-independent parameter `RID_ENABLE = 1` (PX4's OpenDroneID driver). PX4 streams the same `OPEN_DRONE_ID_*` MAVLink messages on the configured MAVLink instance; ensure the serial port's MAVLink mode forwards the OpenDroneID stream. PX4 support is less mature than ArduPilot's and may require a recent main build; consult the PX4 OpenDroneID docs for your version.

Either way, this module consumes the standard messages once they are flowing; no module-side change is needed to switch between ArduPilot and PX4 sources.

| Option | Default | Description |
|--------|---------|-------------|
| `Enable MAVLink OpenDroneID input` | Disabled | Enables the MAVLink parser and state producer. |
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
| `Live operator-location expiry (ms)` | `10000` | Drop a live operator location if no GCS refresh arrives within this window. |

#### Status output and arm gating

When `Send HEARTBEAT and OPEN_DRONE_ID_ARM_STATUS` is enabled, the module periodically transmits two messages back to the flight controller (this requires bidirectional UART wiring, i.e. the TX GPIO connected to the flight controller's RX):

- `HEARTBEAT` with component type `MAV_TYPE_ODID`, so the flight controller and ground station see the Remote ID module as a live component of the vehicle.
- `OPEN_DRONE_ID_ARM_STATUS`, reporting `GOOD_TO_ARM` only when a usable identity is present, all enabled transports are running without a reported fault and (for a MAVLink/DroneCAN position source) a Location arrived within the last 3 s. Otherwise `PRE_ARM_FAIL_GENERIC` with the reason (for example "No fresh aircraft location" or "Remote ID transmitters not running").

This is what lets ArduPilot treat the module as present and healthy. With the relevant `DID_OPTIONS` bit set, ArduPilot refuses to arm until it receives `GOOD_TO_ARM`, so the drone cannot take off without a working Remote ID broadcast. The module shares the flight controller's system ID (adopted automatically by default) and uses an Open Drone ID component ID, so the status appears as part of the same vehicle.

#### Operator-location freshness loop

OpenDroneID requires the operator location to be refreshed continuously (the autopilot treats a stale operator location as a pre-arm failure). A ground station can do this by streaming `OPEN_DRONE_ID_SYSTEM_UPDATE` at ~1 Hz, but that ties compliance to the GCS staying connected. With `Own the operator-location freshness loop` enabled, the firmware keeps the operator location fresh **on the aircraft**, so the UAS location stays autopilot-owned while the operator location becomes aircraft-local.

The mode is derived from the `operator_location_type` carried in the last full `OPEN_DRONE_ID_SYSTEM` message:

| Operator-location type | Firmware behaviour |
|------------------------|--------------------|
| `FIXED` / `TAKEOFF` | The firmware **republishes** `OPEN_DRONE_ID_SYSTEM_UPDATE` (broadcast `target=0/0`) at the configured period (0.5 Hz default), with the fixed position and a fresh timestamp, so the autopilot stays compliant even after the GCS disconnects. |
| `LIVE_GNSS` | The firmware **only consumes** the GCS's live `SYSTEM_UPDATE` stream and **never republishes** it (re-stamping a moving position would falsify freshness). Freshness follows the source `timestamp` carried in `SYSTEM`/`SYSTEM_UPDATE`: only a newer timestamp counts as a refresh, so an autopilot that keeps relaying the last cached packet does not keep the location alive. If no newer timestamp arrives within the expiry window, the operator location is dropped as stale. |

When stale or absent, the operator location is reported via `ARM_STATUS` (`PRE_ARM_FAIL_GENERIC`, "Operator location missing or stale") — this requires status output to also be enabled — and operator position is dropped from the BLE/Wi-Fi broadcast.

The republish timestamp (a Unix-2019 epoch) comes from the shared UTC clock, seeded from the bound autopilot's `SYSTEM_TIME`; until a valid `SYSTEM_TIME` has been seen, the loop holds off transmitting. The same clock stamps the broadcast System and Authentication messages. Without it those timestamps are sent as 0 and a Location without a source timestamp as unknown (`0xFFFF`); uptime is never broadcast as UTC.

On arm the current position becomes the `TAKEOFF` operator location only when the Location is fresh and no source has supplied a `FIXED` or `LIVE_GNSS` operator location; a configured pilot position is never overwritten. The feature requires an outbound MAVLink path (the TX GPIO on the UART transport; inherent on USB Serial/JTAG) and is only available when MAVLink input is enabled.

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

#### Recommended: filter the MAVLink stream in front of the ESP

The ESP only needs a small set of message IDs, and its serial/USB receive buffer is modest. Feeding it the full vehicle telemetry stream wastes link bandwidth and can overflow the receive buffer under load, which corrupts and intermittently drops OpenDroneID frames (for example, a stale broadcast position even though the source is sending fresh `LOCATION`). **Put a MAVLink filter in front of the ESP** so only the messages it consumes are forwarded to it.

With [mavlink-router](https://github.com/mavlink-router/mavlink-router), filter the endpoint's **egress** (messages sent *to* the ESP) with `AllowMsgIdOut`. Note the direction: `AllowMsgIdOut` limits what the router sends to the ESP, while `AllowMsgIdIn` limits what the ESP is allowed to inject back into the mesh — only `AllowMsgIdOut` reduces the ESP's inbound load.

```ini
[TcpEndpoint opendroneid]
Address = 10.0.40.3
Port = 54321
# Messages forwarded TO the ESP. HEARTBEAT (0) for takeoff capture / system-ID
# adoption; SYSTEM_TIME (2) seeds the operator-location loop timestamp; the rest
# are the OpenDroneID inputs and the message pack.
AllowMsgIdOut = 0,2,12900,12901,12902,12903,12904,12905,12915,12919
# Messages the ESP may send back: its HEARTBEAT and ARM_STATUS.
AllowMsgIdIn = 0,12918
```

Message IDs: `0` HEARTBEAT, `2` SYSTEM_TIME, `12900` BASIC_ID, `12901` LOCATION, `12902` AUTHENTICATION, `12903` SELF_ID, `12904` SYSTEM, `12905` OPERATOR_ID, `12915` MESSAGE_PACK, `12918` ARM_STATUS (ESP output), `12919` SYSTEM_UPDATE. Drop `2` and `12919` if the operator-location loop is not used, and drop `0` only if you do not need takeoff-position capture or system-ID adoption.

### DroneCAN (ArduPilot)

DroneCAN is disabled by default. When enabled, the module joins the CAN bus as a DroneCAN node and implements the CAN side of ArduPilot's OpenDroneID support (`DID_CANDRIVER`), with the standard `dronecan.remoteid` messages. DroneCAN is the maintained UAVCAN v0-derived protocol; it is distinct from OpenCyphal/UAVCAN v1. The implementation uses ESP-IDF's built-in TWAI driver and needs no extra library.

| Direction | DroneCAN message | Data type ID | Rate | Purpose |
|-----------|------------------|--------------|------|---------|
| ArduPilot to module | `dronecan.remoteid.BasicID` | `20030` | static rotation | UAS ID, ID type, UA type (ignored in strict mode) |
| ArduPilot to module | `dronecan.remoteid.Location` | `20031` | `DID_RATE_HZ` | Aircraft position, speeds, status, timestamp |
| ArduPilot to module | `dronecan.remoteid.SelfID` | `20032` | static rotation | Self ID |
| ArduPilot to module | `dronecan.remoteid.System` | `20033` | `DID_RATE_HZ` | Operator location, area, EU category/class, timestamp |
| ArduPilot to module | `dronecan.remoteid.OperatorID` | `20034` | static rotation | Operator ID |
| Module to ArduPilot | `dronecan.remoteid.ArmStatus` | `20035` | 1 Hz | `GOOD_TO_ARM` or the pre-arm failure text, same as the MAVLink `ARM_STATUS` |
| Module to bus | `uavcan.protocol.NodeStatus` | `341` | 1 Hz | Node presence. Health is `CRITICAL` after a self-test failure, `OK` otherwise; mode is `OPERATIONAL` |
| Module to requester | `uavcan.protocol.GetNodeInfo` response | `1` (service) | on request | Name `com.peinser.remoteid`, unique ID from the ESP32 MAC |

These messages carry the MAVLink `OPEN_DRONE_ID_*` fields with the same units, so both inputs share one conversion and the same validation (see [MAVLink OpenDroneID Input](#mavlink-opendroneid-input)). `dronecan.remoteid.SecureCommand` (`64`) is not implemented, and DroneCAN has no Authentication message.

Rules applied to DroneCAN input:

- The module binds to the first node that sends a `dronecan.remoteid` message (the autopilot) and ignores Remote ID data from every other node.
- A message with a non-empty `id_or_mac` describes another aircraft and is ignored.
- Multi-frame transfers are checked against their transfer CRC; anonymous (node ID 0) transfers are ignored.
- The UTC clock is seeded once from the first non-zero `System.timestamp`. ArduPilot sends no other UTC time over DroneCAN, and this value comes from the ground station, in whole seconds. Location timestamps come from ArduPilot's GPS time and are used as sent.
- Arming capture does not apply: ArduPilot requires an operator location from the ground station before it arms, and sends it in `System`.

#### ArduPilot setup

1. Build or download ArduPilot firmware with the OpenDroneID feature (many boards need a build from the [custom firmware build server](https://custom.ardupilot.org/)).
2. Set `CAN_P1_DRIVER = 1` (or the port the module is on) and `CAN_D1_PROTOCOL = 1` (DroneCAN), and match the bitrate (`CAN_P1_BITRATE`, 1 Mbit/s by default).
3. Set `DID_ENABLE = 1`, `DID_CANDRIVER = 1` (the DroneCAN driver of that bus) and `DID_MAVPORT = -1`.
4. To block arming without a healthy Remote ID module, enable the pre-arm option in `DID_OPTIONS`. ArduPilot then requires an `ArmStatus` within the last 3 s reporting `GOOD_TO_ARM`, a Basic ID with an ID type, and an operator location.
5. Supply the Operator ID, Self ID and operator location from the ground station, as for the MAVLink connection. In strict mode ArduPilot's own pre-arm check still wants a Basic ID: set the `DID_UAS_ID` parameters to the module's serial so the two agree.

The module's node ID is static (`DroneCAN node ID`, default `100`). It must be unique on the bus; ArduPilot's node allocation server accepts static IDs.

| Option | Default | Description |
|--------|---------|-------------|
| `Enable DroneCAN (ArduPilot OpenDroneID over CAN)` | Disabled | Starts the DroneCAN node. |
| `DroneCAN RX GPIO` | `-1` | ESP32-S3 GPIO connected to the CAN transceiver RXD output. Must be configured. |
| `DroneCAN TX GPIO` | `-1` | ESP32-S3 GPIO connected to the CAN transceiver TXD input. Must be configured. |
| `DroneCAN bitrate` | `1 Mbit/s` | CAN bus bitrate. Match the flight controller bus. |
| `DroneCAN node ID` | `100` | Static node ID, 1 to 125, unique on the bus. |
| `DroneCAN task stack size` | `4096` | Stack of the receive/transmit task. |

The module transmits on the bus (NodeStatus, ArmStatus, GetNodeInfo responses) and acknowledges frames, so it also works on a two-node bus with only the flight controller.

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

### Minimum configuration checklist

Before flashing for any actual operation in the EU:

- [ ] Enable **Strict EU (EASA) compliance mode**
- [ ] Set **UAS ID** to the ANSI/CTA-2063-A serial number of the Remote ID system
- [ ] Set **UA type** to match your airframe
- [ ] Provide the **Operator registration ID** in its full form with the 3 secret characters, in Kconfig (static identity) or from the ground station (runtime identity)
- [ ] Leave **EU equipment class** as **Undeclared** if your drone has no CE class label
- [ ] Set **EU operation category** to **Open** (or your authorised category)
- [ ] Connect a live position source (MAVLink or DroneCAN); a fixed takeoff position is for bench testing only
- [ ] Enable Secure Boot and flash encryption, and keep OTA disabled or protected (see [Strict EU (EASA) compliance mode](#strict-eu-easa-compliance-mode))

Note: BLE and Wi-Fi transports wait for the store readiness gate before transmitting. The gate requires a usable Basic ID/UAS ID and Operator ID, either from Kconfig or from runtime producers such as MAVLink.

For Android app testing, use one transport at a time because many receiver apps do not display whether a detected aircraft came from BLE or Wi-Fi.

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

## Authentication

The broadcast can carry ASTM F3411-22a Authentication pages. They come from exactly one source, chosen under **ESP Remote ID → Authentication → Authentication message source**, so pages of two signatures are never mixed:

| Source | Default | Signing key | What is broadcast |
|--------|---------|-------------|-------------------|
| **Relay from MAVLink** | Yes, when MAVLink input is enabled | With the ground station or autopilot | The pages received in `OPEN_DRONE_ID_AUTHENTICATION` or a message pack, byte for byte |
| **None** | When MAVLink input is disabled | None | No Authentication pages |
| **Local Ed25519** | No | On this module (Kconfig or NVS) | A message set signature made by this module |

Both transports carry all pages: Wi-Fi in every message pack, BLE in the slots described in [BLE schedule with authentication](#ble-schedule-with-authentication). Skipped pages take no advertising time.

### Relay from MAVLink

The module stores the pages of one set: a different page 0 starts a new set and drops the pages of the previous one. A set is broadcast once page 0 and every page up to its `last_page_index` have arrived, and is dropped when page 0 has not been received for 10 s, so a signer that stops is not left on air. Up to 4 pages (64 bytes in page 0 to 3) are supported; larger sets are rejected. DroneCAN has no Authentication message.

The pages go out exactly as the signer produced them. Whether a receiver can verify them depends on what was signed:

- **UAS ID, Operator ID, Network Remote ID and specific-method signatures** cover data this module broadcasts unchanged (the Basic ID is copied byte for byte; the Operator ID is the 16 public characters). They verify as relayed.
- **A message set signature** covers encoded messages. This module re-encodes every message from its own state, adds its own timestamps and schedules Location on its own, so the signature only verifies if the signer signs exactly the messages this module broadcasts. For a verifiable message set signature, use local signing.

### Local Ed25519

The module signs the message set itself, so the signed bytes are the broadcast bytes. For each signature it:

1. Takes the five ODID messages as last broadcast, in the order Basic ID, Location, System, Operator ID, Self ID, each in its 25-byte wire format (125 bytes)
2. Appends the page 0 timestamp, seconds since 2019-01-01 UTC as a 4-byte little-endian integer (129 bytes in total)
3. Signs those bytes with the Ed25519 private key
4. Distributes the 64-byte signature across four authentication pages (page 0: 17 bytes + metadata, pages 1 to 3: 23/23/1 bytes), authentication type message set signature

Because the timestamp is signed, a recorded signature cannot be rebroadcast under a fresh timestamp. A verifier rebuilds the same 129 bytes from the received messages and the page 0 timestamp, and checks the signature with the device's public key.

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

- **Clock.** The timestamp in auth page 0 is seconds since 2019-01-01 UTC from the shared clock, seeded by the autopilot's `SYSTEM_TIME` (MAVLink) or the first `System.timestamp` (DroneCAN). Until then it is 0, and a signed timestamp of 0 gives a verifier nothing to judge freshness by.
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

With local signing selected, if neither source has a valid key, the firmware logs an error, reports a Remote ID system failure (broadcast status, `ARM_STATUS` and the error indicator) and keeps broadcasting unsigned.

### Development configuration

Format the private key for Kconfig (one line, `\n` as separator):

```sh
awk 'NF {printf "%s\\n", $0}' device.pem
```

Select **Local Ed25519** and set the key in `menuconfig` under **ESP Remote ID → Authentication**, or set directly in `sdkconfig`:

```
CONFIG_REMOTEID_AUTH_ED25519=y
CONFIG_REMOTEID_AUTH_PRIVATE_KEY_PEM="-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYD...\n-----END PRIVATE KEY-----\n"
```

For local development, generate your own key as shown above and keep it, and any `sdkconfig` that contains it, out of version control. Never use a development key on a production device.

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
- Without Secure Boot, `POST /update` installs any image, including one with a different UAS serial. For an EU-compliant device enable Secure Boot (OTA images must then be signed) or leave the OTA server out of production builds.
- OTA mode is only active while triggered. Normal Remote ID operation resumes after every reboot without the trigger asserted.
- When flash encryption Release mode is active, the OTA server is the only way to update firmware or provisioned secrets after the device is sealed.
- The AP MAC address is randomised on every OTA boot to avoid persistent device identification while in management mode.

### BLE schedule with authentication

BLE sends one message per slot in a 9-slot rotation, with the authentication pages (100 ms each) spread between the slots:

```text
Location  System  Basic ID     [auth 0]
Location  System  Operator ID  [auth 1]
Location  System  Self ID      [auth 2] [auth 3]
```

With the slot interval T (250 ms by default, at most 260 ms), Location and System are sent at least every 3T + 200 ms (980 ms at the maximum), and every other message and the full authentication set every 9T + 400 ms (2.74 s). A local signature is made at the page 0 slot over the most recently broadcast copy of each message; Location and System are not re-encoded for signing, because their timestamps would then differ from what receivers saw. Pages 1 to 3 of that signature follow later in the same cycle.

## Reusable build workflow

The GitHub Actions build logic is a reusable workflow in [`.dev/actions/`](.dev/actions/), meant to be published as the `peinser/actions` repository (see [`.dev/actions/README.md`](.dev/actions/README.md)). Any ESP-IDF project can then call it:

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

## TODO

- [ ] Signed OTA, validate firmware binaries against stored public keys before flashing.
