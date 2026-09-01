#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    char device_id[32];
    char device_family[32];
    char current_version[16];
    char wifi_ssid[32];
    char wifi_password[64];
    char github_repo[100];
    char github_token[64];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_topic[64];
    char mqtt_username[64];
    char mqtt_password[64];
    char config_url[256];
    uint8_t aes_key[32];
} sys_config_t;

esp_err_t firmware_nvs_init(sys_config_t *out_cfg);
esp_err_t firmware_nvs_stage_str(const char *key, const char *value);
esp_err_t firmware_nvs_stage_u16(const char *key, uint16_t value);
esp_err_t firmware_nvs_commit_staged(void);
void firmware_nvs_discard_stage(void);
esp_err_t firmware_nvs_get_config(sys_config_t *cfg);
esp_err_t firmware_nvs_set_config(const sys_config_t *cfg);

#endif // NVS_MANAGER_H