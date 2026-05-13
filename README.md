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

Set both to your actual registration values before any flight. The placeholder defaults (`CHANGE_ME_*`) are not valid for operation.

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

BLE is enabled by default. Wi-Fi Beacon and Wi-Fi NAN are available but disabled by default so receiver testing can isolate one transport at a time.

| Option | Default | Description |
|--------|---------|-------------|
| `Enable Bluetooth LE legacy advertisements` | Enabled | Broadcasts Android-compatible BLE service-data advertisements on UUID `0xFFFA`. |
| `Enable Wi-Fi Beacon advertisements` | Disabled | Broadcasts OpenDroneID Message Pack payloads in Wi-Fi Beacon vendor IEs. |
| `Enable Wi-Fi NAN advertisements` | Disabled | Broadcasts OpenDroneID Message Pack payloads in Wi-Fi NAN sync/action frames. |
| `Wi-Fi Remote ID channel` | `6` | 2.4 GHz channel used by the ESP32-S3 Wi-Fi transport. |
| `Wi-Fi Remote ID transmit interval` | `1000 ms` | Remote ID Wi-Fi cadence. |
| `Wi-Fi TX power` | `20 dBm` | Converted internally to ESP-IDF quarter-dBm units. |
| `Wi-Fi Beacon SSID` | `OpenDroneID` | SSID embedded only in Beacon frames. Receivers should parse the vendor IE, not rely on this SSID. |
| `TX status LED GPIO` | Disabled (`-1`) | Optional simple GPIO LED that pulses after each successful BLE, Wi-Fi Beacon, or Wi-Fi NAN transmission. |
| `TX status LED is active high` | Enabled | Disable for active-low LEDs. |
| `TX status LED pulse duration` | `40 ms` | Duration of each transmission pulse. |

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

## Architecture

The ESP-IDF component is split so transports can share the same Remote ID state and OpenDroneID encoding:

- `src/remoteid/main.c`: application entrypoint and top-level startup.
- `src/remoteid/remoteid/config.h`: compile-time configuration constants from Kconfig.
- `src/remoteid/remoteid/types.h`: internal Remote ID state and message bundle types.
- `src/remoteid/remoteid/model.c`: builds the current aircraft/operator/location state from configuration.
- `src/remoteid/remoteid/encode.c`: converts internal state into official OpenDroneID messages.
- `src/remoteid/remoteid/led.c`: optional TX status LED pulse handling.
- `src/remoteid/remoteid/ble.c`: NimBLE lifecycle, TX power, and BLE legacy advertisement rotation.
- `src/remoteid/remoteid/wifi.c`: ESP-IDF Wi-Fi setup and OpenDroneID Wi-Fi Beacon/NAN transmission.

Both transports use the same `model` and `encode` modules instead of rebuilding message payloads independently.

## Transmit Power

OpenDroneID BLE reception range is not guaranteed by a fixed distance in the EU documents; practical range depends on transmitter power, receiver hardware, antenna design, enclosure, orientation, and RF environment. This firmware therefore makes BLE advertising TX power explicit and configurable.

The default is `+9 dBm`, the highest ESP32-S3 BLE level exposed by ESP-IDF:

```text
CONFIG_REMOTEID_BLE_TX_POWER_P9=y
```

Change it with `make menuconfig` under `ESP Remote ID -> BLE advertising TX power`, or by editing `sdkconfig.defaults` before regenerating `sdkconfig`. Keep the configured level within the limits of your board, antenna, enclosure, and local RF rules.

## macOS Serial Bridge

Docker Desktop on macOS does not expose `/dev/cu.*` serial devices directly to Linux containers. Use `socat` to bridge the ESP32-S3 USB serial device from the host into the devcontainer.

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

The screenshot below shows the OpenDroneID Android app receiving live advertisements from the module. The placeholder identifiers (`CHANGE_ME_UAS_ID`, `CHANGE_ME_OP_ID`) are visible, replace these with your registration details before any flight.

<p align="center">
  <img src=".github/images/android-opendroneid.png" width="300" alt="OpenDroneID Android app showing one detected drone with Basic ID, Location (no position), System, and Operator ID messages">
</p>
