#include "nvs_manager.h"
#include "factory_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "FW_NVS";
#define SYS_NS "sys_cfg"
#define APP_NS "app_cfg"

static esp_err_t load_or_init_defaults(sys_config_t *cfg) {
    nvs_handle_t sys_h, app_h;
    esp_err_t err = nvs_open(SYS_NS, NVS_READWRITE, &sys_h);
    if (err != ESP_OK) return err;
    
    err = nvs_open(APP_NS, NVS_READWRITE, &app_h);
    if (err != ESP_OK) {
        nvs_close(sys_h);
        return err;
    }

    size_t len;

    // Read system ID (or Write the defaults)
    len = sizeof(cfg->device_id);
    if (nvs_get_str(sys_h, "dev_id", cfg->device_id, &len) != ESP_OK) {
        snprintf(cfg->device_id, sizeof(cfg->device_id), "%s", FACTORY_DEVICE_ID);
        nvs_set_str(sys_h, "dev_id", cfg->device_id);
    }

    // Read Device family
    len = sizeof(cfg->device_family);
    if (nvs_get_str(sys_h, "dev_fam", cfg->device_family, &len) != ESP_OK) {
        snprintf(cfg->device_family, sizeof(cfg->device_family), "%s", FACTORY_DEVICE_FAMILY);
        nvs_set_str(sys_h, "dev_fam", cfg->device_family);
    }

    // Read active firmware version
    len = sizeof(cfg->current_version);
    if (nvs_get_str(app_h, "ver", cfg->current_version, &len) != ESP_OK) {
        snprintf(cfg->current_version, sizeof(cfg->current_version), "%s", FACTORY_FIRMWARE_VERSION);
        nvs_set_str(app_h, "ver", cfg->current_version);
    }

    // Read WiFi SSID
    len = sizeof(cfg->wifi_ssid);
    if (nvs_get_str(app_h, "ssid", cfg->wifi_ssid, &len) != ESP_OK) {
        snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", FACTORY_WIFI_SSID);
        nvs_set_str(app_h, "ssid", cfg->wifi_ssid);
    }

    // Read WiFi Password
    len = sizeof(cfg->wifi_password);
    if (nvs_get_str(app_h, "pass", cfg->wifi_password, &len) != ESP_OK) {
        snprintf(cfg->wifi_password, sizeof(cfg->wifi_password), "%s", FACTORY_WIFI_PASSWORD);
        nvs_set_str(app_h, "pass", cfg->wifi_password);    
    }

    // Read GitHub Repo
    len = sizeof(cfg->github_repo);
    if (nvs_get_str(app_h, "gh_repo", cfg->github_repo, &len) != ESP_OK) {
        snprintf(cfg->github_repo, sizeof(cfg->github_repo), "%s", FACTORY_GITHUB_REPO);
        nvs_set_str(app_h, "gh_repo", cfg->github_repo);
    }

    // Read GitHub Token
    len = sizeof(cfg->github_token);
    if (nvs_get_str(app_h, "gh_token", cfg->github_token, &len) != ESP_OK) {
        snprintf(cfg->github_token, sizeof(cfg->github_token), "%s", FACTORY_GITHUB_TOKEN);
        nvs_set_str(app_h, "gh_token", cfg->github_token);
    }

    // Read MQTT Host
    len = sizeof(cfg->mqtt_host);
    if (nvs_get_str(app_h, "mqtt_host", cfg->mqtt_host, &len) != ESP_OK) {
        snprintf(cfg->mqtt_host, sizeof(cfg->mqtt_host), "%s", FACTORY_MQTT_HOST);
        nvs_set_str(app_h, "mqtt_host", cfg->mqtt_host);
    }

    // Read MQTT Port
    if (nvs_get_u16(app_h, "mqtt_port", &cfg->mqtt_port) != ESP_OK) {
        cfg->mqtt_port = FACTORY_MQTT_PORT;
        nvs_set_u16(app_h, "mqtt_port", cfg->mqtt_port);
    }

    // Read MQTT Topic
    len = sizeof(cfg->mqtt_topic);
    if (nvs_get_str(app_h, "mqtt_topic", cfg->mqtt_topic, &len) != ESP_OK) {
        snprintf(cfg->mqtt_topic, sizeof(cfg->mqtt_topic), "%s", FACTORY_MQTT_TOPIC);
        nvs_set_str(app_h, "mqtt_topic", cfg->mqtt_topic);
    }

    // Read MQTT Username
    len = sizeof(cfg->mqtt_username);
    if (nvs_get_str(app_h, "mqtt_user", cfg->mqtt_username, &len) != ESP_OK) {
        snprintf(cfg->mqtt_username, sizeof(cfg->mqtt_username), "%s", FACTORY_MQTT_USERNAME);
        nvs_set_str(app_h, "mqtt_user", cfg->mqtt_username);
    }

    // Read MQTT Password
    len = sizeof(cfg->mqtt_password);
    if (nvs_get_str(app_h, "mqtt_pass", cfg->mqtt_password, &len) != ESP_OK) {
        snprintf(cfg->mqtt_password, sizeof(cfg->mqtt_password), "%s", FACTORY_MQTT_PASSWORD);
        nvs_set_str(app_h, "mqtt_pass", cfg->mqtt_password);
    }

    // Read Config URL
    len = sizeof(cfg->config_url);
    if (nvs_get_str(app_h, "cfg_url", cfg->config_url, &len) != ESP_OK) {
        snprintf(cfg->config_url, sizeof(cfg->config_url), 
                 "https://raw.githubusercontent.com/%s/main/config.json", 
                 cfg->github_repo);
        nvs_set_str(app_h, "cfg_url", cfg->config_url);
    }

    // Read AES Key (32 bytes for AES-256)
    len = sizeof(cfg->aes_key);
    if (nvs_get_blob(app_h, "aes_key", cfg->aes_key, &len) != ESP_OK) {
        // Use a default key - IN PRODUCTION, USE SECURE KEY PROVISIONING
        memcpy(cfg->aes_key, FACTORY_AES_KEY, sizeof(cfg->aes_key));
        nvs_set_blob(app_h, "aes_key", cfg->aes_key, sizeof(cfg->aes_key));
    }

    nvs_commit(sys_h);
    nvs_commit(app_h);
    nvs_close(sys_h);
    nvs_close(app_h);
    return ESP_OK;
}

