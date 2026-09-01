#include "ota_engine.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include "nvs_manager.h"
#include "esp_crt_bundle.h"
#include "psa/crypto.h"

static const char *TAG = "FW_OTA";
static sys_config_t ota_cfg;
static SemaphoreHandle_t ota_mutex = NULL;
static ota_state_t ota_state = OTA_STATE_IDLE;
static char expected_sha256[65] = {0};

#define FIRMWARE_BIN_NAME "ESP-32-OTA-Firmware-Update.bin"
#define MAX_URL_LENGTH 512
#define MAX_VERSION_LENGTH 32
#define CONFIG_JSON_FILENAME "config.json"
#define SHA256_CHUNK_SIZE 4096

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex_out) {
    for (size_t i = 0; i < len; i++) {
        snprintf(hex_out + (i * 2), 3, "%02x", bytes[i]);
    }
    hex_out[len * 2] = '\0';
}

static esp_err_t verify_downloaded_image(int image_size) {
    if (strlen(expected_sha256) == 0) {
        ESP_LOGI(TAG, "No expected SHA-256 set. Skipping verification.");
        return ESP_OK;
    }

    const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
    if (ota_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get OTA partition for verification");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Verifying SHA-256 of downloaded image (%d bytes)...", image_size);

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld", (long)status);
        return ESP_FAIL;
    }

    psa_hash_operation_t hash_op = PSA_HASH_OPERATION_INIT;
    status = psa_hash_setup(&hash_op, PSA_ALG_SHA_256);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_setup failed: %ld", (long)status);
        return ESP_FAIL;
    }

    uint8_t buf[SHA256_CHUNK_SIZE];
    int remaining = image_size;
    int offset = 0;

    while (remaining > 0) {
        int to_read = (remaining > SHA256_CHUNK_SIZE) ? SHA256_CHUNK_SIZE : remaining;

        esp_err_t err = esp_partition_read(ota_partition, offset, buf, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read OTA partition at offset %d: %s", offset, esp_err_to_name(err));
            psa_hash_abort(&hash_op);
            return ESP_FAIL;
        }

        status = psa_hash_update(&hash_op, buf, to_read);
        if (status != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa_hash_update failed: %ld", (long)status);
            psa_hash_abort(&hash_op);
            return ESP_FAIL;
        }

        offset += to_read;
        remaining -= to_read;
    }

    uint8_t hash[32];
    size_t hash_len = 0;
    status = psa_hash_finish(&hash_op, hash, sizeof(hash), &hash_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_finish failed: %ld", (long)status);
        return ESP_FAIL;
    }

    char computed_hex[65];
    bytes_to_hex(hash, hash_len, computed_hex);

    ESP_LOGI(TAG, "Computed SHA-256:  %s", computed_hex);
    ESP_LOGI(TAG, "Expected SHA-256:  %s", expected_sha256);

    if (strcmp(computed_hex, expected_sha256) != 0) {
        ESP_LOGE(TAG, "SHA-256 MISMATCH! Firmware integrity check FAILED.");
        ESP_LOGE(TAG, "Discarding staged version and rolling back.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SHA-256 verification PASSED.");
    return ESP_OK;
}

esp_err_t firmware_ota_init(void) {
    ota_mutex = xSemaphoreCreateMutex();
    if (ota_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create OTA mutex");
        return ESP_ERR_NO_MEM;
    }
    ota_state = OTA_STATE_IDLE;
    return ESP_OK;
}

