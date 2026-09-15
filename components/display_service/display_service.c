#include "display_service.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Pin assignments and ST7789 controller are from Espressif's ESP32-S3-USB-OTG BSP. */
#define LCD_MOSI GPIO_NUM_7
#define LCD_CLK GPIO_NUM_6
#define LCD_CS GPIO_NUM_5
#define LCD_DC GPIO_NUM_4
#define LCD_RST GPIO_NUM_8
#define LCD_BL GPIO_NUM_9
#define LCD_W 240
#define LCD_H 240
#define C_BLACK 0x0000
#define C_WHITE 0xffff
#define C_BLUE 0x001f
#define C_GREEN 0x07e0

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_frame;
static network_service_snapshot_t s_snapshot;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_dirty = true;

/* Compact 5x7 font for the setup information shown on a 1.3-inch LCD. */
static const uint8_t font[][5] = {
    ['0']={0x3e,0x51,0x49,0x45,0x3e}, ['1']={0,0x42,0x7f,0x40,0}, ['2']={0x42,0x61,0x51,0x49,0x46}, ['3']={0x21,0x41,0x45,0x4b,0x31}, ['4']={0x18,0x14,0x12,0x7f,0x10}, ['5']={0x27,0x45,0x45,0x45,0x39}, ['6']={0x3c,0x4a,0x49,0x49,0x30}, ['7']={0x01,0x71,0x09,0x05,0x03}, ['8']={0x36,0x49,0x49,0x49,0x36}, ['9']={0x06,0x49,0x49,0x29,0x1e},
    ['A']={0x7e,0x11,0x11,0x11,0x7e}, ['B']={0x7f,0x49,0x49,0x49,0x36}, ['C']={0x3e,0x41,0x41,0x41,0x22}, ['D']={0x7f,0x41,0x41,0x22,0x1c}, ['E']={0x7f,0x49,0x49,0x49,0x41}, ['F']={0x7f,0x09,0x09,0x09,0x01}, ['G']={0x3e,0x41,0x49,0x49,0x7a}, ['H']={0x7f,0x08,0x08,0x08,0x7f}, ['I']={0,0x41,0x7f,0x41,0}, ['J']={0x20,0x40,0x41,0x3f,0x01}, ['K']={0x7f,0x08,0x14,0x22,0x41}, ['L']={0x7f,0x40,0x40,0x40,0x40}, ['M']={0x7f,0x02,0x0c,0x02,0x7f}, ['N']={0x7f,0x04,0x08,0x10,0x7f}, ['O']={0x3e,0x41,0x41,0x41,0x3e}, ['P']={0x7f,0x09,0x09,0x09,0x06}, ['Q']={0x3e,0x41,0x51,0x21,0x5e}, ['R']={0x7f,0x09,0x19,0x29,0x46}, ['S']={0x46,0x49,0x49,0x49,0x31}, ['T']={0x01,0x01,0x7f,0x01,0x01}, ['U']={0x3f,0x40,0x40,0x40,0x3f}, ['V']={0x1f,0x20,0x40,0x20,0x1f}, ['W']={0x7f,0x20,0x18,0x20,0x7f}, ['X']={0x63,0x14,0x08,0x14,0x63}, ['Y']={0x07,0x08,0x70,0x08,0x07}, ['Z']={0x61,0x51,0x49,0x45,0x43},
    ['-']={0x08,0x08,0x08,0x08,0x08}, ['.']={0x60,0x60,0,0,0}, [':']={0,0x36,0x36,0,0}, ['/']={0x60,0x10,0x08,0x04,0x03}, [' ']={0,0,0,0,0}
};

