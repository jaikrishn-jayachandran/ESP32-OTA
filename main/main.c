#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
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

static void config_update_check_task(void *param)
{
    ESP_LOGI(TAG, "Checking for configuration updates...");

    sys_config_t current_cfg;
    if (firmware_nvs_get_config(&current_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read current config");
        vTaskDelete(NULL);
        return;
    }

    // Build URL
    char config_url[256];
    snprintf(config_url, sizeof(config_url),
             "https://raw.githubusercontent.com/%s/main/config.json",
             current_cfg.github_repo);
    ESP_LOGI(TAG, "Fetching config from: %s", config_url);

    // HTTP request
    esp_http_client_config_t http_cfg = {
        .url = config_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32-Config-Checker");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "Failed to fetch headers");
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    char *response = malloc(content_length + 1);
    if (response == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed");
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int read_len = esp_http_client_read_response(client, response, content_length);
    response[read_len] = '\0';
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status != 200 || read_len <= 0) {
        ESP_LOGE(TAG, "Failed to fetch config.json (status %d)", status);
        free(response);
        vTaskDelete(NULL);
        return;
    }

    // Parse JSON
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse config.json");
        vTaskDelete(NULL);
        return;
    }

    cJSON *mqtt_broker = cJSON_GetObjectItem(root, "mqtt_broker");
    cJSON *mqtt_topic  = cJSON_GetObjectItem(root, "mqtt_topic");
    cJSON *family      = cJSON_GetObjectItem(root, "device_family");

    if (!cJSON_IsString(mqtt_broker) || !cJSON_IsString(mqtt_topic) || !cJSON_IsArray(family)) {
        ESP_LOGE(TAG, "config.json missing fields");
        cJSON_Delete(root);
        vTaskDelete(NULL);
        return;
    }

    bool changed = false;
    if (strcmp(mqtt_broker->valuestring, current_cfg.mqtt_host) != 0) changed = true;
    if (strcmp(mqtt_topic->valuestring, current_cfg.mqtt_topic) != 0) changed = true;
    cJSON *first_family = cJSON_GetArrayItem(family, 0);
    if (cJSON_IsString(first_family)) {
        if (strcmp(first_family->valuestring, current_cfg.device_family) != 0) changed = true;
    }

    if (!changed) {
        ESP_LOGI(TAG, "Config is up-to-date.");
        cJSON_Delete(root);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Config changes detected, updating NVS...");
    strncpy(current_cfg.mqtt_host, mqtt_broker->valuestring, sizeof(current_cfg.mqtt_host) - 1);
    strncpy(current_cfg.mqtt_topic, mqtt_topic->valuestring, sizeof(current_cfg.mqtt_topic) - 1);
    if (cJSON_IsString(first_family)) {
        strncpy(current_cfg.device_family, first_family->valuestring, sizeof(current_cfg.device_family) - 1);
    }
    cJSON_Delete(root);

    if (firmware_nvs_set_config(&current_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update NVS");
    } else {
        ESP_LOGI(TAG, "Config updated, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    vTaskDelete(NULL);
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
    esp_err_t wifi_status = wifi_manager_init(&sys_cfg);

    if (wifi_status != ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi connection failed. Entering Factory Default LED blink loop...");
        status_led_factory_blink_blocking();
        abort();
    }


    obtain_time();

    initialize_sntp();
    if (!wait_for_time_sync(10000)) {
        ESP_LOGW(TAG, "Time synchronization failed – HTTPS may not work");
    } else {
        ESP_LOGI(TAG, "System time synchronized");
    }

    // 5. Execute User Space NVS Migration Hook
    user_space_nvs_update_hook();


    ESP_LOGI(TAG, "Checking for updates...");
    xTaskCreate(&config_update_check_task, "config_check", 8192, NULL, 5, NULL);
    vTaskDelay(pdMS_TO_TICKS(5000)); // Allow config check to complete before proceeding

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