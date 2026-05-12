#!/usr/bin/env python3
"""
Scan BLE advertisements for OpenDroneID service data.

This verifies that the ESP is broadcasting BLE legacy advertisements with
OpenDroneID service UUID 0xFFFA, independently of whether a phone Remote ID app
accepts and displays the payload.
"""

from __future__ import annotations

import argparse
import asyncio
import math
import re
import struct
import sys
import time
from dataclasses import dataclass, field


ODID_SERVICE_UUID_16 = "fffa"
ODID_AD_APPLICATION_CODE = 0x0D
ODID_MESSAGE_SIZE = 25
ODID_MESSAGE_NAMES = {
    0: "Basic ID",
    1: "Location",
    2: "Authentication",
    3: "Self ID",
    4: "System",
    5: "Operator ID",
    15: "Message Pack",
}
ODID_ID_TYPES = {
    1: "Serial Number",
    2: "CAA Registration ID",
    3: "UTM Assigned UUID",
    4: "Specific Session ID",
}
ODID_UA_TYPES = {
    2: "Helicopter/Multirotor",
}
EXPECTED_UAS_ID = "ESPD0000000000000001"
EXPECTED_OPERATOR_ID = "FIN87ASTRDGE12K8"


@dataclass
class ScanStats:
    started_at: float
    matches: int = 0
    by_type: dict[int, int] = field(default_factory=dict)


@dataclass(frozen=True)
class ServiceData:
    message: bytes
    counter: int | None
    app_code: int | None


def is_opendroneid_uuid(uuid: str) -> bool:
    normalized = uuid.lower().replace("-", "")
    return normalized == ODID_SERVICE_UUID_16 or normalized.startswith(f"0000{ODID_SERVICE_UUID_16}")


def describe_odid_payload(payload: bytes) -> str:
    service_data = unwrap_service_data(payload)
    if len(service_data.message) < 1:
        return "empty payload"

    message_type = service_data.message[0] >> 4
    protocol_version = service_data.message[0] & 0x0f
    message_name = ODID_MESSAGE_NAMES.get(message_type, "unknown")
    details = decode_odid_payload(message_type, service_data.message)
    detail_text = f", {details}" if details else ""
    framed_text = ""
    if service_data.app_code is not None:
        framed_text = f", app_code=0x{service_data.app_code:02x}, counter={service_data.counter}"

    return (
        f"type={message_type} ({message_name}), protocol={protocol_version}{framed_text}{detail_text}, "
        f"bytes={payload.hex()}"
    )


def unwrap_service_data(payload: bytes) -> ServiceData:
    if len(payload) >= 2 + ODID_MESSAGE_SIZE and payload[0] == ODID_AD_APPLICATION_CODE:
        return ServiceData(message=payload[2:], counter=payload[1], app_code=payload[0])

    return ServiceData(message=payload, counter=None, app_code=None)


def decode_odid_payload(message_type: int, payload: bytes) -> str:
    if len(payload) != ODID_MESSAGE_SIZE:
        return f"invalid-length={len(payload)}"

    if message_type == 0:
        ua_type = payload[1] & 0x0F
        id_type = payload[1] >> 4
        uas_id = payload[2:22].rstrip(b"\x00").decode("ascii", errors="replace")

        return (
            f"id_type={id_type} ({ODID_ID_TYPES.get(id_type, 'unknown')}), "
            f"ua_type={ua_type} ({ODID_UA_TYPES.get(ua_type, 'unknown')}), uas_id={uas_id}"
        )

    if message_type == 1:
        status = payload[1] >> 4
        latitude = struct.unpack_from("<i", payload, 5)[0] / 10000000.0
        longitude = struct.unpack_from("<i", payload, 9)[0] / 10000000.0
        timestamp = struct.unpack_from("<H", payload, 21)[0]

        return f"status={status}, lat={latitude:.7f}, lon={longitude:.7f}, timestamp={timestamp}"

    if message_type == 4:
        latitude = struct.unpack_from("<i", payload, 2)[0] / 10000000.0
        longitude = struct.unpack_from("<i", payload, 6)[0] / 10000000.0
        timestamp = struct.unpack_from("<I", payload, 20)[0]

        return f"operator_lat={latitude:.7f}, operator_lon={longitude:.7f}, timestamp={timestamp}"

    if message_type == 5:
        operator_id_type = payload[1]
        operator_id = payload[2:22].rstrip(b"\x00").decode("ascii", errors="replace")

        return f"operator_id_type={operator_id_type}, operator_id={operator_id}"

    return ""


