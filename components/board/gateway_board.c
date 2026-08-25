#include "gateway_board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";
#define USB_OC_POLL_MS 100U
#define USB_OC_CLEAR_COOLDOWN_MS 5000U
static SemaphoreHandle_t s_power_mutex;
static bool s_power_requested = true;
static bool s_overcurrent_latched;
static uint32_t s_overcurrent_events;
static uint32_t s_power_cutoffs;

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

static esp_err_t power_apply(bool enabled)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_DEV_VBUS_EN, 0), TAG, "disable USB_DEV VBUS path");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_BOOST_EN, 0), TAG, "disable boost VBUS path");
    if (!enabled) return ESP_OK;
#if CONFIG_GATEWAY_USB_HOST_POWER_USB_DEV
    return gpio_set_level(PIN_DEV_VBUS_EN, 1);
#elif CONFIG_GATEWAY_USB_HOST_POWER_BATTERY
    return gpio_set_level(PIN_BOOST_EN, 1);
#else
    return ESP_OK;
#endif
}

esp_err_t gateway_board_usb_host_power_set(bool enabled)
{
    if (s_power_mutex != NULL && xSemaphoreTake(s_power_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_power_requested = enabled;
    esp_err_t err;
    if (enabled && s_overcurrent_latched) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = power_apply(enabled);
    }
    if (s_power_mutex != NULL) xSemaphoreGive(s_power_mutex);
    return err;
}

bool gateway_board_usb_host_power_control_available(void)
{
#if CONFIG_GATEWAY_USB_HOST_POWER_USB_DEV || CONFIG_GATEWAY_USB_HOST_POWER_BATTERY
    return true;
#else
    return false;
#endif
}

const char *gateway_board_usb_host_power_source_name(void)
{
#if CONFIG_GATEWAY_USB_HOST_POWER_USB_DEV
    return "USB_DEV";
#elif CONFIG_GATEWAY_USB_HOST_POWER_BATTERY
    return "battery_boost";
#else
    return "external/off";
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

static void power_monitor_task(void *arg)
{
    (void)arg;
    TickType_t clear_since = 0;
    for (;;) {
        const bool asserted = gateway_board_usb_overcurrent();
        if (s_power_mutex != NULL && xSemaphoreTake(s_power_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (asserted) {
                clear_since = 0;
                if (!s_overcurrent_latched) {
                    s_overcurrent_latched = true;
                    ++s_overcurrent_events;
                    ESP_LOGE(TAG, "USB host over-current asserted");
                    if (gateway_board_usb_host_power_control_available()) {
                        if (power_apply(false) == ESP_OK) ++s_power_cutoffs;
                    }
                }
            } else if (s_overcurrent_latched) {
                if (clear_since == 0) clear_since = xTaskGetTickCount();
                if ((xTaskGetTickCount() - clear_since) >= pdMS_TO_TICKS(USB_OC_CLEAR_COOLDOWN_MS)) {
                    s_overcurrent_latched = false;
                    clear_since = 0;
                    if (s_power_requested && gateway_board_usb_host_power_control_available()) {
                        ESP_LOGW(TAG, "USB over-current cleared; restoring host VBUS after cooldown");
                        (void)power_apply(true);
                    }
                }
            }
            xSemaphoreGive(s_power_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(USB_OC_POLL_MS));
    }
}

esp_err_t gateway_board_power_diagnostics(gateway_board_power_diagnostics_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_power_mutex != NULL && xSemaphoreTake(s_power_mutex, pdMS_TO_TICKS(250)) != pdTRUE) return ESP_ERR_TIMEOUT;
    out->overcurrent = gateway_board_usb_overcurrent();
    out->cutoff_latched = s_overcurrent_latched;
    out->overcurrent_events = s_overcurrent_events;
    out->power_cutoffs = s_power_cutoffs;
    if (s_power_mutex != NULL) xSemaphoreGive(s_power_mutex);
    return ESP_OK;
}

esp_err_t gateway_board_init(void)
{
    s_power_mutex = xSemaphoreCreateMutex();
    if (s_power_mutex == NULL) return ESP_ERR_NO_MEM;
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

    if (xTaskCreate(power_monitor_task, "usb_power_mon", 3072, NULL, 8, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "USB Type-A data routed to ESP32-S3 native USB host");
    return ESP_OK;
}
