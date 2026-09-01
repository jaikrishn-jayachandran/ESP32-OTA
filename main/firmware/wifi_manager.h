#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "nvs_manager.h"

#include <stdbool.h>

esp_err_t wifi_manager_init(const sys_config_t *sys_cfg);
bool wifi_manager_is_connected(void);

#endif // WIFI_MANAGER_H