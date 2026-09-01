#ifndef STATUS_LED_H
#define STATUS_LED_H

#include "driver/gpio.h"

#include <stdbool.h>

#define STATUS_LED_GPIO GPIO_NUM_2 // Inbuilt LED GPIO for standard ESP32

void status_led_init(void);
void status_led_set(bool state);
void status_led_factory_blink_blocking(void); // 500ms continuous pattern

#endif // STATUS_LED_H