#include "gateway_diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#define DIAG_NAMESPACE "gw_diag"
#define DIAG_CRASH_KEY "last_crash"
#define DIAG_RTC_MAGIC 0x53474449U /* "SGDI" */
#define DIAG_STABLE_AFTER_US (60LL * 1000 * 1000)
#define DIAG_SAFE_MODE_CRASHES 3U
#define DIAG_SAFE_MODE_DEEP_CRASHES 6U
#define DIAG_BACKTRACE_ENTRIES 8

static const char *TAG = "diag";

/* Survives software resets and panics, not power cycles. */
static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_crashes;

static esp_reset_reason_t s_reason;
static uint32_t s_crashes_before_boot;
static gateway_safe_mode_t s_safe_mode;
static char s_last_crash[GATEWAY_DIAG_CRASH_TEXT_MAX];
static bool s_crash_fresh;
static esp_timer_handle_t s_stable_timer;

static bool reason_is_abnormal(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_USB:
    case ESP_RST_CPU_LOCKUP:
    case ESP_RST_PWR_GLITCH:
        return true;
    default:
        return false;
    }
}

const char *gateway_diag_reset_reason(void)
{
    switch (s_reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dropped)";
    case ESP_RST_USB: return "reset by USB host (DTR/RTS)";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    default: return "unknown";
    }
}

static void store_crash_text(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(DIAG_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (s_last_crash[0] != '\0') (void)nvs_set_str(nvs, DIAG_CRASH_KEY, s_last_crash);
    else (void)nvs_erase_key(nvs, DIAG_CRASH_KEY);
    (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_crash_text(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(DIAG_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t len = sizeof(s_last_crash);
    if (nvs_get_str(nvs, DIAG_CRASH_KEY, s_last_crash, &len) != ESP_OK) s_last_crash[0] = '\0';
    nvs_close(nvs);
}

/* Turns a core dump left by the previous boot into a one-line summary. */
static bool summarize_core_dump(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (esp_core_dump_image_check() != ESP_OK) return false;
    esp_core_dump_summary_t *summary = calloc(1, sizeof(*summary));
    if (summary == NULL) return false;
    bool ok = false;
    if (esp_core_dump_get_summary(summary) == ESP_OK) {
        char panic[49] = {0};
        (void)esp_core_dump_get_panic_reason(panic, sizeof(panic));
        /* Compact on purpose: it has to fit the 240x240 display (~170 characters). */
        int n = snprintf(s_last_crash, sizeof(s_last_crash), "%s%stask %.15s PC %08lx BT",
                         panic[0] ? panic : "", panic[0] ? " | " : "",
                         summary->exc_task, (unsigned long)summary->exc_pc);
        const uint32_t depth = summary->exc_bt_info.depth < DIAG_BACKTRACE_ENTRIES
                                   ? summary->exc_bt_info.depth : DIAG_BACKTRACE_ENTRIES;
        for (uint32_t i = 0; i < depth && n > 0 && (size_t)n < sizeof(s_last_crash); ++i) {
            n += snprintf(s_last_crash + n, sizeof(s_last_crash) - (size_t)n, " %08lx",
                          (unsigned long)summary->exc_bt_info.bt[i]);
        }
        ok = true;
    }
    free(summary);
    (void)esp_core_dump_image_erase();
    return ok;
#else
    return false;
#endif
}

static void stable_cb(void *arg)
{
    (void)arg;
    s_rtc_crashes = 0;
    ESP_LOGI(TAG, "running stable; crash counter cleared");
}

esp_err_t gateway_diag_init(void)
{
    s_reason = esp_reset_reason();
    if (s_rtc_magic != DIAG_RTC_MAGIC || s_reason == ESP_RST_POWERON) {
        s_rtc_magic = DIAG_RTC_MAGIC;
        s_rtc_crashes = 0;
    }
    if (reason_is_abnormal(s_reason)) ++s_rtc_crashes;
    else s_rtc_crashes = 0;
    s_crashes_before_boot = s_rtc_crashes;

    load_crash_text();
    if (summarize_core_dump()) {
        s_crash_fresh = reason_is_abnormal(s_reason);
        store_crash_text();
    } else if (s_reason == ESP_RST_BROWNOUT || s_reason == ESP_RST_USB || s_reason == ESP_RST_TASK_WDT ||
               s_reason == ESP_RST_INT_WDT || s_reason == ESP_RST_WDT) {
        snprintf(s_last_crash, sizeof(s_last_crash), "%s", gateway_diag_reset_reason());
        s_crash_fresh = true;
        store_crash_text();
    }

    if (s_crashes_before_boot >= DIAG_SAFE_MODE_DEEP_CRASHES) s_safe_mode = GATEWAY_SAFE_MODE_NO_MODEM_NO_DISPLAY;
    else if (s_crashes_before_boot >= DIAG_SAFE_MODE_CRASHES) s_safe_mode = GATEWAY_SAFE_MODE_NO_MODEM;

    ESP_LOGI(TAG, "reset reason: %s, consecutive abnormal resets: %lu",
             gateway_diag_reset_reason(), (unsigned long)s_crashes_before_boot);
    if (s_last_crash[0] != '\0') {
        ESP_LOGW(TAG, "%s crash: %s", s_crash_fresh ? "previous boot ended with" : "last recorded", s_last_crash);
    }
    if (s_safe_mode != GATEWAY_SAFE_MODE_OFF) {
        ESP_LOGE(TAG, "SAFE MODE %d after %lu crashes: modem%s disabled for this boot",
                 (int)s_safe_mode, (unsigned long)s_crashes_before_boot,
                 s_safe_mode == GATEWAY_SAFE_MODE_NO_MODEM_NO_DISPLAY ? " and display" : "");
    }

    const esp_timer_create_args_t args = {.callback = stable_cb, .name = "diag_stable"};
    if (esp_timer_create(&args, &s_stable_timer) == ESP_OK) {
        (void)esp_timer_start_once(s_stable_timer, DIAG_STABLE_AFTER_US);
    }
    return ESP_OK;
}

gateway_safe_mode_t gateway_diag_safe_mode(void)
{
    return s_safe_mode;
}

uint32_t gateway_diag_crash_count(void)
{
    return s_crashes_before_boot;
}

const char *gateway_diag_last_crash(bool *fresh)
{
    if (fresh != NULL) *fresh = s_crash_fresh;
    return s_last_crash;
}

void gateway_diag_clear_crash(void)
{
    s_last_crash[0] = '\0';
    s_crash_fresh = false;
    store_crash_text();
}