esp_err_t firmware_nvs_init(sys_config_t *out_cfg) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS flash: %s", esp_err_to_name(err));
        return err;
    }

    // Check if stored version matches compiled version; reset NVS if changed
    nvs_handle_t app_h;
    char stored_ver[32] = {0};
    size_t len = sizeof(stored_ver);
    if (nvs_open(APP_NS, NVS_READONLY, &app_h) == ESP_OK) {
        nvs_get_str(app_h, "ver", stored_ver, &len);
        nvs_close(app_h);
    }

    if (strcmp(stored_ver, FACTORY_FIRMWARE_VERSION) != 0) {
        ESP_LOGW(TAG, "Firmware version changed ('%s' -> '%s'). Resetting NVS to factory defaults.",
                 stored_ver, FACTORY_FIRMWARE_VERSION);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    return load_or_init_defaults(out_cfg);
}

esp_err_t firmware_nvs_stage_str(const char *key, const char *value) {
    nvs_handle_t app_h;
    esp_err_t err = nvs_open(APP_NS, NVS_READWRITE, &app_h);
    if (err != ESP_OK) return err;

    char stage_key[32];
    snprintf(stage_key, sizeof(stage_key), "%s_tmp", key);

    err = nvs_set_str(app_h, stage_key, value);
    if (err == ESP_OK) nvs_commit(app_h);

    nvs_close(app_h);
    return err;
}

esp_err_t firmware_nvs_stage_u16(const char *key, uint16_t value) {
    nvs_handle_t app_h;
    esp_err_t err = nvs_open(APP_NS, NVS_READWRITE, &app_h);
    if (err != ESP_OK) return err;

    char stage_key[32];
    snprintf(stage_key, sizeof(stage_key), "%s_tmp", key);

    err = nvs_set_u16(app_h, stage_key, value);
    if (err == ESP_OK) nvs_commit(app_h);

    nvs_close(app_h);
    return err;
}

esp_err_t firmware_nvs_commit_staged(void) {
    nvs_handle_t app_h;
    esp_err_t err = nvs_open(APP_NS, NVS_READWRITE, &app_h);
    if (err != ESP_OK) return err;

    // Commit staged version
    char staged_ver[32] = {0};
    size_t len = sizeof(staged_ver);
    if (nvs_get_str(app_h, "ver_tmp", staged_ver, &len) == ESP_OK) {
        nvs_set_str(app_h, "ver", staged_ver);
        nvs_erase_key(app_h, "ver_tmp");
        ESP_LOGI(TAG, "Committed staged firmware version: %s", staged_ver);
    }

    nvs_commit(app_h);
    nvs_close(app_h);
    return ESP_OK;
}

