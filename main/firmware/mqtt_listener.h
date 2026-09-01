#ifndef MQTT_LISTENER_H
#define MQTT_LISTENER_H

#include "esp_err.h"
#include "nvs_manager.h"

#include <stdbool.h>

esp_err_t firmware_mqtt_init(const sys_config_t *cfg);
void firmware_mqtt_stop(void);
bool firmware_mqtt_is_connected(void);

#endif // MQTT_LISTENER_H