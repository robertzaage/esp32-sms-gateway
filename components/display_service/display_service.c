#include "display_service.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_board.h"
#include "gateway_diag.h"
#include "gateway_security.h"
#include "modem_core.h"
#include "mqtt_service.h"

/*
 * Status display for the board's 1.3" 240x240 ST7789 panel.
 *
 * The screen only lights up when something worth reading changes (Wi-Fi,
 * MQTT or modem state, a new SMS, setup progress) or a button is pressed, and
 * goes dark again after CONFIG_GATEWAY_DISPLAY_TIMEOUT_SECONDS. Each wake also
 * shifts the whole image by a few pixels so static text never sits on the
 * same pixels for long.
 */

/* Pin assignments and ST7789 controller are from Espressif's ESP32-S3-USB-OTG BSP. */
#define LCD_MOSI GPIO_NUM_7
#define LCD_CLK GPIO_NUM_6
#define LCD_CS GPIO_NUM_5
#define LCD_DC GPIO_NUM_4
#define LCD_RST GPIO_NUM_8
#define LCD_BL GPIO_NUM_9
#define LCD_W 240
#define LCD_H 240

#define DISPLAY_POLL_MS 50
#define DISPLAY_REFRESH_MS 1000
#define DISPLAY_SHIFT_MAX 4
/*
 * The screen is drawn in horizontal strips of BAND_H lines through one small
 * DMA buffer (11.5 KB). A full 240x240 frame would need 115 KB of internal RAM,
 * which leaves too little for Wi-Fi to start.
 */
#define BAND_H 24
#define BUTTON_LONG_PRESS_MS (CONFIG_GATEWAY_DISPLAY_PORTAL_HOLD_SECONDS * 1000)

/* RGB565, byte-swapped because the SPI panel expects big-endian pixels. */
#define RGB(r, g, b) ((uint16_t)(((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)) >> 8 | \
                                 ((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)) << 8))
#define C_BLACK RGB(0, 0, 0)
#define C_WHITE RGB(255, 255, 255)
#define C_GREY RGB(140, 140, 140)
#define C_GREEN RGB(40, 220, 90)
#define C_YELLOW RGB(255, 210, 0)
#define C_RED RGB(255, 70, 60)
#define C_BLUE RGB(80, 160, 255)

static const char *TAG = "display";

typedef enum {
    PAGE_STATUS = 0,
    PAGE_SETUP,
    PAGE_CRASH, /* previous boot crashed; shown until a button is pressed */
} display_page_t;

/*
 * Everything that is drawn. "Significant" fields wake the display when they
 * change; the rest is only redrawn while it is already on.
 */
typedef struct {
    display_page_t page;
    bool wifi_connected;
    bool provisioned;
    char ipv4[NETWORK_IPV4_MAX];
    char sta_ssid[NETWORK_SSID_MAX];
    network_portal_reason_t portal_reason;
    network_setup_state_t setup_state;
    char portal_ssid[NETWORK_PORTAL_SSID_MAX];
    char portal_passphrase[NETWORK_PORTAL_PASSWORD_MAX];
    int mqtt; /* 0 disabled, 1 offline, 2 connected */
    modem_state_t modem_state;
    modem_sim_state_t sim;
    uint32_t sms_id;
    char sms_sender[SMS_MAX_ADDRESS_LENGTH];
    char sms_time[16];
    char sms_preview[64];
    gateway_safe_mode_t safe_mode;
} view_significant_t;

typedef struct {
    int signal_bars; /* -1 unknown, 0..4 */
    int rssi_dbm;
    char operator_name[24];
} view_minor_t;

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_band;
static int s_band_y;
static SemaphoreHandle_t s_flush_done;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static network_service_snapshot_t s_net;
static bool s_net_valid;
static uint32_t s_sms_id;
static char s_sms_sender[SMS_MAX_ADDRESS_LENGTH];
static char s_sms_time[16];
static char s_sms_preview[64];
static bool s_crash_dismissed;
static int s_shift_x;
static int s_shift_y;