def validate_static_payloads(samples: dict[int, bytes]) -> list[str]:
    warnings = []

    basic_id = samples.get(0)

    if basic_id:
        basic_id = unwrap_service_data(basic_id).message

        if len(basic_id) != ODID_MESSAGE_SIZE:
            return warnings

        id_type = basic_id[1] >> 4
        ua_type = basic_id[1] & 0x0F
        uas_id = basic_id[2:22].rstrip(b"\x00").decode("ascii", errors="replace")

        if id_type != 1:
            warnings.append(f"Basic ID type is {id_type}, expected 1 (Serial Number)")

        if ua_type != 2:
            warnings.append(f"UA type is {ua_type}, expected 2 (Helicopter/Multirotor)")

        if uas_id != EXPECTED_UAS_ID:
            warnings.append(f"UAS ID is {uas_id!r}, expected {EXPECTED_UAS_ID!r}")

        if not re.fullmatch(r"[A-Z0-9]{6,20}", uas_id):
            warnings.append(f"UAS ID {uas_id!r} is not uppercase alphanumeric 6-20 chars")

    operator = samples.get(5)
    if operator:
        operator = unwrap_service_data(operator).message
        if len(operator) != ODID_MESSAGE_SIZE:
            return warnings

        operator_id_type = operator[1]
        operator_id = operator[2:22].rstrip(b"\x00").decode("ascii", errors="replace")

        if operator_id_type != 0:
            warnings.append(f"Operator ID type is {operator_id_type}, expected 0")

        if operator_id != EXPECTED_OPERATOR_ID:
            warnings.append(f"Operator ID is {operator_id!r}, expected {EXPECTED_OPERATOR_ID!r}")

        if not re.fullmatch(r"[A-Z]{3}[A-Z0-9]{13}", operator_id):
            warnings.append(f"Operator ID {operator_id!r} is not shaped like EU public operator ID")

    location = samples.get(1)
    if location:
        location = unwrap_service_data(location).message
        if len(location) != ODID_MESSAGE_SIZE:
            return warnings

        latitude = struct.unpack_from("<i", location, 5)[0] / 10000000.0
        longitude = struct.unpack_from("<i", location, 9)[0] / 10000000.0

        if not (-90.0 <= latitude <= 90.0 and -180.0 <= longitude <= 180.0):
            warnings.append(f"Location is out of range: {latitude:.7f}, {longitude:.7f}")

    return warnings


def validate_service_framing(samples: dict[int, bytes]) -> list[str]:
    warnings = []
    for message_type, payload in samples.items():
        service_data = unwrap_service_data(payload)
        if service_data.app_code != ODID_AD_APPLICATION_CODE:
            warnings.append(
                f"type {message_type} service data is missing AD Application Code 0x{ODID_AD_APPLICATION_CODE:02X}; "
                "OpenDroneID Android filters for this byte"
            )
        if len(service_data.message) != ODID_MESSAGE_SIZE:
            warnings.append(
                f"type {message_type} OpenDroneID message length is {len(service_data.message)}, "
                f"expected {ODID_MESSAGE_SIZE}"
            )

    return warnings


def distance_km(lat_a: float, lon_a: float, lat_b: float, lon_b: float) -> float:
    earth_radius_km = 6371.0
    lat_a_rad = math.radians(lat_a)
    lat_b_rad = math.radians(lat_b)
    delta_lat = math.radians(lat_b - lat_a)
    delta_lon = math.radians(lon_b - lon_a)
    hav = (
        math.sin(delta_lat / 2.0) ** 2
        + math.cos(lat_a_rad) * math.cos(lat_b_rad) * math.sin(delta_lon / 2.0) ** 2
    )

    return earth_radius_km * 2.0 * math.atan2(math.sqrt(hav), math.sqrt(1.0 - hav))


