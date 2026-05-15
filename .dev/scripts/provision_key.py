#!/usr/bin/env python3
"""
Provision an Ed25519 private key into the NVS partition of a Remote ID device.

Two provisioning methods are available:

  Serial (before first boot / flash encryption):
    Generates an NVS binary from the supplied PKCS#8 PEM file and writes it to
    the device's NVS partition via serial. Must be run before first boot when
    flash encryption is enabled, as the plaintext NVS binary is rejected after.

  OTA HTTP server (while device is running in OTA mode):
    Posts the key directly to the running OTA management server over Wi-Fi.
    The device must be in OTA mode (REMOTEID_OTA_ENABLE + trigger asserted).

Usage:
    python .dev/scripts/provision_key.py device.pem
    python .dev/scripts/provision_key.py device.pem --port /dev/ttyUSB0
    python .dev/scripts/provision_key.py device.pem --output nvs.bin
    python .dev/scripts/provision_key.py device.pem --ota-url http://192.168.4.1
"""

import argparse
import csv
import json
import os
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

NVS_NAMESPACE     = "remoteid_auth"
NVS_KEY           = "private_key"
NVS_PARTITION_SIZE = 0x6000  # 24 K matches the default single-app partition table


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def nvs_gen_script():
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        die("IDF_PATH is not set — source the ESP-IDF environment first:\n"
            "  . $IDF_PATH/export.sh")
    path = os.path.join(idf_path, "components", "nvs_flash",
                        "nvs_partition_generator", "nvs_partition_gen.py")
    if not os.path.isfile(path):
        die(f"nvs_partition_gen.py not found at {path}")

    return path


def read_pem(path):
    try:
        with open(path) as f:
            pem = f.read().strip()
    except OSError as e:
        die(str(e))

    if "BEGIN PRIVATE KEY" not in pem:
        die(f"{path} does not look like a PKCS#8 PEM private key.\n"
            "  Generate one with: openssl genpkey -algorithm ed25519 -out device.pem")
    if "END PRIVATE KEY" not in pem:
        die(f"{path}: PEM footer missing")

    return pem


def generate_nvs_binary(pem, output_path):
    # Escape real newlines so the value fits on one CSV line.
    # The firmware PEM parser accepts both real newlines and \n escape sequences.
    pem_oneline = pem.replace("\n", "\\n")
    if not pem_oneline.endswith("\\n"):
        pem_oneline += "\\n"

    with tempfile.NamedTemporaryFile(mode="w", suffix=".csv",
                                     delete=False, newline="") as f:
        csv_path = f.name
        writer = csv.writer(f)
        writer.writerow(["key", "type", "encoding", "value"])
        writer.writerow([NVS_NAMESPACE, "namespace", "", ""])
        writer.writerow([NVS_KEY, "data", "string", pem_oneline])

    try:
        result = subprocess.run(
            [sys.executable, nvs_gen_script(),
             "generate", csv_path, output_path, hex(NVS_PARTITION_SIZE)],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            die(f"nvs_partition_gen.py failed:\n{result.stderr.strip()}")

    finally:
        os.unlink(csv_path)


def flash_nvs_partition(bin_path, port):
    result = subprocess.run(
        ["parttool.py", "-p", port,
         "write_partition", "--partition-name", "nvs", "--input", bin_path],
        capture_output=True, text=True,
    )

    if result.returncode != 0:
        die(f"parttool.py failed:\n{result.stderr.strip()}")


def provision_via_ota(pem, ota_url):
    payload = {
        "namespace": NVS_NAMESPACE,
        "key": NVS_KEY,
        "type": "string",
        "value": pem,
    }
    data = json.dumps(payload).encode()
    url = ota_url.rstrip("/") + "/nvs"

    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            body = resp.read().decode()
            print(f"OTA server response: {body}")
    except urllib.error.HTTPError as e:
        die(f"OTA server error {e.code}: {e.read().decode()}")
    except urllib.error.URLError as e:
        die(f"OTA server unreachable at {url}: {e.reason}\n"
            "  Make sure the device is in OTA mode and you are connected to its AP.")


def main():
    parser = argparse.ArgumentParser(
        description="Provision an Ed25519 private key into the device NVS partition.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("key_file", help="PKCS#8 PEM private key file (e.g. device.pem)")
    parser.add_argument(
        "--port",
        default=os.environ.get("ESPPORT", "/dev/ttyESP32"),
        help="Serial port (default: $ESPPORT or /dev/ttyESP32)",
    )
    parser.add_argument(
        "--output",
        metavar="FILE",
        help="Write NVS binary to FILE and exit without flashing",
    )
    parser.add_argument(
        "--ota-url",
        metavar="URL",
        help="Provision via OTA HTTP server (e.g. http://192.168.4.1) instead of serial",
    )
    args = parser.parse_args()

    pem = read_pem(args.key_file)

    if args.ota_url:
        print(f"Provisioning key via OTA server at {args.ota_url} ...")
        provision_via_ota(pem, args.ota_url)
        print("Private key provisioned successfully via OTA.")
    elif args.output:
        generate_nvs_binary(pem, args.output)
        print(f"NVS binary written to {args.output}")
        print(f"Flash with: parttool.py -p <PORT> write_partition --partition-name nvs --input {args.output}")
    else:
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            bin_path = f.name

        try:
            generate_nvs_binary(pem, bin_path)
            print(f"Flashing NVS partition to {args.port} ...")
            flash_nvs_partition(bin_path, args.port)
            print("Private key provisioned successfully.")

        finally:
            os.unlink(bin_path)


if __name__ == "__main__":
    main()
