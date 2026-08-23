#include "gateway_board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

/* Official ESP32-S3-USB-OTG board signals. */
#define PIN_USB_SEL          GPIO_NUM_18
#define PIN_USB_LIMIT_EN     GPIO_NUM_17
#define PIN_USB_OVERCURRENT  GPIO_NUM_21
#define PIN_DEV_VBUS_EN      GPIO_NUM_12
#define PIN_BOOST_EN         GPIO_NUM_13
#define PIN_LED_GREEN        GPIO_NUM_15
#define PIN_LED_YELLOW       GPIO_NUM_16

static esp_err_t set_output(gpio_num_t pin, int level)
{
    ESP_RETURN_ON_ERROR(gpio_set_direction(pin, GPIO_MODE_OUTPUT), TAG, "gpio %d direction", pin);
    return gpio_set_level(pin, level);
}

esp_err_t gateway_board_usb_host_power_set(bool enabled)
{
    /* Never permit both power paths at once; the board documents that state as undefined. */
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_DEV_VBUS_EN, 0), TAG, "disable USB_DEV VBUS path");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_BOOST_EN, 0), TAG, "disable boost VBUS path");

    if (!enabled) {
        return ESP_OK;
    }

#if CONFIG_GATEWAY_USB_HOST_POWER_USB_DEV
    ESP_LOGI(TAG, "USB host VBUS source: USB_DEV 5 V input");
    return gpio_set_level(PIN_DEV_VBUS_EN, 1);
#elif CONFIG_GATEWAY_USB_HOST_POWER_BATTERY
    ESP_LOGI(TAG, "USB host VBUS source: battery boost");
    return gpio_set_level(PIN_BOOST_EN, 1);
#else
    ESP_LOGI(TAG, "USB host VBUS intentionally disabled");
    return ESP_OK;
#endif
}

bool gateway_board_usb_overcurrent(void)
{
    return gpio_get_level(PIN_USB_OVERCURRENT) != 0;
}

esp_err_t gateway_board_status_led_set(bool on)
{
    return gpio_set_level(PIN_LED_GREEN, on ? 1 : 0);
}

esp_err_t gateway_board_init(void)
{
    ESP_RETURN_ON_ERROR(set_output(PIN_DEV_VBUS_EN, 0), TAG, "DEV_VBUS_EN init");
    ESP_RETURN_ON_ERROR(set_output(PIN_BOOST_EN, 0), TAG, "BOOST_EN init");
    ESP_RETURN_ON_ERROR(set_output(PIN_USB_LIMIT_EN, 0), TAG, "LIMIT_EN init");
    ESP_RETURN_ON_ERROR(set_output(PIN_USB_SEL, 1), TAG, "USB_SEL host routing");
    ESP_RETURN_ON_ERROR(set_output(PIN_LED_GREEN, 0), TAG, "green LED init");
    ESP_RETURN_ON_ERROR(set_output(PIN_LED_YELLOW, 0), TAG, "yellow LED init");

    gpio_config_t input = {
        .pin_bit_mask = 1ULL << PIN_USB_OVERCURRENT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input), TAG, "over-current input init");

    /* The MIC2005A current limiter must be enabled for board-provided VBUS. */
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_USB_LIMIT_EN, 1), TAG, "enable USB current limiter");
    ESP_RETURN_ON_ERROR(gateway_board_usb_host_power_set(true), TAG, "enable USB host power");
    ESP_RETURN_ON_ERROR(gateway_board_status_led_set(true), TAG, "status LED");

    ESP_LOGI(TAG, "USB Type-A data routed to ESP32-S3 native USB host");
    return ESP_OK;
}