static void pixel(int x, int y, uint16_t color) { if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H) s_frame[y * LCD_W + x] = color; }
static void text(int x, int y, int scale, const char *value, uint16_t color)
{
    for (; *value != '\0'; ++value, x += 6 * scale) {
        const unsigned char ch = (unsigned char)toupper((unsigned char)*value);
        const uint8_t *glyph = ch < sizeof(font) / sizeof(font[0]) ? font[ch] : font[' '];
        for (int col = 0; col < 5; ++col) for (int row = 0; row < 7; ++row) if (glyph[col] & (1u << row))
            for (int dx = 0; dx < scale; ++dx) for (int dy = 0; dy < scale; ++dy) pixel(x + col * scale + dx, y + row * scale + dy, color);
    }
}
static void draw(const network_service_snapshot_t *state)
{
    memset(s_frame, 0, LCD_W * LCD_H * sizeof(*s_frame));
    if (state->provisioning) {
        text(18, 18, 3, "SMS GATEWAY", C_GREEN);
        text(12, 58, 2, "SETUP WIFI", C_WHITE);
        text(12, 88, 2, "SSID", C_BLUE); text(12, 106, 2, state->portal_ssid, C_WHITE);
        text(12, 140, 2, "PASS", C_BLUE); text(12, 158, 2, state->portal_passphrase, C_WHITE);
        text(12, 198, 2, "192.168.4.1", C_GREEN);
    } else if (state->connected) {
        text(18, 28, 3, "SMS GATEWAY", C_GREEN);
        text(18, 82, 2, "ONLINE", C_WHITE);
        text(18, 112, 2, state->ipv4, C_BLUE);
        text(18, 164, 2, "MODEM STARTING", C_WHITE);
    } else {
        text(18, 28, 3, "SMS GATEWAY", C_WHITE);
        text(18, 90, 2, "WIFI RECONNECT", C_BLUE);
    }
    const esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_frame);
    if (err != ESP_OK) ESP_LOGW(TAG, "LCD refresh failed: %s", esp_err_to_name(err));
}
static void display_task(void *arg)
{
    (void)arg;
    for (;;) { if (s_dirty) { network_service_snapshot_t state; portENTER_CRITICAL(&s_lock); state = s_snapshot; s_dirty = false; portEXIT_CRITICAL(&s_lock); draw(&state); } vTaskDelay(pdMS_TO_TICKS(150)); }
}
void display_service_network_event(const network_service_snapshot_t *snapshot, void *user_ctx)
{
    (void)user_ctx; if (snapshot == NULL) return;
    portENTER_CRITICAL(&s_lock);
    const bool changed = s_snapshot.provisioning != snapshot->provisioning ||
                         s_snapshot.connected != snapshot->connected ||
                         strcmp(s_snapshot.ipv4, snapshot->ipv4) != 0 ||
                         strcmp(s_snapshot.portal_ssid, snapshot->portal_ssid) != 0 ||
                         strcmp(s_snapshot.portal_passphrase, snapshot->portal_passphrase) != 0;
    s_snapshot = *snapshot;
    if (changed) s_dirty = true;
    portEXIT_CRITICAL(&s_lock);
}
esp_err_t display_service_init(void)
{
    s_frame = heap_caps_malloc(LCD_W * LCD_H * sizeof(*s_frame), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_frame == NULL) return ESP_ERR_NO_MEM;
    const spi_bus_config_t bus = {.sclk_io_num = LCD_CLK, .mosi_io_num = LCD_MOSI, .miso_io_num = GPIO_NUM_NC, .quadwp_io_num = GPIO_NUM_NC, .quadhd_io_num = GPIO_NUM_NC, .max_transfer_sz = LCD_W * LCD_H * 2};
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "LCD SPI");
    const esp_lcd_panel_io_spi_config_t io_cfg = {.dc_gpio_num = LCD_DC, .cs_gpio_num = LCD_CS, .pclk_hz = 20 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 1};
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io), TAG, "LCD IO");
    const esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel), TAG, "ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "LCD init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "LCD invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "LCD on");
    gpio_config_t bl = {.pin_bit_mask = 1ULL << LCD_BL, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "LCD backlight");
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_BL, 1), TAG, "LCD backlight on");
    return xTaskCreate(display_task, "lcd_status", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
