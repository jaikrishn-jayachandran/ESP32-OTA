#include "mqtt_listener.h"

#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_crt_bundle.h"
#include "psa/crypto.h"

#include "cJSON.h"

#include "ota_engine.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "FW_MQTT";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static sys_config_t cached_cfg;
static bool is_connected = false;
static bool is_subscribed = false;

#define AES_GCM_TAG_LENGTH 16
#define AES_GCM_IV_LENGTH 12



static esp_err_t decrypt_payload(const uint8_t *encrypted_data,
                                 size_t encrypted_len,
                                 uint8_t **plaintext,
                                 size_t *plaintext_len)
{
    if (encrypted_data == NULL ||
        plaintext == NULL ||
        plaintext_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (encrypted_len <= AES_GCM_IV_LENGTH + AES_GCM_TAG_LENGTH) {
        ESP_LOGE(TAG, "Encrypted payload too short");
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *iv = encrypted_data;
    const uint8_t *ciphertext = encrypted_data + AES_GCM_IV_LENGTH;
    const uint8_t *tag = encrypted_data + encrypted_len - AES_GCM_TAG_LENGTH;
    const size_t ciphertext_len = encrypted_len - AES_GCM_IV_LENGTH - AES_GCM_TAG_LENGTH;

    uint8_t *output = malloc(ciphertext_len + 1);
    if (output == NULL) {
        ESP_LOGE(TAG, "Failed to allocate plaintext buffer");
        return ESP_ERR_NO_MEM;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld", (long)status);
        free(output);
        return ESP_FAIL;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);

    psa_key_id_t key_id = 0;
    status = psa_import_key(&attributes, cached_cfg.aes_key,
                            sizeof(cached_cfg.aes_key), &key_id);
    psa_reset_key_attributes(&attributes);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key failed: %ld", (long)status);
        free(output);
        return ESP_FAIL;
    }

    /* Multipart GCM decrypt using psa_aead_verify() with separate tag */

    psa_aead_operation_t operation = PSA_AEAD_OPERATION_INIT;

    status = psa_aead_decrypt_setup(&operation, key_id, PSA_ALG_GCM);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_aead_decrypt_setup failed: %ld", (long)status);
        psa_destroy_key(key_id);
        free(output);
        return ESP_FAIL;
    }

    status = psa_aead_set_nonce(&operation, iv, AES_GCM_IV_LENGTH);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_aead_set_nonce failed: %ld", (long)status);
        psa_aead_abort(&operation);
        psa_destroy_key(key_id);
        free(output);
        return ESP_FAIL;
    }

    /* No AAD */
    status = psa_aead_update_ad(&operation, NULL, 0);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_aead_update_ad failed: %ld", (long)status);
        psa_aead_abort(&operation);
        psa_destroy_key(key_id);
        free(output);
        return ESP_FAIL;
    }

    /* Feed ciphertext */
    size_t output_len = 0;
    status = psa_aead_update(&operation, ciphertext, ciphertext_len,
                             output, ciphertext_len, &output_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_aead_update failed: %ld", (long)status);
        psa_aead_abort(&operation);
        psa_destroy_key(key_id);
        free(output);
        return ESP_FAIL;
    }

    /* Finish + authenticate with separate TAG */
    size_t finish_len = 0;
    status = psa_aead_verify(&operation,
                             output + output_len,
                             ciphertext_len - output_len,
                             &finish_len,
                             tag,
                             AES_GCM_TAG_LENGTH);

    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "GCM authentication failed: %ld", (long)status);
        free(output);
        return ESP_FAIL;
    }

    output[output_len + finish_len] = '\0';
    *plaintext = output;
    *plaintext_len = output_len + finish_len;

    return ESP_OK;
}