void firmware_nvs_discard_stage(void) {
    nvs_handle_t app_h;
    if (nvs_open(APP_NS, NVS_READWRITE, &app_h) == ESP_OK) {
        nvs_erase_key(app_h, "ver_tmp");
        nvs_commit(app_h);
        nvs_close(app_h);
        ESP_LOGI(TAG, "Discarded staged firmware version");
    }
}

esp_err_t firmware_nvs_get_config(sys_config_t *cfg) {
    nvs_handle_t sys_h, app_h;
    esp_err_t err = nvs_open(SYS_NS, NVS_READONLY, &sys_h);
    if (err != ESP_OK) return err;
    
    err = nvs_open(APP_NS, NVS_READONLY, &app_h);
    if (err != ESP_OK) {
        nvs_close(sys_h);
        return err;
    }

    size_t len;

    // Read all fields
    len = sizeof(cfg->device_id);
    nvs_get_str(sys_h, "dev_id", cfg->device_id, &len);
    
    len = sizeof(cfg->device_family);
    nvs_get_str(sys_h, "dev_fam", cfg->device_family, &len);
    
    len = sizeof(cfg->current_version);
    nvs_get_str(app_h, "ver", cfg->current_version, &len);
    
    len = sizeof(cfg->wifi_ssid);
    nvs_get_str(app_h, "ssid", cfg->wifi_ssid, &len);
    
    len = sizeof(cfg->wifi_password);
    nvs_get_str(app_h, "pass", cfg->wifi_password, &len);
    
    len = sizeof(cfg->github_repo);
    nvs_get_str(app_h, "gh_repo", cfg->github_repo, &len);
    
    len = sizeof(cfg->github_token);
    nvs_get_str(app_h, "gh_token", cfg->github_token, &len);
    
    len = sizeof(cfg->mqtt_host);
    nvs_get_str(app_h, "mqtt_host", cfg->mqtt_host, &len);
    
    nvs_get_u16(app_h, "mqtt_port", &cfg->mqtt_port);
    
    len = sizeof(cfg->mqtt_topic);
    nvs_get_str(app_h, "mqtt_topic", cfg->mqtt_topic, &len);
    
    len = sizeof(cfg->mqtt_username);
    nvs_get_str(app_h, "mqtt_user", cfg->mqtt_username, &len);
    
    len = sizeof(cfg->mqtt_password);
    nvs_get_str(app_h, "mqtt_pass", cfg->mqtt_password, &len);
    
    len = sizeof(cfg->config_url);
    nvs_get_str(app_h, "cfg_url", cfg->config_url, &len);
    
    len = sizeof(cfg->aes_key);
    nvs_get_blob(app_h, "aes_key", cfg->aes_key, &len);

    nvs_close(sys_h);
    nvs_close(app_h);
    return ESP_OK;
}

esp_err_t firmware_nvs_set_config(const sys_config_t *cfg) {
    nvs_handle_t sys_h, app_h;
    esp_err_t err = nvs_open(SYS_NS, NVS_READWRITE, &sys_h);
    if (err != ESP_OK) return err;
    
    err = nvs_open(APP_NS, NVS_READWRITE, &app_h);
    if (err != ESP_OK) {
        nvs_close(sys_h);
        return err;
    }

    // Write all fields
    nvs_set_str(sys_h, "dev_id", cfg->device_id);
    nvs_set_str(sys_h, "dev_fam", cfg->device_family);
    nvs_set_str(app_h, "ver", cfg->current_version);
    nvs_set_str(app_h, "ssid", cfg->wifi_ssid);
    nvs_set_str(app_h, "pass", cfg->wifi_password);
    nvs_set_str(app_h, "gh_repo", cfg->github_repo);
    nvs_set_str(app_h, "gh_token", cfg->github_token);
    nvs_set_str(app_h, "mqtt_host", cfg->mqtt_host);
    nvs_set_u16(app_h, "mqtt_port", cfg->mqtt_port);
    nvs_set_str(app_h, "mqtt_topic", cfg->mqtt_topic);
    nvs_set_str(app_h, "mqtt_user", cfg->mqtt_username);
    nvs_set_str(app_h, "mqtt_pass", cfg->mqtt_password);
    nvs_set_str(app_h, "cfg_url", cfg->config_url);
    nvs_set_blob(app_h, "aes_key", cfg->aes_key, sizeof(cfg->aes_key));

    nvs_commit(sys_h);
    nvs_commit(app_h);
    nvs_close(sys_h);
    nvs_close(app_h);
    return ESP_OK;
}