/* Classic 5x7 font, columns LSB-top, printable ASCII 0x20..0x7e. */
static const uint8_t font[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5f,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1c,0x22,0x41,0x00},{0x00,0x41,0x22,0x1c,0x00},{0x14,0x08,0x3e,0x08,0x14},{0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3e},{0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},{0x3e,0x41,0x49,0x49,0x7a},
    {0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},
    {0x7f,0x40,0x40,0x40,0x40},{0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7f,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},{0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78},{0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},{0x7f,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7f,0x40,0x00},{0x7c,0x04,0x18,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7c,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20},{0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7f,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},
};

static void fill(int x, int y, int w, int h, uint16_t color)
{
    x += s_shift_x;
    y += s_shift_y;
    for (int row = y; row < y + h; ++row) {
        if (row < s_band_y || row >= s_band_y + BAND_H) continue;
        for (int col = x; col < x + w; ++col) {
            if (col >= 0 && col < LCD_W) s_band[(row - s_band_y) * LCD_W + col] = color;
        }
    }
}

/* Draws ASCII text; returns the x position after the last glyph. */
static int text(int x, int y, int scale, const char *value, uint16_t color)
{
    for (; value != NULL && *value != '\0'; ++value, x += 6 * scale) {
        unsigned char ch = (unsigned char)*value;
        if (ch < 0x20 || ch > 0x7e) ch = '?';
        const uint8_t *glyph = font[ch - 0x20];
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if (glyph[col] & (1u << row)) fill(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
    return x;
}

static void signal_bars(int x, int y, int bars)
{
    for (int i = 0; i < 4; ++i) {
        const int h = 4 + i * 4;
        fill(x + i * 6, y + 16 - h, 4, h, bars < 0 ? C_GREY : (i < bars ? C_WHITE : RGB(60, 60, 60)));
    }
}

/* Reduce UTF-8 to the ASCII font: German umlauts are transliterated, anything else becomes '?'. */
static void utf8_to_ascii(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0' && o + 2 < out_size; ++p) {
        if (*p < 0x80) {
            out[o++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : (char)*p;
            continue;
        }
        const char *repl = "?";
        if (*p == 0xc3 && p[1] != '\0') {
            switch (p[1]) {
            case 0xa4: repl = "ae"; break;
            case 0xb6: repl = "oe"; break;
            case 0xbc: repl = "ue"; break;
            case 0x84: repl = "Ae"; break;
            case 0x96: repl = "Oe"; break;
            case 0x9c: repl = "Ue"; break;
            case 0x9f: repl = "ss"; break;
            default: break;
            }
        }
        for (; *repl != '\0' && o + 1 < out_size; ++repl) out[o++] = *repl;
        while ((p[1] & 0xc0) == 0x80) ++p; /* skip continuation bytes */
    }
    out[o] = '\0';
}

static const char *modem_label(modem_state_t state, modem_sim_state_t sim, uint16_t *color)
{
    *color = C_YELLOW;
    if (sim == MODEM_SIM_PIN_REQUIRED) { *color = C_RED; return "SIM PIN needed"; }
    if (sim == MODEM_SIM_PUK_REQUIRED || sim == MODEM_SIM_BLOCKED) { *color = C_RED; return "SIM locked"; }
    if (sim == MODEM_SIM_NOT_INSERTED) { *color = C_RED; return "No SIM"; }
    switch (state) {
    case MODEM_STATE_READY: *color = C_GREEN; return "ready";
    case MODEM_STATE_BOOT:
    case MODEM_STATE_WAIT_USB: *color = C_GREY; return "waiting for USB";
    case MODEM_STATE_ENUMERATE:
    case MODEM_STATE_FIND_AT_INTERFACE:
    case MODEM_STATE_AT_PROBE: return "connecting";
    case MODEM_STATE_AT_READY:
    case MODEM_STATE_SIM_CHECK: return "checking SIM";
    case MODEM_STATE_NETWORK_REGISTER: return "registering";
    case MODEM_STATE_RECOVERY: return "recovering";
    case MODEM_STATE_DEGRADED: *color = C_RED; return "error";
    default: return "unknown";
    }
}

static void draw_setup(const view_significant_t *v)
{
    text(8, 6, 2, "Gateway setup", C_GREEN);
    text(8, 34, 2, "1. Join Wi-Fi:", C_GREY);
    text(8, 54, 2, v->portal_ssid, C_WHITE);
    text(8, 80, 2, "Password:", C_GREY);
    text(8, 100, 3, v->portal_passphrase, C_WHITE);
    text(8, 132, 2, "2. Open in browser:", C_GREY);
    text(8, 152, 2, "192.168.4.1", C_BLUE);

    char line[48];
    switch (v->setup_state) {
    case NETWORK_SETUP_CONNECTING:
        text(8, 184, 2, "Connecting to", C_YELLOW);
        text(8, 204, 2, v->sta_ssid, C_YELLOW);
        break;
    case NETWORK_SETUP_CONNECTED:
        text(8, 184, 2, "Connected!", C_GREEN);
        snprintf(line, sizeof(line), "IP %s", v->ipv4);
        text(8, 204, 2, line, C_GREEN);
        break;
    case NETWORK_SETUP_FAILED:
        text(8, 184, 2, "Wi-Fi failed:", C_RED);
        text(8, 204, 2, "check password", C_RED);
        break;
    case NETWORK_SETUP_IDLE:
    default:
        if (v->portal_reason == NETWORK_PORTAL_FALLBACK) {
            text(8, 184, 2, "Still retrying", C_GREY);
            text(8, 204, 2, v->sta_ssid, C_GREY);
        } else if (v->wifi_connected) {
            snprintf(line, sizeof(line), "LAN %s", v->ipv4);
            text(8, 194, 2, line, C_GREY);
        }
        break;
    }
}

static void draw_crash(void)
{
    text(8, 6, 2, "Last boot crashed", C_RED);
    /* Wrap the summary at 19 characters per line; photograph this screen. */
    const char *crash = gateway_diag_last_crash(NULL);
    char line[20];
    const size_t len = strlen(crash);
    for (int row = 0; row < 9 && (size_t)row * 19 < len; ++row) {
        snprintf(line, sizeof(line), "%.19s", crash + row * 19);
        text(8, 28 + row * 21, 2, line, C_WHITE);
    }
    text(8, 226, 1, "Press any button to continue", C_GREY);
}

static void draw_status(const view_significant_t *v, const view_minor_t *m)
{
    text(8, 6, 2, v->safe_mode != GATEWAY_SAFE_MODE_OFF ? "SAFE MODE" : "SMS Gateway",
         v->safe_mode != GATEWAY_SAFE_MODE_OFF ? C_RED : C_GREEN);

    text(8, 34, 2, "WiFi", C_GREY);
    if (v->wifi_connected) text(68, 34, 2, v->ipv4, C_WHITE);
    else text(68, 34, 2, v->provisioned ? "connecting" : "not set up", C_YELLOW);

    text(8, 56, 2, "MQTT", C_GREY);
    text(68, 56, 2, v->mqtt == 2 ? "connected" : (v->mqtt == 1 ? "offline" : "off"),
         v->mqtt == 2 ? C_WHITE : (v->mqtt == 1 ? C_YELLOW : C_GREY));

    uint16_t color;
    const char *label = modem_label(v->modem_state, v->sim, &color);
    text(8, 78, 2, "LTE", C_GREY);
    text(68, 78, 2, label, color);

    if (v->modem_state == MODEM_STATE_READY || m->signal_bars >= 0) {
        signal_bars(8, 100, m->signal_bars);
        char line[40];
        if (m->rssi_dbm > -200 && m->rssi_dbm < 0) {
            snprintf(line, sizeof(line), "%.10s %ddBm", m->operator_name[0] ? m->operator_name : "-", m->rssi_dbm);
        } else {
            snprintf(line, sizeof(line), "%.16s", m->operator_name[0] ? m->operator_name : "-");
        }
        text(40, 102, 2, line, C_WHITE);
    }

    fill(8, 128, LCD_W - 16 - DISPLAY_SHIFT_MAX, 1, RGB(70, 70, 70));
    if (v->sms_id == 0) {
        text(8, 140, 2, "No SMS received", C_GREY);
        return;
    }
    char header[40];
    snprintf(header, sizeof(header), "Last SMS %s", v->sms_time);
    text(8, 138, 2, header, C_GREY);
    text(8, 160, 2, v->sms_sender, C_BLUE);
#if CONFIG_GATEWAY_DISPLAY_SMS_PREVIEW
    /* Two lines of 19 characters. */
    char line[20];
    for (int i = 0; i < 2; ++i) {
        const size_t start = (size_t)i * 19;
        if (start >= strlen(v->sms_preview)) break;
        snprintf(line, sizeof(line), "%.19s", v->sms_preview + start);
        text(8, 184 + i * 22, 2, line, C_WHITE);
    }
#endif
}

static void flush_band(void)
{
    (void)xSemaphoreTake(s_flush_done, 0);
    const esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, s_band_y, LCD_W, s_band_y + BAND_H, s_band);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD refresh failed: %s", esp_err_to_name(err));
        return;
    }
    /* Do not touch the strip buffer while DMA still reads it. */
    (void)xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(500));
}