esp_err_t firmware_ota_acquire(void) {
    if (ota_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire OTA mutex (timeout)");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void firmware_ota_release(void) {
    if (ota_mutex != NULL) {
        xSemaphoreGive(ota_mutex);
    }
}

ota_state_t firmware_ota_get_state(void) {
    return ota_state;
}

void firmware_ota_set_expected_sha256(const char *hex_hash) {
    if (hex_hash != NULL) {
        strncpy(expected_sha256, hex_hash, sizeof(expected_sha256) - 1);
        expected_sha256[sizeof(expected_sha256) - 1] = '\0';
        ESP_LOGI(TAG, "Expected SHA-256 set: %s", expected_sha256);
    }
}

static esp_err_t parse_version_from_config(const char *json_str, char **version_out) {
    if (json_str == NULL || version_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *version_out = NULL;

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse config.json");
        return ESP_FAIL;
    }

    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsString(version)) {
        ESP_LOGE(TAG, "config.json missing 'version' field");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    *version_out = strdup(version->valuestring);
    if (*version_out == NULL) {
        ESP_LOGE(TAG, "Failed to allocate version string");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Found version in config.json: %s", *version_out);

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_config_version(char **version) {
    if (version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *version = NULL;

    char config_url[256];
    snprintf(config_url, sizeof(config_url),
             "https://raw.githubusercontent.com/%s/main/%s",
             ota_cfg.github_repo, CONFIG_JSON_FILENAME);

    ESP_LOGI(TAG, "Fetching config from: %s", config_url);

    esp_http_client_config_t config = {
        .url = config_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = false,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Client");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "Failed to fetch headers");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *response_buffer = malloc(content_length + 1);
    if (response_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read_response(client, response_buffer, content_length);
    response_buffer[read_len] = '\0';

    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_code != 200 || read_len <= 0) {
        ESP_LOGE(TAG, "Failed to fetch config.json. Status: %d", status_code);
        free(response_buffer);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "config.json: %s", response_buffer);

    err = parse_version_from_config(response_buffer, version);
    free(response_buffer);

    return err;
}

static void build_binary_url(char *url, size_t url_len) {
    snprintf(url, url_len,
             "https://raw.githubusercontent.com/%s/main/%s",
             ota_cfg.github_repo, FIRMWARE_BIN_NAME);
}

esp_err_t firmware_ota_start_download(const char *bin_url) {
    if (bin_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Downloading firmware from: %s", bin_url);

    esp_http_client_config_t ota_client_config = {
        .url = bin_url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = false,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &ota_client_config,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    while (1) {
        ret = esp_https_ota_perform(handle);
        if (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            continue;
        }
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Firmware download failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(handle);
        return ret;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "Incomplete firmware data received");
        esp_https_ota_abort(handle);
        return ESP_FAIL;
    }

    int image_size = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "Firmware download complete. Image size: %d bytes", image_size);

    ret = esp_https_ota_finish(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = verify_downloaded_image(image_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Firmware verification FAILED. Rolling back...");
        firmware_nvs_discard_stage();
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful. Rebooting...");
    esp_restart();
    return ESP_OK;
}

void firmware_ota_check_and_update_task(void *param) {
    esp_err_t result = ESP_OK;
    char *remote_version = NULL;

    if (firmware_ota_acquire() != ESP_OK) {
        ESP_LOGW(TAG, "Cannot start OTA: another OTA operation is in progress");
        vTaskDelete(NULL);
        return;
    }

    ota_state = OTA_STATE_RUNNING;

    result = firmware_nvs_get_config(&ota_cfg);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load configuration: %s", esp_err_to_name(result));
        goto cleanup;
    }

    result = fetch_config_version(&remote_version);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fetch config from GitHub");
        goto cleanup;
    }

    if (strcmp(remote_version, ota_cfg.current_version) == 0) {
        ESP_LOGI(TAG, "Version match: %s. No update needed.", ota_cfg.current_version);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Version mismatch!");
    ESP_LOGI(TAG, "Current: %s | Remote: %s", ota_cfg.current_version, remote_version);

    if (strlen(expected_sha256) > 0) {
        ESP_LOGI(TAG, "Expected SHA-256: %s", expected_sha256);
    }

    ESP_LOGI(TAG, "Proceeding with OTA update...");

    firmware_nvs_stage_str("ver", remote_version);

    char bin_url[MAX_URL_LENGTH];
    build_binary_url(bin_url, sizeof(bin_url));

    result = firmware_ota_start_download(bin_url);

    if (result != ESP_OK) {
        firmware_nvs_discard_stage();
    }

cleanup:
    free(remote_version);
    memset(expected_sha256, 0, sizeof(expected_sha256));

    ota_state = (result == ESP_OK) ? OTA_STATE_SUCCESS : OTA_STATE_FAILED;
    firmware_ota_release();

    ESP_LOGI(TAG, "OTA task finished: %s", esp_err_to_name(result));
    vTaskDelete(NULL);
}

void firmware_ota_verify_or_rollback(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t img_state;

    if (esp_ota_get_state_partition(running, &img_state) == ESP_OK) {
        if (img_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "OTA update pending verification. Marking as valid...");
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                firmware_nvs_commit_staged();
                ESP_LOGI(TAG, "OTA update marked as valid. Committing staged configuration...");
            }
        } else {
            ESP_LOGI(TAG, "Failed boot. Rolling back...");
            firmware_nvs_discard_stage();
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }
}
