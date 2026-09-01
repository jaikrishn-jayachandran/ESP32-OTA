#include "status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

void status_led_init(void) {
    gpio_reset_pin(STATUS_LED_GPIO);
    gpio_set_direction(STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
}

void status_led_set(bool state) {
    gpio_set_level(STATUS_LED_GPIO, state ? 1 : 0);
}

void status_led_factory_blink_blocking(void) {
    status_led_init();
    while (1) {
        gpio_set_level(STATUS_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(STATUS_LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}