static void process_ota_ping_payload(const char *data, int data_len) {
    // Decrypt the payload first
    uint8_t *plaintext = NULL;
    size_t plaintext_len = 0;

    ESP_LOGI(TAG, "Encrypted payload length: %d", data_len);

    if (data_len > 0) {
        char hex_str[65] = {0};
        for (int i = 0; i < data_len && i < 16; i++) {
            sprintf(hex_str + i*2, "%02x", (uint8_t)data[i]);
        }
        ESP_LOGI(TAG, "Payload first 16 bytes: %s", hex_str);
    }

    ESP_LOGI(TAG, "AES key first 4 bytes: %02x %02x %02x %02x",
             cached_cfg.aes_key[0], cached_cfg.aes_key[1],
             cached_cfg.aes_key[2], cached_cfg.aes_key[3]);
    
    esp_err_t decrypt_err = decrypt_payload((const uint8_t *)data, data_len,
                                            &plaintext, &plaintext_len);
    if (decrypt_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to decrypt MQTT payload. Dropping.");
        return;
    }

    // Parse decrypted JSON
    cJSON *root = cJSON_ParseWithLength((const char *)plaintext, plaintext_len);
    free(plaintext);
    
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse decrypted MQTT JSON payload");
        return;
    }

    cJSON *event = cJSON_GetObjectItem(root, "event");
    cJSON *version = cJSON_GetObjectItem(root, "version");
    cJSON *family = cJSON_GetObjectItem(root, "family");
    cJSON *sha256 = cJSON_GetObjectItem(root, "sha256");

    // Validate essential keys exist
    if (!cJSON_IsString(event) || !cJSON_IsString(version) || 
        !cJSON_IsArray(family)) {
        ESP_LOGE(TAG, "Malformed OTA ping JSON structure");
        cJSON_Delete(root);
        return;
    }

    // Ignore non-OTA event pings
    if (strcmp(event->valuestring, "OTA_AVAILABLE") != 0) {
        cJSON_Delete(root);
        return;
    }

    // Family Check
    bool family_matched = false;
    cJSON *fam_elem = NULL;
    cJSON_ArrayForEach(fam_elem, family) {
        if (cJSON_IsString(fam_elem)) {
            if (strcmp(fam_elem->valuestring, "all") == 0 || 
                strcmp(fam_elem->valuestring, cached_cfg.device_family) == 0) {
                family_matched = true;
                break;
            }
        }
    }

    if (!family_matched) {
        ESP_LOGW(TAG, "Device family %s not target of update ping. Ignoring.", 
                 cached_cfg.device_family);
        cJSON_Delete(root);
        return;
    }

    // Set expected SHA-256 for verification
    if (cJSON_IsString(sha256) && strlen(sha256->valuestring) == 64) {
        firmware_ota_set_expected_sha256(sha256->valuestring);
    }

    ESP_LOGI(TAG, "OTA Update Ping VALID! Version: %s", version->valuestring);
    cJSON_Delete(root);

    // Create task for OTA to avoid blocking MQTT
    // The OTA task itself will acquire the mutex and reject if already in progress
    xTaskCreate(&firmware_ota_check_and_update_task, "ota_update_task", 
                8192, NULL, 5, NULL);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to MQTT Broker");
            is_connected = true;
            if (!is_subscribed) {
                ESP_LOGI(TAG, "Subscribing to topic: %s", cached_cfg.mqtt_topic);
                esp_mqtt_client_subscribe(mqtt_client, cached_cfg.mqtt_topic, 1);
                is_subscribed = true;
            }
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Received encrypted message on topic: %.*s", 
                     event->topic_len, event->topic);
            process_ota_ping_payload(event->data, event->data_len);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected. Auto-reconnecting in background...");
            is_connected = false;
            is_subscribed = false;  // Reset for resubscription
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Client Event Error encountered");
            break;

        default:
            break;
    }
}

esp_err_t firmware_mqtt_init(const sys_config_t *sys_cfg) {
    if (sys_cfg == NULL) return ESP_ERR_INVALID_ARG;
    
    // Cache configuration locally for task context access
    memcpy(&cached_cfg, sys_cfg, sizeof(sys_config_t));

    // Build URI
    char mqtt_uri[128];
    snprintf(mqtt_uri, sizeof(mqtt_uri), "mqtts://%s:%d", 
             cached_cfg.mqtt_host, cached_cfg.mqtt_port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = mqtt_uri,
                // REMOVED: .transport = MQTT_TRANSPORT_OVER_SSL (handled by mqtts:// prefix)
            },
            .verification = {
                .crt_bundle_attach = esp_crt_bundle_attach, // FIX: Attach standard x509 bundle
            },
        },
        .credentials = {
            .client_id = cached_cfg.device_id,
        },
        .session = {
            .keepalive = 60,
            .disable_clean_session = false,
        },
        .task = {
            .priority = 3,
            .stack_size = 8192,
        }
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to instantiate MQTT client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, 
                                                    mqtt_event_handler, NULL));
    return esp_mqtt_client_start(mqtt_client);
}

void firmware_mqtt_stop(void) {
    if (mqtt_client != NULL) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        is_connected = false;
        is_subscribed = false;
    }
}

bool firmware_mqtt_is_connected(void) {
    return is_connected;
}