#include "esp_log.h"
#include "esp_sntp.h"
#include "firmware/nvs_manager.h"
#include "firmware/wifi_manager.h"
#include "firmware/mqtt_listener.h"
#include "firmware/ota_engine.h"
#include "firmware/status_led.h"
#include "user_space/user_space.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN_CORE";

static bool sntp_initialized = false;

static void initialize_sntp(void)
{
    if (sntp_initialized) {
        ESP_LOGW(TAG, "SNTP already initialized – skipping");
        return;
    }

    // Check if SNTP is already running (e.g., started by Wi-Fi manager)
    if (esp_sntp_enabled()) {
        ESP_LOGW(TAG, "SNTP already running – using existing service");
        sntp_initialized = true;   // prevent re-init
        return;
    }

    sntp_initialized = true;

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
}

static void obtain_time(void) {
    ESP_LOGI(TAG, "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Wait for system time to update
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;

    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to sync... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "Current time: %s", asctime(&timeinfo));
}

static bool wait_for_time_sync(uint32_t timeout_ms)
{
    uint32_t start = xTaskGetTickCount();
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
    }
    return true;
}

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
    ESP_LOGI(TAG, "Connecting to Wi-Fi network: %s", sys_cfg.wifi_ssid);
    esp_err_t wifi_status = wifi_manager_init(&sys_cfg);

    if (wifi_status != ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi connection failed. Entering Factory Default LED blink loop...");
    }
    obtain_time();

    initialize_sntp();
    if (!wait_for_time_sync(10000)) {
        ESP_LOGW(TAG, "Time synchronization failed – HTTPS may not work");
    } else {
        ESP_LOGI(TAG, "System time synchronized");
    }

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