/* Draws the page strip by strip; v == NULL clears the screen. */
static void render(const view_significant_t *v, const view_minor_t *m)
{
    for (s_band_y = 0; s_band_y < LCD_H; s_band_y += BAND_H) {
        memset(s_band, 0, LCD_W * BAND_H * sizeof(*s_band));
        if (v == NULL) { /* blank */ }
        else if (v->page == PAGE_CRASH) draw_crash();
        else if (v->page == PAGE_SETUP) draw_setup(v);
        else draw_status(v, m);
        flush_band();
    }
}

static void panel_power(bool on)
{
    if (on) {
        (void)esp_lcd_panel_disp_sleep(s_panel, false);
        vTaskDelay(pdMS_TO_TICKS(120)); /* ST7789 needs 120 ms after SLPOUT */
        (void)esp_lcd_panel_disp_on_off(s_panel, true);
        (void)gpio_set_level(LCD_BL, 1);
    } else {
        (void)gpio_set_level(LCD_BL, 0);
        (void)esp_lcd_panel_disp_on_off(s_panel, false);
        (void)esp_lcd_panel_disp_sleep(s_panel, true);
    }
}

static int signal_to_bars(int rssi)
{
    if (rssi < 0 || rssi == 99) return -1;
    if (rssi >= 20) return 4;
    if (rssi >= 15) return 3;
    if (rssi >= 10) return 2;
    if (rssi >= 5) return 1;
    return 0;
}

