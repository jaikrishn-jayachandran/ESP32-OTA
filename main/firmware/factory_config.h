// ========== Factory Configuration Header File ==========

// This header file containts the factory configuration for the ESP32 firmware update on factory flash

// ALL THE VARIABLES IN THIS SHOULD BE A SECRETE AND SHOULD NOT BE EXPOSED TO THE PUBLIC. THIS IS FOR YOUR REFERENCE ONLY. PLEASE CHANGE THE VALUES BEFORE USING IN PRODUCTION.

#ifndef FACTORY_CONFIG_H
#define FACTORY_CONFIG_H

// Factory Default Configuration
#define FACTORY_DEVICE_ID          "ESP32_DEVICE_001"
#define FACTORY_DEVICE_FAMILY      "esp32-gen1"
#define FACTORY_FIRMWARE_VERSION   "1.0.0"
#define FACTORY_WIFI_SSID          "Tinker Space"
#define FACTORY_WIFI_PASSWORD      "123tinkerspace"
#define FACTORY_GITHUB_REPO        "jaikrishn-jayachandran/ESP32-OTA"
#define FACTORY_GITHUB_TOKEN       ""
#define FACTORY_MQTT_HOST          "broker.hivemq.com"
#define FACTORY_MQTT_PORT          8883
#define FACTORY_MQTT_TOPIC         "/esp32-ota/trigger/jaikrishn-jayachandran"
#define FACTORY_MQTT_USERNAME      ""
#define FACTORY_MQTT_PASSWORD      ""

// Factory AES-256 Key (32 bytes) - Shared with CI notify script
// Generate: python3 -c "import secrets; print(secrets.token_hex(32))"
static const uint8_t FACTORY_AES_KEY[32] = {
    0xBA, 0x18, 0x43, 0x09, 0xC4, 0x0C, 0x9C, 0xC7,
    0xD8, 0x36, 0xF7, 0x85, 0xDA, 0x8C, 0x4B, 0x18,
    0x27, 0x47, 0xA3, 0x86, 0xB8, 0x5D, 0x42, 0x48,
    0x25, 0x6A, 0x1A, 0x40, 0x59, 0xB0, 0xC6, 0xA5
};

#endif // FACTORY_CONFIG_H