#include "esp_log.h"
#include "firmware/nvs_manager.h"
#include "firmware/wifi_manager.h"
#include "firmware/mqtt_listener.h"
#include "firmware/ota_engine.h"
#include "firmware/status_led.h"
#include "user_space/user_space.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN_CORE";

void app_main(void) {
    ESP_LOGI(TAG, "=== Initializing System Core ===");

    // 1. Initialize Storage & Read Active NVS Parameters
    sys_config_t sys_cfg;
    ESP_ERROR_CHECK(firmware_nvs_init(&sys_cfg));

    // 2. Initialize OTA mutex (before any OTA task can run)
    ESP_ERROR_CHECK(firmware_ota_init());

    // 3. Check and Validate OTA Partition Health
    firmware_ota_verify_or_rollback();

    // 4. Connect to Wi-Fi using NVS Credentials
    ESP_LOGI(TAG, "Connecting to Wi-Fi network: %s", sys_cfg.wifi_ssid);
    esp_err_t wifi_status = wifi_manager_init(&sys_cfg);

    if (wifi_status != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connection failed. Entering Factory Default LED blink loop...");
        status_led_factory_blink_blocking();
        abort();
    }

    // 5. Execute User Space NVS Migration Hook
    user_space_nvs_update_hook();

    // 6. Startup OTA Check (Pull Event) - Execute only once
    ESP_LOGI(TAG, "Performing startup OTA check...");
    xTaskCreate(&firmware_ota_check_and_update_task, "startup_ota_check",
                8192, NULL, 5, NULL);

    // Give startup OTA a brief window, then proceed
    vTaskDelay(pdMS_TO_TICKS(5000));

    // 7. Spawn Background MQTT OTA Listener Task
    ESP_LOGI(TAG, "Starting background MQTT listener service...");
    esp_err_t mqtt_status = firmware_mqtt_init(&sys_cfg);

    if (mqtt_status != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MQTT: %s",
                 esp_err_to_name(mqtt_status));
    }

    // 8. Hand Control Over to User Application Space
    ESP_LOGI(TAG, "Launching User Application Space...");
    xTaskCreate(&user_space_main, "user_space_task", 4096, NULL, 5, NULL);
}