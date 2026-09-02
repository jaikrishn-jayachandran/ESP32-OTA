# ESP-32-OTA-Firmware-Update

MQTT-triggered Over-The-Air (OTA) firmware update system for ESP32 using GitHub as the firmware distribution backend and HiveMQ as the free public MQTT broker.

---

## Table of Contents

- [Architecture](#architecture)
- [Setup (Industry Worker)](#setup-industry-worker)
  - [1.1 Flashing with Factory Default Values](#11-flashing-with-factory-default-values)
  - [1.2 GitHub Repository Setup](#12-github-repository-setup)
  - [1.3 SHA-256 Key and AES Security Setup](#13-sha-256-key-and-aes-security-setup)
  - [1.4 Clearing All Flash Memory with ESP-IDF](#14-clearing-all-flash-memory-with-esp-idf)
  - [1.5 Flashing the Code](#15-flashing-the-code)
- [Performing Firmware Updates (New Version Commits)](#performing-firmware-updates-new-version-commits)
  - [Step 1: Update config.json](#step-1-update-configjson)
  - [Step 2: Build the Project](#step-2-build-the-project)
  - [Step 3: Copy Binary to Project Root](#step-3-copy-binary-to-project-root)
  - [Step 4: Commit and Push](#step-4-commit-and-push)
  - [What Happens Automatically](#what-happens-automatically)
- [Security Model](#security-model)
- [Components](#components)
- [MQTT Protocol](#mqtt-protocol)
- [NVS Namespace Layout](#nvs-namespace-layout)
- [Partition Table](#partition-table)
- [Troubleshooting](#troubleshooting)

---

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

---

## Setup (Industry Worker)

### Prerequisites

Before starting, install these tools:

| Tool | Version | Install Command |
|------|---------|-----------------|
| Python | 3.11+ | `sudo apt install python3 python3-pip` |
| Git | Latest | `sudo apt install git` |
| ESP-IDF | v6.0+ | See [ESP-IDF Install Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) |

Install ESP-IDF (Ubuntu/Linux):

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32
source ~/esp/esp-idf/export.sh
```

Verify installation:

```bash
idf.py --version
# Should show: ESP-IDF v6.x.x
```

---

### 1.1 Flashing with Factory Default Values

Factory defaults are defined in `main/firmware/factory_config.h`. These values are compiled into the firmware and written to NVS (Non-Volatile Storage) on first boot.

#### Current Factory Defaults

```c
#define FACTORY_DEVICE_ID          "ESP32_DEVICE_001"
#define FACTORY_DEVICE_FAMILY      "esp32-gen1"
#define FACTORY_FIRMWARE_VERSION   "1.0.0"
#define FACTORY_WIFI_SSID          "JK"
#define FACTORY_WIFI_PASSWORD      ""
#define FACTORY_GITHUB_REPO        "jaikrishn-jayachandran/ESP32-OTA"
#define FACTORY_MQTT_HOST          "broker.hivemq.com"
#define FACTORY_MQTT_PORT          8883
#define FACTORY_MQTT_TOPIC         "/esp32-ota/trigger/jaikrishn-jayachandran"
```

#### How Factory Defaults Work

1. On first boot (or after NVS erase), the firmware reads from NVS
2. If NVS is empty, it writes the factory defaults from `factory_config.h`
3. On subsequent boots, it reads the stored values from NVS
4. If `FACTORY_FIRMWARE_VERSION` changes, NVS is automatically erased and reset to new defaults

#### To Flash with Factory Defaults

```bash
# Set target (only needed once)
idf.py set-target esp32

# Build the project
idf.py build

# Erase entire flash (clears NVS) and flash
idf.py erase-flash flash monitor
```

After this, the ESP32 boots with factory defaults, connects to your WiFi, and starts the MQTT listener.

---

### 1.2 GitHub Repository Setup

#### Step 1: Create or Fork the Repository

Option A - Fork this repository:
```bash
# Fork on GitHub, then clone
git clone https://github.com/YOUR_USERNAME/ESP32-OTA.git
cd ESP32-OTA
```

Option B - Create new repository:
```bash
git init
git remote add origin https://github.com/YOUR_USERNAME/YOUR_REPO.git
```

#### Step 2: Configure Git

```bash
git config user.name "Your Name"
git config user.email "your.email@example.com"
```

#### Step 3: Update Factory Config

Edit `main/firmware/factory_config.h` and update:

```c
#define FACTORY_GITHUB_REPO        "YOUR_USERNAME/YOUR_REPO"
#define FACTORY_WIFI_SSID          "YOUR_WIFI_SSID"
#define FACTORY_WIFI_PASSWORD      "YOUR_WIFI_PASSWORD"
#define FACTORY_MQTT_TOPIC         "/esp32-ota/trigger/YOUR_USERNAME"
```

#### Step 4: Update Python Script Config

Edit `scripts/notify_ota_update.py` and update the hardcoded values to match:

```python
MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 8883
MQTT_TOPIC = "/esp32-ota/trigger/YOUR_USERNAME"  # must match factory_config.h
DEVICE_FAMILY = ["esp32-gen1", "all"]
```

#### Step 5: Initial Commit and Push

```bash
git add -A
git commit -m "Initial setup"
git branch -M main
git push -u origin main
```

---

### 1.3 SHA-256 Key and AES Security Setup

This system uses AES-256-GCM encryption to secure MQTT OTA trigger messages. The same 32-byte key must exist in both the ESP32 firmware and the GitHub Actions CI pipeline.

#### Step 1: Generate AES-256 Key

```bash
python3 -c "import secrets; print(secrets.token_hex(32))"
```

Example output:
```
ba184309c40c9cc7d836f785da8c4b182747a386b85d4248256a1a4059b0c6a5
```

#### Step 2: Update factory_config.h

Convert the hex string to C byte array format. You can use this Python helper:

```bash
python3 -c "
key = bytes.fromhex('YOUR_HEX_KEY_HERE')
print('static const uint8_t FACTORY_AES_KEY[32] = {')
for i in range(0, 32, 8):
    chunk = ', '.join(f'0x{b:02X}' for b in key[i:i+8])
    comma = ',' if i + 8 < 32 else ''
    print(f'    {chunk}{comma}')
print('};')
"
```

Replace the `FACTORY_AES_KEY` in `main/firmware/factory_config.h` with the output.

#### Step 3: Add GitHub Repository Secret

1. Go to your GitHub repo: **Settings > Secrets and variables > Actions**
2. Click **New repository secret**
3. Name: `FIRMWARE_AES_KEY`
4. Value: paste the hex string from Step 1 (e.g., `ba184309c40c9cc7...`)
5. Click **Add secret**

> **Important:** Never commit the AES key to source code if the repo is public. The key should only exist as a GitHub secret and in the compiled firmware binary.

---

### 1.4 Clearing All Flash Memory with ESP-IDF

Clearing the flash is required when:
- First-time setup
- Changing WiFi credentials
- Changing MQTT broker or topic
- Changing the AES key
- NVS has become corrupted

#### Command

```bash
idf.py erase-flash
```

This erases the **entire 4MB flash**, including:

| Partition | Offset | What's Erased |
|-----------|--------|---------------|
| nvs | 0x9000 | WiFi credentials, MQTT config, AES key, firmware version |
| otadata | 0xF000 | OTA boot selection data |
| phy_init | 0x11000 | PHY calibration data |
| ota_0 | 0x20000 | Firmware partition 0 |
| ota_1 | 0x200000 | Firmware partition 1 |

After erase, the ESP32 has no firmware and no configuration. You must reflash.

#### Erase + Flash (One Command)

```bash
idf.py erase-flash flash monitor
```

This erases everything, flashes the new firmware, and opens the serial monitor.

---

### 1.5 Flashing the Code

#### First-Time Flash

```bash
# 1. Set target (only once per project)
idf.py set-target esp32

# 2. Build the firmware
idf.py build

# 3. Erase flash and write firmware
idf.py erase-flash flash monitor
```

#### Subsequent Flashes (Without Erasing NVS)

If you only want to update the firmware without clearing WiFi/MQTT config:

```bash
idf.py build flash monitor
```

> **Note:** If you changed `FACTORY_FIRMWARE_VERSION` in `factory_config.h`, NVS auto-resets on boot anyway.

#### Flash Using USB/Serial

ESP-IDF automatically detects the serial port. If you need to specify it:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

On Windows, the port is typically `COM3` or `COM4`.

#### Monitor Serial Output

```bash
idf.py monitor
```

Press `Ctrl+]` to exit the monitor.

---

## Performing Firmware Updates (New Version Commits)

Once the initial setup is complete and ESP32s are deployed, follow this workflow for every firmware update.

### Step 1: Update config.json

Edit `config.json` in the project root and bump the version number:

```json
{
    "version": "1.2.0",
    "firmware_name": "Blink Application",
    "description": "Your update description here",
    "author": "Your Name"
}
```

> **Critical:** The version in `config.json` must be different from the version currently running on the ESP32s. The ESP32 compares this value against its stored version to decide if an update is needed.

### Step 2: Build the Project

```bash
idf.py build
```

This compiles the firmware and produces `build/ESP-32-OTA-Firmware-Update.bin`.

### Step 3: Copy Binary to Project Root

```bash
cp build/ESP-32-OTA-Firmware-Update.bin .
```

The binary must be in the project root (not in `build/`) because the ESP32 downloads it from:
```
https://raw.githubusercontent.com/YOUR_USERNAME/YOUR_REPO/main/ESP-32-OTA-Firmware-Update.bin
```

### Step 4: Commit and Push

```bash
git add -A
git commit -m "v1.2.0 - Your update description"
git push origin main
```

### What Happens Automatically

Once you push to `main`:

```
1. GitHub Actions workflow triggers (on push to main)
   |
2. Python script runs (scripts/notify_ota_update.py):
   a. SHA-256 verifies ESP-32-OTA-Firmware-Update.bin
   b. Reads version from config.json
   c. Encrypts JSON payload with AES-256-GCM:
      {
        "event": "OTA_AVAILABLE",
        "version": "1.2.0",
        "sha256": "f7f789e8...",
        "family": ["esp32-gen1", "all"]
      }
   d. Publishes encrypted payload to broker.hivemq.com:8883
   |
3. All deployed ESP32s receive the MQTT message:
   a. Decrypt payload using shared AES key
   b. Validate event type and device family
   c. Fetch config.json from GitHub raw URL
   d. Compare remote version vs local version
   e. If different -> download new .bin from GitHub raw
   f. SHA-256 verify downloaded binary matches MQTT hash
   g. If match -> reboot into new firmware
   h. If mismatch -> rollback to previous firmware
```

### Summary Checklist

| Step | Command/Action | Done? |
|------|----------------|-------|
| 1 | Edit `config.json` version | [ ] |
| 2 | `idf.py build` | [ ] |
| 3 | `cp build/ESP-32-OTA-Firmware-Update.bin .` | [ ] |
| 4 | `git add -A && git commit -m "vX.Y.Z" && git push origin main` | [ ] |
| 5 | Verify GitHub Actions runs successfully | [ ] |

---

## Security Model

| Layer | Mechanism | Purpose |
|-------|-----------|---------|
| MQTT Payload | AES-256-GCM encryption | Prevents unauthorized OTA triggers |
| Key Distribution | GitHub Secret + Factory Config | AES key shared between CI and firmware |
| Transport | TLS (MQTTS on port 8883) | Encrypts MQTT broker connection |
| Firmware Integrity | SHA-256 verification | Ensures downloaded binary matches expected hash |
| Safe Update | Dual OTA partitions | Enables rollback if update fails |
| Auto-Rollback | Boot verification | Reverts if new firmware fails to boot |

### AES Key Security

The AES key is the root of trust. It must be identical in:
- `main/firmware/factory_config.h` (compiled into ESP32)
- GitHub Secret `FIRMWARE_AES_KEY` (used by CI to encrypt MQTT messages)

If the repo is public, the AES key in `factory_config.h` is visible to anyone. For production use, consider provisioning the key at manufacturing time instead of committing it to source code.

---

## Components

| File | Description |
|------|-------------|
| `main/firmware/factory_config.h` | Compile-time defaults (WiFi, MQTT, AES key, GitHub repo) |
| `main/firmware/ota_engine.c` | OTA update logic (version check, HTTPS download, SHA-256 verification) |
| `main/firmware/ota_engine.h` | OTA engine public interface |
| `main/firmware/mqtt_listener.c` | MQTT subscriber (AES-256-GCM decrypt, OTA trigger) |
| `main/firmware/mqtt_listener.h` | MQTT listener public interface |
| `main/firmware/nvs_manager.c` | NVS persistence (config storage, version staging) |
| `main/firmware/nvs_manager.h` | NVS manager public interface and `sys_config_t` struct |
| `main/firmware/wifi_manager.c` | WiFi connection management |
| `main/firmware/status_led.c` | LED status indicator |
| `main/main.c` | Application entry point and boot sequence |
| `scripts/notify_ota_update.py` | CI script (SHA-256 verify, AES encrypt, MQTT publish) |
| `.github/workflows/firmware-release.yml` | GitHub Actions workflow |
| `config.json` | Version manifest (fetched by ESP32 to check for updates) |
| `partitions.csv` | ESP32 partition table |

---

## MQTT Protocol

### Broker

| Setting | Value |
|---------|-------|
| Host | `broker.hivemq.com` |
| Port | `8883` (TLS) |
| Authentication | None (free public broker) |

### Topic Format

```
/esp32-ota/trigger/{github_username}
```

Example:
```
/esp32-ota/trigger/jaikrishn-jayachandran
```

### Encrypted Payload Format

Wire format: `[12-byte IV][16-byte GCM tag][encrypted JSON]`

### Decrypted JSON Structure

```json
{
    "event": "OTA_AVAILABLE",
    "version": "1.2.0",
    "sha256": "f7f789e825a62e6793b19150bb6ea3e444fa4ac08c995c0d0f3b547a21add39c",
    "family": ["esp32-gen1", "all"]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `event` | string | Must be `"OTA_AVAILABLE"` to trigger update |
| `version` | string | Target firmware version |
| `sha256` | string | SHA-256 hash of the firmware binary (64 hex chars) |
| `family` | array | Device family filter. `"all"` matches all devices |

---

## NVS Namespace Layout

### `sys_cfg` Namespace

| Key | Type | Description |
|-----|------|-------------|
| `dev_id` | string | Device identifier (e.g., `ESP32_DEVICE_001`) |
| `dev_fam` | string | Device family (e.g., `esp32-gen1`) |

### `app_cfg` Namespace

| Key | Type | Description |
|-----|------|-------------|
| `ver` | string | Current firmware version |
| `ssid` | string | WiFi SSID |
| `pass` | string | WiFi password |
| `gh_repo` | string | GitHub repository (user/repo format) |
| `aes_key` | blob | AES-256 key (32 bytes) |
| `mqtt_host` | string | MQTT broker hostname |
| `mqtt_port` | uint16 | MQTT broker port |
| `mqtt_topic` | string | MQTT subscription topic |
| `cfg_url` | string | Config URL (auto-generated from github_repo) |

---

## Partition Table

| Name | Type | Offset | Size | Description |
|------|------|--------|------|-------------|
| nvs | data | 0x9000 | 24 KB | Non-volatile storage (config, keys) |
| otadata | data | 0xF000 | 8 KB | OTA boot partition selection |
| phy_init | data | 0x11000 | 4 KB | WiFi PHY calibration data |
| ota_0 | app | 0x20000 | 1.875 MB | Firmware partition A |
| ota_1 | app | 0x200000 | 1.875 MB | Firmware partition B |

Total flash used: ~3.9 MB on 4 MB flash.

---

## Troubleshooting

### WiFi fails to connect

- Verify `FACTORY_WIFI_SSID` and `FACTORY_WIFI_PASSWORD` in `factory_config.h`
- Run `idf.py erase-flash flash` to reset NVS with new credentials
- Check serial monitor for connection errors

### MQTT client fails to instantiate

- Ensure `FACTORY_MQTT_HOST` is `broker.hivemq.com` (not a placeholder)
- Check that TLS cert bundle is enabled in `sdkconfig.defaults`
- The broker requires TLS on port 8883

### OTA does not trigger

- Verify GitHub Actions workflow completed successfully (check Actions tab)
- Verify `FIRMWARE_AES_KEY` secret matches `FACTORY_AES_KEY` in firmware
- Check that ESP32 is connected to WiFi and MQTT
- Use `idf.py monitor` to view serial output

### OTA downloads but fails SHA-256 verification

- Ensure `ESP-32-OTA-Firmware-Update.bin` in repo root is the same as what was built
- Re-run `cp build/ESP-32-OTA-Firmware-Update.bin .` before committing
- Check that `config.json` version was bumped

### Version mismatch - no update triggered

- Verify `config.json` on GitHub has a different version than the ESP32's current version
- GitHub CDN may cache raw content for 5-15 minutes after push
- Test by opening the raw URL in a browser: `https://raw.githubusercontent.com/YOUR_REPO/main/config.json`

### NVS not resetting on firmware version change

- Ensure `FACTORY_FIRMWARE_VERSION` in `factory_config.h` was changed
- The NVS auto-reset only triggers when the compiled version differs from the stored version
- Manual reset: `idf.py erase-flash flash`

---

## Requirements

| Requirement | Version |
|-------------|---------|
| ESP-IDF | v6.0.0 or later |
| Python | 3.11+ |
| ESP32 Board | Any ESP32 with 4MB flash |
| GitHub Account | For repository and Actions |
| Python packages (CI only) | `paho-mqtt`, `cryptography` (auto-installed in CI) |