def validate_distance_filter(
    samples: dict[int, bytes], near_lat: float | None, near_lon: float | None, max_km: float
) -> list[str]:
    if near_lat is None or near_lon is None or 1 not in samples:
        return []

    location = unwrap_service_data(samples[1]).message
    if len(location) != ODID_MESSAGE_SIZE:
        return []

    drone_lat = struct.unpack_from("<i", location, 5)[0] / 10000000.0
    drone_lon = struct.unpack_from("<i", location, 9)[0] / 10000000.0
    km = distance_km(near_lat, near_lon, drone_lat, drone_lon)
    if km <= max_km:
        return [f"advertised drone is {km:.1f} km from provided phone position"]

    return [
        f"advertised drone is {km:.1f} km from provided phone position; "
        f"many phone apps hide aircraft beyond {max_km:g} km or outside the map viewport"
    ]


async def scan(
    timeout_s: float, min_matches: int, near_lat: float | None, near_lon: float | None, max_distance_km: float
) -> int:
    try:
        from bleak import BleakScanner

    except ImportError:
        print("Missing dependency: bleak", file=sys.stderr)
        print("Install on macOS with: python3 -m pip install bleak", file=sys.stderr)
        return 2

    stats = ScanStats(started_at=time.monotonic())
    samples: dict[int, bytes] = {}

    def on_advertisement(device, advertisement_data) -> None:
        for uuid, payload in advertisement_data.service_data.items():
            if not is_opendroneid_uuid(uuid):
                continue

            stats.matches += 1
            message = unwrap_service_data(payload).message
            message_type = message[0] >> 4 if message else -1
            stats.by_type[message_type] = stats.by_type.get(message_type, 0) + 1
            samples.setdefault(message_type, payload)
            elapsed = time.monotonic() - stats.started_at
            name = device.name or advertisement_data.local_name or "unknown"
            rssi = getattr(advertisement_data, "rssi", None)
            rssi_text = f", rssi={rssi}" if rssi is not None else ""
            print(f"[{elapsed:6.2f}s] {device.address} ({name}{rssi_text}): {describe_odid_payload(payload)}")

    print(f"Scanning for OpenDroneID BLE service data 0xFFFA for {timeout_s:g}s...")
    async with BleakScanner(on_advertisement):
        await asyncio.sleep(timeout_s)

    if stats.matches < min_matches:
        print(f"FAIL: observed {stats.matches} OpenDroneID advertisements; expected at least {min_matches}")
        return 1

    for message_type, count in sorted(stats.by_type.items()):
        message_name = ODID_MESSAGE_NAMES.get(message_type, "unknown")
        print(f"  type {message_type:2d} ({message_name}): {count}")

    if 0 not in stats.by_type or 1 not in stats.by_type:
        print("WARN: phone apps usually need at least Basic ID (type 0) and Location (type 1)")

    for warning in validate_static_payloads(samples):
        print(f"WARN: {warning}")

    for warning in validate_service_framing(samples):
        print(f"WARN: {warning}")

    for warning in validate_distance_filter(samples, near_lat, near_lon, max_distance_km):
        print(f"WARN: {warning}")

    print(f"PASS: observed {stats.matches} OpenDroneID advertisements")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=20.0, help="scan duration in seconds")
    parser.add_argument("--min-matches", type=int, default=1, help="minimum matching advertisements required")
    parser.add_argument("--near-lat", type=float, help="phone/current latitude used to detect app distance filtering")
    parser.add_argument("--near-lon", type=float, help="phone/current longitude used to detect app distance filtering")
    parser.add_argument("--max-distance-km", type=float, default=5.0, help="distance threshold used for app-filter warning")

    return parser.parse_args()


def main() -> int:
    args = parse_args()

    return asyncio.run(scan(args.timeout, args.min_matches, args.near_lat, args.near_lon, args.max_distance_km))


if __name__ == "__main__":
    raise SystemExit(main())
