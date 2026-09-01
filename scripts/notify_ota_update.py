#!/usr/bin/env python3
"""
OTA MQTT Notify Script

Called by GitHub Actions on main branch push.
SHA-256 verifies the firmware binary, encrypts an MQTT payload,
and publishes to broker.hivemq.com to trigger OTA on all devices.

Env vars:
    FIRMWARE_AES_KEY  - hex-encoded 32-byte AES key (GitHub secret)
    FIRMWARE_SHA256   - SHA-256 hash of the .bin (set by workflow)
"""

import os
import sys
import json

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
import paho.mqtt.client as mqtt

# ─── Hardcoded config (update when changing factory_config.h) ───────────────
MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 8883
MQTT_TOPIC = "/esp32-ota/trigger/jaikrishn-jayachandran"
DEVICE_FAMILY = ["esp32-gen1", "all"]
FIRMWARE_BIN = "ESP-32-OTA-Firmware-Update.bin"
CONFIG_JSON = "config.json"
# ─────────────────────────────────────────────────────────────────────────────

AES_GCM_IV_LENGTH = 12


def load_aes_key():
    """Load AES-256 key from environment variable."""
    key_hex = os.environ.get("FIRMWARE_AES_KEY")
    if not key_hex:
        print("ERROR: FIRMWARE_AES_KEY environment variable not set")
        sys.exit(1)

    key = bytes.fromhex(key_hex)
    if len(key) != 32:
        print(f"ERROR: AES key must be 32 bytes, got {len(key)}")
        sys.exit(1)

    return key


def load_version():
    """Read version from config.json."""
    if not os.path.exists(CONFIG_JSON):
        print(f"ERROR: {CONFIG_JSON} not found")
        sys.exit(1)

    with open(CONFIG_JSON, "r") as f:
        config = json.load(f)

    version = config.get("version")
    if not version:
        print(f"ERROR: 'version' field missing from {CONFIG_JSON}")
        sys.exit(1)

    return version


def load_firmware_sha256():
    """Get SHA-256 from workflow env or compute it."""
    sha256 = os.environ.get("FIRMWARE_SHA256")
    if sha256:
        return sha256

    if not os.path.exists(FIRMWARE_BIN):
        print(f"ERROR: {FIRMWARE_BIN} not found")
        sys.exit(1)

    import hashlib
    with open(FIRMWARE_BIN, "rb") as f:
        sha256 = hashlib.sha256(f.read()).hexdigest()

    return sha256


def encrypt_payload(aes_key, plaintext):
    """Encrypt JSON payload with AES-256-GCM.

    Returns: iv(12) + tag(16) + ciphertext
    """
    iv = os.urandom(AES_GCM_IV_LENGTH)
    aesgcm = AESGCM(aes_key)
    ciphertext_with_tag = aesgcm.encrypt(iv, plaintext, None)
    return iv + ciphertext_with_tag


def main():
    print("=== OTA MQTT Notify ===")

    # Load config
    aes_key = load_aes_key()
    version = load_version()
    sha256 = load_firmware_sha256()

    print(f"Broker:     {MQTT_BROKER}:{MQTT_PORT}")
    print(f"Topic:      {MQTT_TOPIC}")
    print(f"Version:    {version}")
    print(f"SHA-256:    {sha256}")
    print(f"Family:     {DEVICE_FAMILY}")

    # Build plaintext JSON
    payload = json.dumps({
        "event": "OTA_AVAILABLE",
        "version": version,
        "sha256": sha256,
        "family": DEVICE_FAMILY,
    })
    print(f"Plaintext:  {payload}")

    # Encrypt
    encrypted = encrypt_payload(aes_key, payload.encode())
    print(f"Encrypted:  {len(encrypted)} bytes")

    # MQTT publish
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.tls_set()

    print(f"Connecting to {MQTT_BROKER}:{MQTT_PORT}...")
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=10)

    result = client.publish(MQTT_TOPIC, encrypted, qos=1)
    result.wait_for_publish()

    print("MQTT message published successfully!")
    client.disconnect()
    print("=== Done ===")


if __name__ == "__main__":
    main()