static void collect(view_significant_t *v, view_minor_t *m)
{
    memset(v, 0, sizeof(*v));
    memset(m, 0, sizeof(*m));
    m->signal_bars = -1;
    m->rssi_dbm = 0;

    portENTER_CRITICAL(&s_lock);
    const bool net_valid = s_net_valid;
    if (net_valid) {
        v->page = s_net.provisioning ? PAGE_SETUP : PAGE_STATUS;
        v->wifi_connected = s_net.connected;
        v->provisioned = s_net.provisioned;
        memcpy(v->ipv4, s_net.ipv4, sizeof(v->ipv4));
        memcpy(v->sta_ssid, s_net.sta_ssid, sizeof(v->sta_ssid));
        v->portal_reason = s_net.portal_reason;
        v->setup_state = s_net.setup_state;
        memcpy(v->portal_ssid, s_net.portal_ssid, sizeof(v->portal_ssid));
        memcpy(v->portal_passphrase, s_net.portal_passphrase, sizeof(v->portal_passphrase));
    }
    v->sms_id = s_sms_id;
    memcpy(v->sms_sender, s_sms_sender, sizeof(v->sms_sender));
    memcpy(v->sms_time, s_sms_time, sizeof(v->sms_time));
    memcpy(v->sms_preview, s_sms_preview, sizeof(v->sms_preview));
    portEXIT_CRITICAL(&s_lock);
    /* While setup is in progress, keep the setup page (with its result) visible. */
    if (v->setup_state == NETWORK_SETUP_CONNECTING || v->setup_state == NETWORK_SETUP_CONNECTED) v->page = PAGE_SETUP;
    v->safe_mode = gateway_diag_safe_mode();
    bool fresh = false;
    if (!s_crash_dismissed && gateway_diag_last_crash(&fresh)[0] != '\0' && fresh) v->page = PAGE_CRASH;

    mqtt_service_diagnostics_t mqtt = {0};
    if (mqtt_service_get_diagnostics(&mqtt) == ESP_OK && mqtt.enabled) v->mqtt = mqtt.connected ? 2 : 1;

    v->modem_state = modem_core_state();
    modem_manager_snapshot_t modem;
    if (modem_core_manager_snapshot(&modem) == ESP_OK) {
        v->sim = modem.sim;
        m->signal_bars = signal_to_bars(modem.signal.rssi);
        m->rssi_dbm = modem.signal.rssi == 99 ? 0 : modem.signal.rssi_dbm;
        utf8_to_ascii(modem.operator_info.name, m->operator_name, sizeof(m->operator_name));
    }
}

