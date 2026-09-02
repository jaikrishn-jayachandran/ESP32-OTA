#include "user_space.h"
#include <inttypes.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "firmware/status_led.h"
#include "firmware/wifi_manager.h"
#include "esp_crt_bundle.h"

static const char *TAG = "USER_SPACE";

void user_space_nvs_update_hook(void) {
    ESP_LOGI(TAG, "Executing User Space NVS Migration Hook...");
    
}

static void perform_user_http_request(void) {
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "Wi-Fi unavailable for user space request.");
        return;
    }

    esp_http_client_config_t config = {
        .url = "https://jsonplaceholder.typicode.com/todos/1",
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = NULL,    
        .cert_pem = NULL,
        .use_global_ca_store = false,
        .user_agent = "ESP32-User-Space-Agent/1.0"
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP User Request Success! Status Code: %d", 
                 esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "HTTP User Request Failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

void user_space_main(void *pvParameters) {
    (void)pvParameters; // Suppress unused parameter compiler warning

    ESP_LOGI(TAG, "Starting User Space Application Logic...");
    status_led_init();

    uint32_t loop_counter = 0;

    while (1) {
        // Pattern: 510blinks of 300ms followed by a 2-second pause
        for (int i = 0; i < 10; i++) {
            status_led_set(true);
            vTaskDelay(pdMS_TO_TICKS(500));
            status_led_set(false);
            vTaskDelay(pdMS_TO_TICKS(350));
        }

        // ESP_LOGI(TAG, "User Space Application running smoothly... Cycle: %" PRIu32, ++loop_counter);

        // Every 5 cycles, send an HTTP request using the shared Wi-Fi interface
        // if (loop_counter % 5 == 0) {
        //     perform_user_http_request();
        // }

        // 2-second pause before repeating pattern
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}