# ESP-32-OTA-Firmware-Update

MQTT-triggered OTA firmware update system for ESP32 using GitHub as the firmware distribution backend.

## Architecture

```
Developer pushes to main
        |
GitHub Actions triggers
        |
Python script (scripts/notify_ota_update.py):
  1. SHA-256 verifies ESP-32-OTA-Firmware-Update.bin
  2. Encrypts JSON payload (AES-256-GCM)
  3. Publishes to broker.hivemq.com:8883
        |
ESP32 receives MQTT message
  1. Decrypts payload (AES-256-GCM)
  2. Fetches config.json from GitHub raw
  3. Compares remote version != current_version
  4. If different -> downloads .bin from GitHub raw
  5. SHA-256 verifies downloaded binary against MQTT hash
  6. If mismatch -> rollback. If match -> reboot
```

## Components

| Component | Description |
|-----------|-------------|
| `main/firmware/factory_config.h` | Compile-time defaults (WiFi, MQTT, AES key, GitHub repo) |
| `main/firmware/ota_engine.c` | OTA update logic (version check, HTTPS download) |
| `main/firmware/mqtt_listener.c` | MQTT subscriber (AES-256-GCM decrypt, OTA trigger) |
| `main/firmware/nvs_manager.c` | NVS persistence (config storage, version staging) |
| `scripts/notify_ota_update.py` | CI script (SHA-256 verify, AES encrypt, MQTT publish) |
| `.github/workflows/firmware-release.yml` | GitHub Actions workflow |
| `config.json` | Version manifest (fetched by ESP32) |

## Security

- **AES-256-GCM** encrypted MQTT payloads prevent unauthorized OTA triggers
- The 32-byte AES key is shared between `factory_config.h` (firmware) and the `FIRMWARE_AES_KEY` GitHub secret (CI)
- Even if the repository is public, random people cannot trigger OTA updates without the AES key
- **SHA-256 verification** on-device: ESP32 computes SHA-256 of the downloaded binary and compares with the hash from the MQTT message. Mismatch triggers automatic rollback.
- TLS transport to HiveMQ broker adds transport-layer encryption

## First-Time Setup

### 1. Generate AES-256 key

```bash
python3 -c "import secrets; print(secrets.token_hex(32))"
```

### 2. Update `main/firmware/factory_config.h`

Set the generated key in `FACTORY_AES_KEY`:

```c
static const uint8_t FACTORY_AES_KEY[32] = {
    0xXX, 0xXX, 0xXX, ... // your 32 random bytes
};
```

Also update:
- `FACTORY_WIFI_SSID` / `FACTORY_WIFI_PASSWORD`
- `FACTORY_MQTT_TOPIC` (must match `scripts/notify_ota_update.py`)
- `FACTORY_GITHUB_REPO`

### 3. Add GitHub Secret

Go to your repo: **Settings -> Secrets and variables -> Actions -> New repository secret**

- Name: `FIRMWARE_AES_KEY`
- Value: the hex string from step 1 (e.g., `ba184309c40c9cc7...`)

### 4. Update Python script config

Edit `scripts/notify_ota_update.py` and update the hardcoded values to match `factory_config.h`:

```python
MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 8883
MQTT_TOPIC = "/esp32-ota/trigger/jaikrishn-jayachandran"  # must match
DEVICE_FAMILY = ["esp32-gen1", "all"]
```

### 5. Flash with NVS erase

```bash
idf.py set-target esp32
idf.py build
idf.py erase-flash flash monitor
```

## Developer Workflow

### Making a firmware update

1. Make code changes in `main/`
2. Build: `idf.py build`
3. Copy binary to repo root:
   ```bash
   cp build/ESP-32-OTA-Firmware-Update.bin .
   ```
4. Update `config.json` version:
   ```json
   {"version": "1.3.0", ...}
   ```
5. Commit and push:
   ```bash
   git add -A
   git commit -m "v1.3.0"
   git push origin main
   ```
6. GitHub Actions triggers -> sends MQTT notification
7. All deployed ESP32s receive the trigger -> check `config.json` -> download new binary -> OTA update

### Changing MQTT topic or broker

Update both:
1. `main/firmware/factory_config.h` - `FACTORY_MQTT_TOPIC`
2. `scripts/notify_ota_update.py` - `MQTT_TOPIC`

Then flash with `idf.py erase-flash flash` to reset NVS.

### Changing the AES key

1. Generate new key: `python3 -c "import secrets; print(secrets.token_hex(32))"`
2. Update `FACTORY_AES_KEY` in `factory_config.h`
3. Update `FIRMWARE_AES_KEY` GitHub secret
4. Flash with `idf.py erase-flash flash`

## MQTT Protocol

### Topic

```
/esp32-ota/trigger/{github_username}
```

### Payload (encrypted with AES-256-GCM)

Wire format: `[12-byte IV][16-byte GCM tag][encrypted JSON]`

Decrypted JSON:

```json
{
    "event": "OTA_AVAILABLE",
    "version": "1.3.0",
    "sha256": "a1b2c3d4...",
    "family": ["esp32-gen1", "all"]
}
```

| Field | Description |
|-------|-------------|
| `event` | Must be `"OTA_AVAILABLE"` to trigger update |
| `version` | Target firmware version (compared against `config.json`) |
| `sha256` | SHA-256 hash of the firmware binary |
| `family` | Device family filter. `"all"` matches all devices |

## NVS Namespace Layout

### `sys_cfg`

| Key | Type | Description |
|-----|------|-------------|
| `dev_id` | string | Device identifier |
| `dev_fam` | string | Device family |

### `app_cfg`

| Key | Type | Description |
|-----|------|-------------|
| `ver` | string | Current firmware version |
| `ssid` | string | WiFi SSID |
| `pass` | string | WiFi password |
| `gh_repo` | string | GitHub repo (user/repo) |
| `aes_key` | blob | AES-256 key (32 bytes) |
| `mqtt_host` | string | MQTT broker host |
| `mqtt_port` | uint16 | MQTT broker port |
| `mqtt_topic` | string | MQTT subscription topic |

## Partition Table

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data | 0x9000 | 24 KB |
| otadata | data | 0xF000 | 8 KB |
| phy_init | data | 0x11000 | 4 KB |
| ota_0 | app | 0x20000 | 1.875 MB |
| ota_1 | app | 0x200000 | 1.875 MB |

## Requirements

- ESP-IDF >= 6.0.0
- Python 3.11+ (for CI script)
- `paho-mqtt` and `cryptography` Python packages (installed in CI)