static void display_task(void *arg)
{
    (void)arg;
    view_significant_t shown = {0}, current;
    view_minor_t shown_minor = {0}, current_minor;
    bool on = true;
    bool first = true;
    int64_t last_activity_ms = esp_timer_get_time() / 1000;
    int64_t last_refresh_ms = 0;
    int64_t press_started_ms[GATEWAY_BUTTON_COUNT] = {0};
    bool long_press_fired = false;

    for (;;) {
        const int64_t now = esp_timer_get_time() / 1000;
        bool wake = false;

        /* Buttons: any press wakes the display; holding MENU opens the setup portal. */
        for (int b = 0; b < GATEWAY_BUTTON_COUNT; ++b) {
            if (gateway_board_button_pressed((gateway_board_button_t)b)) {
                if (press_started_ms[b] == 0) {
                    press_started_ms[b] = now;
                    /* The first press only lights a dark screen; a press on a lit crash page dismisses it. */
                    if (on) s_crash_dismissed = true;
                    wake = true;
                }
                if (b == GATEWAY_BUTTON_MENU && !long_press_fired && now - press_started_ms[b] >= BUTTON_LONG_PRESS_MS) {
                    long_press_fired = true;
                    ESP_LOGI(TAG, "MENU held: opening setup portal");
                    (void)network_service_open_portal();
                }
            } else {
                if (b == GATEWAY_BUTTON_MENU) long_press_fired = false;
                press_started_ms[b] = 0;
            }
        }

        if (wake || now - last_refresh_ms >= DISPLAY_REFRESH_MS) {
            last_refresh_ms = now;
            collect(&current, &current_minor);
            const bool changed = first || memcmp(&current, &shown, sizeof(current)) != 0;
            const bool minor_changed = memcmp(&current_minor, &shown_minor, sizeof(current_minor)) != 0;
            if (changed) wake = true;

            const int timeout_s = current.page == PAGE_SETUP ? CONFIG_GATEWAY_DISPLAY_SETUP_TIMEOUT_SECONDS
                                                             : CONFIG_GATEWAY_DISPLAY_TIMEOUT_SECONDS;
            if (wake) {
                last_activity_ms = now;
                if (!on) {
                    /* Move the image a little on every wake to spread pixel wear. */
                    s_shift_x = (s_shift_x + 1) % DISPLAY_SHIFT_MAX;
                    s_shift_y = (s_shift_y + 3) % DISPLAY_SHIFT_MAX;
                    render(&current, &current_minor);
                    panel_power(true);
                    on = true;
                } else {
                    render(&current, &current_minor);
                }
            } else if (on && minor_changed) {
                render(&current, &current_minor);
            }
            if (wake || (on && minor_changed)) {
                gateway_security_wipe(shown.portal_passphrase, sizeof(shown.portal_passphrase));
                shown = current;
                shown_minor = current_minor;
            }
            gateway_security_wipe(current.portal_passphrase, sizeof(current.portal_passphrase));
            first = false;

            if (on && timeout_s > 0 && now - last_activity_ms >= (int64_t)timeout_s * 1000) {
                panel_power(false);
                on = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_POLL_MS));
    }
}

