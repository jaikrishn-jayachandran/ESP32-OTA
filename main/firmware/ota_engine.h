#ifndef OTA_ENGINE_H
#define OTA_ENGINE_H

#include "esp_err.h"
#include "nvs_manager.h"

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_RUNNING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED
} ota_state_t;

esp_err_t firmware_ota_init(void);
void firmware_ota_check_and_update_task(void *pvParameters);
void firmware_ota_verify_or_rollback(void);
esp_err_t firmware_ota_acquire(void);
void firmware_ota_release(void);
ota_state_t firmware_ota_get_state(void);
void firmware_ota_set_expected_sha256(const char *hex_hash);

#endif // OTA_ENGINE_H