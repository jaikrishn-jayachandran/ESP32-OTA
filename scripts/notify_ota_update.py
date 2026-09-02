#!/usr/bin/env python3
"""
OTA MQTT Notify Script

Reads all configuration from config.json (repository root).
Called by GitHub Actions on main branch push.

Env vars:
    FIRMWARE_AES_KEY  - hex-encoded 32-byte AES key (GitHub secret)
    FIRMWARE_SHA256   - optional SHA-256 hash (set by workflow; if not set, script computes it)
"""

import os
import sys
import json
import socket
import hashlib
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
import paho.mqtt.client as mqtt

# ─── Fixed constants (not configurable) ─────────────────────────────────────
CONFIG_FILE = "config.json"        # always in repo root
AES_GCM_IV_LENGTH = 12
NETWORK_TIMEOUT_SECONDS = 10
# ─────────────────────────────────────────────────────────────────────────────

def load_config():
    """Load all parameters from config.json."""
    if not os.path.exists(CONFIG_FILE):
        print(f"ERROR: {CONFIG_FILE} not found in current directory")
        sys.exit(1)

    with open(CONFIG_FILE, "r") as f:
        config = json.load(f)

    required_keys = [
        "version",
        "firmware_bin",
        "mqtt_broker",
        "mqtt_port_tls",
        "mqtt_port_tcp",
        "mqtt_topic",
        "device_family"
    ]
    for key in required_keys:
        if key not in config:
            print(f"ERROR: Missing required key '{key}' in {CONFIG_FILE}")
            sys.exit(1)

    # Optional validation for device_family being a list
    if not isinstance(config["device_family"], list):
        print("ERROR: 'device_family' must be a list")
        sys.exit(1)

    return config

def load_aes_key():
    """Load AES-256 key from environment variable."""
    key_hex = os.environ.get("FIRMWARE_AES_KEY")
    if not key_hex:
        print("ERROR: FIRMWARE_AES_KEY environment variable not set")
        sys.exit(1)

    try:
        key = bytes.fromhex(key_hex)
    except ValueError:
        print("ERROR: FIRMWARE_AES_KEY must be a valid hex string")
        sys.exit(1)

    if len(key) != 32:
        print(f"ERROR: AES key must be 32 bytes, got {len(key)}")
        sys.exit(1)

    return key

def load_firmware_sha256(firmware_bin):
    """Get SHA-256 from env var or compute from binary file."""
    sha256 = os.environ.get("FIRMWARE_SHA256")
    if sha256:
        return sha256

    if not os.path.exists(firmware_bin):
        print(f"ERROR: Firmware binary '{firmware_bin}' not found")
        sys.exit(1)

    with open(firmware_bin, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()

def encrypt_payload(aes_key, plaintext):
    """Encrypt JSON payload with AES-256-GCM. Returns iv + tag + ciphertext."""
    iv = os.urandom(AES_GCM_IV_LENGTH)
    aesgcm = AESGCM(aes_key)
    ciphertext_with_tag = aesgcm.encrypt(iv, plaintext, None)
    return iv + ciphertext_with_tag

def publish_mqtt(payload_bytes, config):
    """Publish message to MQTT broker with automatic socket timeout and port fallback."""
    socket.setdefaulttimeout(NETWORK_TIMEOUT_SECONDS)

    # Attempt TLS port first
    print(f"Connecting to {config['mqtt_broker']}:{config['mqtt_port_tls']} (TLS)...")
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        client.tls_set()
        client.connect(config["mqtt_broker"], config["mqtt_port_tls"], keepalive=10)
        result = client.publish(config["mqtt_topic"], payload_bytes, qos=1)
        result.wait_for_publish(timeout=NETWORK_TIMEOUT_SECONDS)
        print("MQTT message published successfully over TLS")
        client.disconnect()
        return
    except Exception as e:
        print(f"WARNING: TLS connection failed/timed out: {e}")
        print("Falling back to TCP...")

    # Fallback to TCP
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        client.connect(config["mqtt_broker"], config["mqtt_port_tcp"], keepalive=10)
        result = client.publish(config["mqtt_topic"], payload_bytes, qos=1)
        result.wait_for_publish(timeout=NETWORK_TIMEOUT_SECONDS)
        print("MQTT message published successfully over TCP")
        client.disconnect()
    except Exception as e:
        print(f"ERROR: Failed to publish MQTT message: {e}")
        sys.exit(1)

def main():
    print("=== OTA MQTT Notify ===")

    # Load everything from config.json
    config = load_config()
    aes_key = load_aes_key()
    sha256 = load_firmware_sha256(config["firmware_bin"])

    print(f"Target Topic : {config['mqtt_topic']}")
    print(f"Firmware Ver : {config['version']}")
    print(f"Firmware Bin : {config['firmware_bin']}")
    print(f"SHA-256 Hash : {sha256}")
    print(f"Device Family: {config['device_family']}")

    # Build plaintext JSON
    payload = json.dumps({
        "event": "OTA_AVAILABLE",
        "version": config["version"],
        "sha256": sha256,
        "family": config["device_family"],
        "firmware_bin": config["firmware_bin"]
    })

    # Encrypt payload
    encrypted_payload = encrypt_payload(aes_key, payload.encode())
    print(f"Encrypted Payload Size: {len(encrypted_payload)} bytes")

    # Publish over network
    publish_mqtt(encrypted_payload, config)
    print("=== Done ===")

if __name__ == "__main__":
    main()