void display_service_network_event(const network_service_snapshot_t *snapshot, void *user_ctx)
{
    (void)user_ctx;
    if (snapshot == NULL) return;
    portENTER_CRITICAL(&s_lock);
    s_net = *snapshot;
    s_net_valid = true;
    portEXIT_CRITICAL(&s_lock);
}

static void sms_event(sms_service_event_t event, const sms_message_t *message, void *user_ctx)
{
    (void)user_ctx;
    if (event != SMS_SERVICE_EVENT_RECEIVED || message == NULL || message->direction != SMS_DIRECTION_INBOUND) return;
    char sender[SMS_MAX_ADDRESS_LENGTH];
    char when[16] = {0};
    char preview[64] = {0};
    utf8_to_ascii(message->sender, sender, sizeof(sender));
    /* "2026-10-07T14:32:11+02:00" -> "07.10. 14:32" */
    const char *ts = message->service_center_timestamp;
    if (strlen(ts) >= 16) snprintf(when, sizeof(when), "%.2s.%.2s. %.5s", ts + 8, ts + 5, ts + 11);
#if CONFIG_GATEWAY_DISPLAY_SMS_PREVIEW
    utf8_to_ascii(message->text, preview, sizeof(preview));
#endif
    portENTER_CRITICAL(&s_lock);
    s_sms_id = message->id;
    memcpy(s_sms_sender, sender, sizeof(s_sms_sender));
    memcpy(s_sms_time, when, sizeof(s_sms_time));
    memcpy(s_sms_preview, preview, sizeof(s_sms_preview));
    portEXIT_CRITICAL(&s_lock);
}

static bool flush_done_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

esp_err_t display_service_init(void)
{
    s_flush_done = xSemaphoreCreateBinary();
    s_band = heap_caps_malloc(LCD_W * BAND_H * sizeof(*s_band), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_band == NULL || s_flush_done == NULL) return ESP_ERR_NO_MEM;

    /* Keep the backlight off until the first frame is in the panel. */
    gpio_config_t bl = {.pin_bit_mask = 1ULL << LCD_BL, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "LCD backlight");
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_BL, 0), TAG, "LCD backlight off");

    const spi_bus_config_t bus = {.sclk_io_num = LCD_CLK, .mosi_io_num = LCD_MOSI, .miso_io_num = GPIO_NUM_NC, .quadwp_io_num = GPIO_NUM_NC, .quadhd_io_num = GPIO_NUM_NC, .max_transfer_sz = LCD_W * BAND_H * 2};
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "LCD SPI");
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC, .cs_gpio_num = LCD_CS, .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 2,
        .on_color_trans_done = flush_done_cb,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io), TAG, "LCD IO");
    const esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel), TAG, "ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "LCD init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "LCD invert");
    render(NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "LCD on");
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_BL, 1), TAG, "LCD backlight on");

    ESP_RETURN_ON_ERROR(modem_core_add_sms_event_callback(sms_event, NULL), TAG, "SMS observer");
    return xTaskCreate(display_task, "display", 4096, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
