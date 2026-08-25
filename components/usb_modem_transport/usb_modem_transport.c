#include "sdkconfig.h"
#include "usb_modem_transport.h"
#include "usb_modem_descriptor.h"
#include "huawei_mode_switch.h"

#include <string.h>
#include <inttypes.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

#define MODEM_USB_MAX_CANDIDATES 4
#define MODEM_USB_QUEUE_DEPTH 8
#define MODEM_USB_HOST_TASK_STACK 4096
#define MODEM_USB_WORKER_TASK_STACK 6144
#define MODEM_USB_SWITCH_TASK_STACK 4096
#define MODEM_USB_SWITCH_QUEUE_DEPTH 6
#define MODEM_USB_SWITCH_TIMEOUT_MS 2500
#define MODEM_USB_HOST_TASK_PRIORITY 18
#define MODEM_USB_CDC_TASK_PRIORITY 19
#define MODEM_USB_WORKER_TASK_PRIORITY 12

#define MODEM_USB_EVENT_BIT_AT_OK BIT0
#define MODEM_USB_EVENT_BIT_DEVICE_GONE BIT1
#define MODEM_USB_EVENT_BIT_TRANSPORT_ERROR BIT2

static const char *TAG = "modem_usb";

typedef enum {
    WORK_EVENT_HOST_READY = 0,
    WORK_EVENT_DEVICE_FOUND,
    WORK_EVENT_DISCONNECTED,
    WORK_EVENT_ERROR,
} work_event_id_t;

typedef struct {
    work_event_id_t id;
    union {
        struct {
            uint16_t vid;
            uint16_t pid;
            uint8_t count;
            modem_usb_candidate_t candidates[MODEM_USB_MAX_CANDIDATES];
        } device;
        struct {
            cdc_acm_dev_hdl_t handle;
        } disconnected;
        struct {
            esp_err_t error;
            uint32_t generation;
        } error;
    } data;
} work_event_t;

typedef struct {
    bool started;
    bool probing;
    cdc_acm_dev_hdl_t cdc_handle;
    QueueHandle_t work_queue;
    EventGroupHandle_t probe_events;
    SemaphoreHandle_t io_mutex;
    TaskHandle_t worker_task;
    TaskHandle_t host_task;
    TaskHandle_t mode_switch_task;
    modem_usb_event_callback_t event_cb;
    void *event_user_ctx;
    modem_usb_rx_callback_t rx_cb;
    void *rx_user_ctx;
    modem_usb_diagnostics_t diagnostics;
    char probe_line[32];
    size_t probe_line_len;
    uint32_t next_generation;
    uint32_t active_generation;
    work_event_t last_device_event;
    bool last_device_valid;
} modem_usb_state_t;

static modem_usb_state_t s_usb;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void diagnostics_copy(modem_usb_diagnostics_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_usb.diagnostics;
    portEXIT_CRITICAL(&s_lock);
}

static void diagnostics_set_error(esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_usb.diagnostics.last_error = error;
    portEXIT_CRITICAL(&s_lock);
}

static void emit_event(modem_usb_event_t event)
{
    if (s_usb.event_cb == NULL) {
        return;
    }

    modem_usb_diagnostics_t snapshot;
    diagnostics_copy(&snapshot);
    s_usb.event_cb(event, &snapshot, s_usb.event_user_ctx);
}

static void scan_candidate_interfaces(const usb_config_desc_t *config, work_event_t *event)
{
    event->data.device.count = (uint8_t)modem_usb_find_serial_candidates(
        (const uint8_t *)config, config->wTotalLength,
        event->data.device.candidates, MODEM_USB_MAX_CANDIDATES);
}

typedef struct {
    QueueHandle_t new_devices;
} mode_switch_client_ctx_t;

typedef struct {
    volatile bool done;
    usb_transfer_status_t status;
} mode_switch_transfer_ctx_t;

static void mode_switch_transfer_cb(usb_transfer_t *transfer)
{
    mode_switch_transfer_ctx_t *ctx = (mode_switch_transfer_ctx_t *)transfer->context;
    if (ctx != NULL) {
        ctx->status = transfer->status;
        ctx->done = true;
    }
}

static void mode_switch_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    mode_switch_client_ctx_t *ctx = (mode_switch_client_ctx_t *)arg;
    if (ctx == NULL || ctx->new_devices == NULL || event_msg == NULL) return;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        const uint8_t address = event_msg->new_dev.address;
        (void)xQueueSend(ctx->new_devices, &address, 0);
    }
}

static bool find_mass_storage_bulk_out(const usb_config_desc_t *config, uint8_t *interface_number, uint8_t *endpoint)
{
    if (config == NULL || interface_number == NULL || endpoint == NULL) return false;
    const uint8_t *raw = (const uint8_t *)config;
    size_t offset = 0;
    bool mass_storage = false;
    uint8_t current_interface = 0;
    while (offset + 2U <= config->wTotalLength) {
        const uint8_t len = raw[offset];
        const uint8_t type = raw[offset + 1U];
        if (len < 2U || offset + len > config->wTotalLength) break;
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= sizeof(usb_intf_desc_t)) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)(raw + offset);
            current_interface = intf->bInterfaceNumber;
            mass_storage = intf->bAlternateSetting == 0 && intf->bInterfaceClass == USB_CLASS_MASS_STORAGE;
        } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && mass_storage && len >= sizeof(usb_ep_desc_t)) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)(raw + offset);
            if ((ep->bmAttributes & 0x03U) == USB_TRANSFER_TYPE_BULK && (ep->bEndpointAddress & 0x80U) == 0U) {
                *interface_number = current_interface;
                *endpoint = ep->bEndpointAddress;
                return true;
            }
        }
        offset += len;
    }
    return false;
}

static void mode_switch_count(bool success)
{
    portENTER_CRITICAL(&s_lock);
    ++s_usb.diagnostics.mode_switch_attempts;
    if (success) ++s_usb.diagnostics.mode_switch_successes;
    else ++s_usb.diagnostics.mode_switch_failures;
    portEXIT_CRITICAL(&s_lock);
}

static void process_mode_switch_device(usb_host_client_handle_t client, uint8_t address)
{
    usb_device_handle_t dev = NULL;
    if (usb_host_device_open(client, address, &dev) != ESP_OK || dev == NULL) return;

    const usb_device_desc_t *device_desc = NULL;
    const usb_config_desc_t *config_desc = NULL;
    if (usb_host_get_device_descriptor(dev, &device_desc) != ESP_OK || device_desc == NULL ||
        device_desc->idVendor != HUAWEI_USB_VID ||
        !huawei_mode_switch_pid_supported(device_desc->idProduct, CONFIG_GATEWAY_MODEM_USB_SWITCH_PID_EXTRA)) {
        (void)usb_host_device_close(client, dev);
        return;
    }
    if (usb_host_get_active_config_descriptor(dev, &config_desc) != ESP_OK || config_desc == NULL) {
        mode_switch_count(false);
        (void)usb_host_device_close(client, dev);
        return;
    }
    uint8_t interface_number = 0;
    uint8_t bulk_out = 0;
    if (!find_mass_storage_bulk_out(config_desc, &interface_number, &bulk_out)) {
        ESP_LOGE(TAG, "Huawei pre-switch device %04x has no mass-storage bulk OUT endpoint", (unsigned)device_desc->idProduct);
        mode_switch_count(false);
        (void)usb_host_device_close(client, dev);
        return;
    }
    if (usb_host_interface_claim(client, dev, interface_number, 0) != ESP_OK) {
        mode_switch_count(false);
        (void)usb_host_device_close(client, dev);
        return;
    }

    usb_transfer_t *transfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(HUAWEI_MODE_SWITCH_MESSAGE_LEN, 0, &transfer);
    if (err != ESP_OK || transfer == NULL) {
        mode_switch_count(false);
        (void)usb_host_interface_release(client, dev, interface_number);
        (void)usb_host_device_close(client, dev);
        return;
    }
    mode_switch_transfer_ctx_t tx = {.done = false, .status = USB_TRANSFER_STATUS_ERROR};
    (void)huawei_mode_switch_message(transfer->data_buffer, transfer->data_buffer_size);
    transfer->num_bytes = HUAWEI_MODE_SWITCH_MESSAGE_LEN;
    transfer->device_handle = dev;
    transfer->bEndpointAddress = bulk_out;
    transfer->timeout_ms = MODEM_USB_SWITCH_TIMEOUT_MS;
    transfer->callback = mode_switch_transfer_cb;
    transfer->context = &tx;

    ESP_LOGI(TAG, "switching Huawei %04x:%04x from storage mode on interface=%u ep=0x%02x",
             HUAWEI_USB_VID, (unsigned)device_desc->idProduct, (unsigned)interface_number, (unsigned)bulk_out);
    err = usb_host_transfer_submit(transfer);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(MODEM_USB_SWITCH_TIMEOUT_MS + 500U);
    while (err == ESP_OK && !tx.done && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        (void)usb_host_client_handle_events(client, pdMS_TO_TICKS(50));
    }
    if (err == ESP_OK && !tx.done) {
        (void)usb_host_endpoint_halt(dev, bulk_out);
        (void)usb_host_endpoint_flush(dev, bulk_out);
        const TickType_t flush_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
        while (!tx.done && (int32_t)(flush_deadline - xTaskGetTickCount()) > 0) {
            (void)usb_host_client_handle_events(client, pdMS_TO_TICKS(25));
        }
    }
    if (!tx.done) {
        ESP_LOGE(TAG, "Huawei mode-switch transfer could not be retired safely; restarting gateway");
        mode_switch_count(false);
        esp_restart();
        for (;;) vTaskDelay(portMAX_DELAY); /* Defensive: never free an in-flight transfer. */
    }

    const bool success = err == ESP_OK &&
        (tx.status == USB_TRANSFER_STATUS_COMPLETED || tx.status == USB_TRANSFER_STATUS_NO_DEVICE || tx.status == USB_TRANSFER_STATUS_CANCELED);
    mode_switch_count(success);
    (void)usb_host_transfer_free(transfer);
    (void)usb_host_interface_release(client, dev, interface_number);
    (void)usb_host_device_close(client, dev);
    if (!success) ESP_LOGW(TAG, "Huawei mode switch transfer status=%d", (int)tx.status);
}

static void mode_switch_task(void *arg)
{
    TaskHandle_t parent = (TaskHandle_t)arg;
    mode_switch_client_ctx_t ctx = {.new_devices = xQueueCreate(MODEM_USB_SWITCH_QUEUE_DEPTH, sizeof(uint8_t))};
    if (ctx.new_devices == NULL) {
        if (parent != NULL) (void)xTaskNotify(parent, 2U, eSetValueWithOverwrite);
        vTaskDelete(NULL);
        return;
    }
    usb_host_client_handle_t client = NULL;
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 6,
        .async = {.client_event_callback = mode_switch_client_event_cb, .callback_arg = &ctx},
    };
    if (usb_host_client_register(&cfg, &client) != ESP_OK) {
        if (parent != NULL) (void)xTaskNotify(parent, 2U, eSetValueWithOverwrite);
        vQueueDelete(ctx.new_devices);
        vTaskDelete(NULL);
        return;
    }
    if (parent != NULL) (void)xTaskNotify(parent, 1U, eSetValueWithOverwrite);
    for (;;) {
        (void)usb_host_client_handle_events(client, pdMS_TO_TICKS(100));
        uint8_t address = 0;
        while (xQueueReceive(ctx.new_devices, &address, 0) == pdTRUE) {
            process_mode_switch_device(client, address);
        }
    }
}

static void new_usb_device_callback(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *device_desc = NULL;
    const usb_config_desc_t *config_desc = NULL;

    if (usb_host_get_device_descriptor(usb_dev, &device_desc) != ESP_OK || device_desc == NULL) {
        ESP_LOGW(TAG, "unable to read newly connected USB device descriptor");
        return;
    }

    if (device_desc->idVendor != CONFIG_GATEWAY_MODEM_USB_VID ||
        device_desc->idProduct != CONFIG_GATEWAY_MODEM_USB_PID) {
        ESP_LOGD(TAG, "ignoring USB device %04x:%04x",
                 (unsigned)device_desc->idVendor, (unsigned)device_desc->idProduct);
        return;
    }

    if (usb_host_get_active_config_descriptor(usb_dev, &config_desc) != ESP_OK || config_desc == NULL) {
        ESP_LOGE(TAG, "unable to read Huawei active configuration descriptor");
        return;
    }

    work_event_t event = {
        .id = WORK_EVENT_DEVICE_FOUND,
        .data.device = {
            .vid = device_desc->idVendor,
            .pid = device_desc->idProduct,
            .count = 0,
        },
    };
    scan_candidate_interfaces(config_desc, &event);

    ESP_LOGI(TAG, "Huawei modem %04x:%04x connected; %u serial candidate(s)",
             (unsigned)event.data.device.vid, (unsigned)event.data.device.pid,
             (unsigned)event.data.device.count);
    for (uint8_t i = 0; i < event.data.device.count; ++i) {
        const modem_usb_candidate_t *candidate = &event.data.device.candidates[i];
        ESP_LOGI(TAG,
                 "candidate[%u]: if=%u class=%02x/%02x proto=%02x "
                 "bulk-in=0x%02x bulk-out=0x%02x intr-in=0x%02x score=%u",
                 (unsigned)i, (unsigned)candidate->interface_number,
                 (unsigned)candidate->interface_class, (unsigned)candidate->interface_subclass,
                 (unsigned)candidate->interface_protocol, (unsigned)candidate->bulk_in_ep,
                 (unsigned)candidate->bulk_out_ep, (unsigned)candidate->interrupt_in_ep,
                 (unsigned)candidate->score);
    }

    if (s_usb.work_queue != NULL && xQueueSend(s_usb.work_queue, &event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "USB work queue full while reporting modem connection");
    }
}

static void reset_probe_parser(void)
{
    portENTER_CRITICAL(&s_lock);
    s_usb.probe_line_len = 0;
    s_usb.probe_line[0] = '\0';
    s_usb.probing = true;
    portEXIT_CRITICAL(&s_lock);
    xEventGroupClearBits(s_usb.probe_events, MODEM_USB_EVENT_BIT_AT_OK);
}

static void stop_probe_parser(void)
{
    portENTER_CRITICAL(&s_lock);
    s_usb.probing = false;
    s_usb.probe_line_len = 0;
    portEXIT_CRITICAL(&s_lock);
}

static bool cdc_rx_callback(const uint8_t *data, size_t data_len, void *user_arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)user_arg;
    portENTER_CRITICAL(&s_lock);
    const bool current_generation = generation != 0 && generation == s_usb.active_generation;
    portEXIT_CRITICAL(&s_lock);
    if (!current_generation) return true;
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_DEBUG);

    bool found_ok = false;
    portENTER_CRITICAL(&s_lock);
    if (s_usb.probing) {
        for (size_t i = 0; i < data_len; ++i) {
            const char ch = (char)data[i];
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                s_usb.probe_line[s_usb.probe_line_len] = '\0';
                if (strcmp(s_usb.probe_line, "OK") == 0) {
                    found_ok = true;
                }
                s_usb.probe_line_len = 0;
                continue;
            }
            if (s_usb.probe_line_len + 1 < sizeof(s_usb.probe_line)) {
                s_usb.probe_line[s_usb.probe_line_len++] = ch;
            } else {
                s_usb.probe_line_len = 0;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (found_ok) {
        xEventGroupSetBits(s_usb.probe_events, MODEM_USB_EVENT_BIT_AT_OK);
    }

    modem_usb_rx_callback_t rx_cb;
    void *rx_user_ctx;
    bool forward_rx;
    portENTER_CRITICAL(&s_lock);
    rx_cb = s_usb.rx_cb;
    rx_user_ctx = s_usb.rx_user_ctx;
    forward_rx = !s_usb.probing && s_usb.diagnostics.at_ready;
    portEXIT_CRITICAL(&s_lock);
    if (forward_rx && rx_cb != NULL) {
        rx_cb(data, data_len, rx_user_ctx);
    }
    return true;
}

static void cdc_device_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    const uint32_t generation = (uint32_t)(uintptr_t)user_ctx;
    if (event == NULL) return;
    portENTER_CRITICAL(&s_lock);
    const bool current_generation = generation != 0 && generation == s_usb.active_generation;
    portEXIT_CRITICAL(&s_lock);
    if (!current_generation) {
        ESP_LOGD(TAG, "ignoring stale CDC callback generation=%" PRIu32, generation);
        return;
    }

    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
        portENTER_CRITICAL(&s_lock);
        s_usb.diagnostics.at_ready = false;
        s_usb.diagnostics.device_present = false;
        portEXIT_CRITICAL(&s_lock);
        if (s_usb.probe_events != NULL) {
            xEventGroupSetBits(s_usb.probe_events, MODEM_USB_EVENT_BIT_DEVICE_GONE);
        }

        work_event_t work = {
            .id = WORK_EVENT_DISCONNECTED,
            .data.disconnected.handle = event->data.cdc_hdl,
        };
        if (s_usb.work_queue != NULL && xQueueSend(s_usb.work_queue, &work, 0) != pdTRUE) {
            ESP_LOGE(TAG, "USB work queue full while reporting disconnect");
        }
        break;
    }
    case CDC_ACM_HOST_ERROR: {
        ESP_LOGE(TAG, "CDC modem transport error=%d", event->data.error);
        portENTER_CRITICAL(&s_lock);
        s_usb.diagnostics.at_ready = false;
        s_usb.diagnostics.last_error = ESP_FAIL;
        portEXIT_CRITICAL(&s_lock);
        if (s_usb.probe_events != NULL) xEventGroupSetBits(s_usb.probe_events, MODEM_USB_EVENT_BIT_TRANSPORT_ERROR);
        work_event_t work = {.id = WORK_EVENT_ERROR, .data = {.error = {.error = ESP_FAIL, .generation = generation}}};
        if (s_usb.work_queue != NULL && xQueueSend(s_usb.work_queue, &work, 0) != pdTRUE) {
            ESP_LOGE(TAG, "USB work queue full while reporting CDC error");
        }
        break;
    }
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(TAG, "CDC serial state=0x%04x", (unsigned)event->data.serial_state.val);
        break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
        ESP_LOGD(TAG, "CDC network notification=%d", event->data.network_connected);
        break;
    default:
        ESP_LOGD(TAG, "CDC event=%d", event->type);
        break;
    }
}

static esp_err_t write_open_handle(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(data != NULL && len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid TX buffer");
    ESP_RETURN_ON_FALSE(s_usb.io_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "transport not initialized");

    if (xSemaphoreTake(s_usb.io_mutex, pdMS_TO_TICKS(timeout_ms + 100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    cdc_acm_dev_hdl_t handle = s_usb.cdc_handle;
    esp_err_t err = handle != NULL
                        ? cdc_acm_host_data_tx_blocking(handle, data, len, timeout_ms)
                        : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_usb.io_mutex);
    return err;
}

static void close_current_handle(cdc_acm_dev_hdl_t expected_handle)
{
    if (s_usb.io_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_usb.io_mutex, portMAX_DELAY);
    if (s_usb.cdc_handle != NULL && (expected_handle == NULL || expected_handle == s_usb.cdc_handle)) {
        cdc_acm_dev_hdl_t handle = s_usb.cdc_handle;
        s_usb.cdc_handle = NULL;
        portENTER_CRITICAL(&s_lock); s_usb.active_generation = 0; portEXIT_CRITICAL(&s_lock);
        esp_err_t err = cdc_acm_host_close(handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CDC close returned %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_usb.io_mutex);
}

static bool probe_candidate(const modem_usb_candidate_t *candidate)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t generation = ++s_usb.next_generation;
    if (generation == 0) generation = ++s_usb.next_generation;
    s_usb.active_generation = generation;
    portEXIT_CRITICAL(&s_lock);
    cdc_acm_host_device_config_t config = {
        .connection_timeout_ms = CONFIG_GATEWAY_MODEM_USB_OPEN_TIMEOUT_MS,
        .out_buffer_size = CONFIG_GATEWAY_MODEM_USB_TX_BUFFER_SIZE,
        .in_buffer_size = CONFIG_GATEWAY_MODEM_USB_RX_BUFFER_SIZE,
        .event_cb = cdc_device_event_callback,
        .data_cb = cdc_rx_callback,
        .user_arg = (void *)(uintptr_t)generation,
    };

    cdc_acm_dev_hdl_t handle = NULL;
    ESP_LOGI(TAG, "opening Huawei interface %u for AT probe", (unsigned)candidate->interface_number);
    esp_err_t err = cdc_acm_host_open(CONFIG_GATEWAY_MODEM_USB_VID,
                                      CONFIG_GATEWAY_MODEM_USB_PID,
                                      candidate->interface_number,
                                      &config,
                                      &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "interface %u open failed: %s",
                 (unsigned)candidate->interface_number, esp_err_to_name(err));
        portENTER_CRITICAL(&s_lock);
        s_usb.diagnostics.open_failures++;
        s_usb.diagnostics.last_error = err;
        if (s_usb.active_generation == generation) s_usb.active_generation = 0;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }

    xSemaphoreTake(s_usb.io_mutex, portMAX_DELAY);
    s_usb.cdc_handle = handle;
    xSemaphoreGive(s_usb.io_mutex);

    for (int attempt = 1; attempt <= CONFIG_GATEWAY_MODEM_USB_PROBE_RETRIES; ++attempt) {
        portENTER_CRITICAL(&s_lock);
        s_usb.diagnostics.probe_attempts++;
        portEXIT_CRITICAL(&s_lock);

        reset_probe_parser();
        ESP_LOGI(TAG, "AT probe interface=%u attempt=%d/%d",
                 (unsigned)candidate->interface_number, attempt, CONFIG_GATEWAY_MODEM_USB_PROBE_RETRIES);
        static const uint8_t at_command[] = "AT\r";
        err = write_open_handle(at_command, sizeof(at_command) - 1, CONFIG_GATEWAY_MODEM_USB_PROBE_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AT transmit failed on interface %u: %s",
                     (unsigned)candidate->interface_number, esp_err_to_name(err));
            diagnostics_set_error(err);
            const EventBits_t state_bits = xEventGroupGetBits(s_usb.probe_events);
            if ((state_bits & MODEM_USB_EVENT_BIT_DEVICE_GONE) != 0) {
                stop_probe_parser();
                close_current_handle(handle);
                return false;
            }
            continue;
        }

        const EventBits_t bits = xEventGroupWaitBits(s_usb.probe_events,
                                                     MODEM_USB_EVENT_BIT_AT_OK | MODEM_USB_EVENT_BIT_DEVICE_GONE | MODEM_USB_EVENT_BIT_TRANSPORT_ERROR,
                                                     pdTRUE,
                                                     pdFALSE,
                                                     pdMS_TO_TICKS(CONFIG_GATEWAY_MODEM_USB_PROBE_TIMEOUT_MS));
        if ((bits & MODEM_USB_EVENT_BIT_DEVICE_GONE) != 0) {
            ESP_LOGW(TAG, "modem disappeared during AT probe");
            stop_probe_parser();
            close_current_handle(handle);
            return false;
        }
        if ((bits & MODEM_USB_EVENT_BIT_TRANSPORT_ERROR) != 0) {
            ESP_LOGW(TAG, "CDC transport failed during AT probe");
            stop_probe_parser();
            close_current_handle(handle);
            return false;
        }
        if ((bits & MODEM_USB_EVENT_BIT_AT_OK) != 0) {
            stop_probe_parser();
            ESP_LOGI(TAG, "AT probe succeeded on interface %u", (unsigned)candidate->interface_number);
            cdc_acm_host_desc_print(handle);
            return true;
        }
        ESP_LOGW(TAG, "no OK response from interface %u", (unsigned)candidate->interface_number);
        diagnostics_set_error(ESP_ERR_TIMEOUT);
    }

    stop_probe_parser();
    close_current_handle(handle);
    return false;
}

static void update_selected_candidate(const work_event_t *event, const modem_usb_candidate_t *candidate)
{
    portENTER_CRITICAL(&s_lock);
    s_usb.diagnostics.device_present = true;
    s_usb.diagnostics.at_ready = true;
    s_usb.diagnostics.vid = event->data.device.vid;
    s_usb.diagnostics.pid = event->data.device.pid;
    s_usb.diagnostics.interface_number = candidate->interface_number;
    s_usb.diagnostics.interface_class = candidate->interface_class;
    s_usb.diagnostics.interface_subclass = candidate->interface_subclass;
    s_usb.diagnostics.interface_protocol = candidate->interface_protocol;
    s_usb.diagnostics.bulk_in_ep = candidate->bulk_in_ep;
    s_usb.diagnostics.bulk_out_ep = candidate->bulk_out_ep;
    s_usb.diagnostics.interrupt_in_ep = candidate->interrupt_in_ep;
    s_usb.diagnostics.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
}

static void update_probe_candidate(const work_event_t *event, const modem_usb_candidate_t *candidate)
{
    portENTER_CRITICAL(&s_lock);
    s_usb.diagnostics.vid = event->data.device.vid;
    s_usb.diagnostics.pid = event->data.device.pid;
    s_usb.diagnostics.interface_number = candidate->interface_number;
    s_usb.diagnostics.interface_class = candidate->interface_class;
    s_usb.diagnostics.interface_subclass = candidate->interface_subclass;
    s_usb.diagnostics.interface_protocol = candidate->interface_protocol;
    s_usb.diagnostics.bulk_in_ep = candidate->bulk_in_ep;
    s_usb.diagnostics.bulk_out_ep = candidate->bulk_out_ep;
    s_usb.diagnostics.interrupt_in_ep = candidate->interrupt_in_ep;
    portEXIT_CRITICAL(&s_lock);
}

static void handle_device_found(const work_event_t *event, bool new_connection)
{
    xEventGroupClearBits(s_usb.probe_events, MODEM_USB_EVENT_BIT_DEVICE_GONE);

    portENTER_CRITICAL(&s_lock);
    s_usb.diagnostics.device_present = true;
    s_usb.diagnostics.at_ready = false;
    s_usb.diagnostics.vid = event->data.device.vid;
    s_usb.diagnostics.pid = event->data.device.pid;
    if (new_connection) s_usb.diagnostics.connect_count++;
    s_usb.last_device_event = *event;
    s_usb.last_device_valid = true;
    portEXIT_CRITICAL(&s_lock);
    if (new_connection) emit_event(MODEM_USB_EVENT_DEVICE_FOUND);

    if (event->data.device.count == 0) {
        ESP_LOGE(TAG, "Huawei device has no eligible vendor-specific bulk serial interface");
        diagnostics_set_error(ESP_ERR_NOT_FOUND);
        emit_event(MODEM_USB_EVENT_ERROR);
        return;
    }

    for (uint8_t i = 0; i < event->data.device.count; ++i) {
        const modem_usb_candidate_t *candidate = &event->data.device.candidates[i];
        update_probe_candidate(event, candidate);
        emit_event(MODEM_USB_EVENT_PROBING);
        if (probe_candidate(candidate)) {
            update_selected_candidate(event, candidate);
            emit_event(MODEM_USB_EVENT_AT_READY);
            return;
        }

        portENTER_CRITICAL(&s_lock);
        const bool still_present = s_usb.diagnostics.device_present;
        portEXIT_CRITICAL(&s_lock);
        if (!still_present) {
            return;
        }
    }

    ESP_LOGE(TAG, "none of the Huawei serial candidates answered AT");
    diagnostics_set_error(ESP_ERR_NOT_FOUND);
    emit_event(MODEM_USB_EVENT_ERROR);
}

static void handle_disconnect(cdc_acm_dev_hdl_t disconnected_handle)
{
    ESP_LOGW(TAG, "Huawei modem disconnected");
    stop_probe_parser();
    close_current_handle(disconnected_handle);

    portENTER_CRITICAL(&s_lock);
    s_usb.diagnostics.device_present = false;
    s_usb.diagnostics.at_ready = false;
    s_usb.diagnostics.disconnect_count++;
    s_usb.diagnostics.interface_number = 0;
    s_usb.diagnostics.interface_class = 0;
    s_usb.diagnostics.interface_subclass = 0;
    s_usb.diagnostics.interface_protocol = 0;
    s_usb.diagnostics.bulk_in_ep = 0;
    s_usb.diagnostics.bulk_out_ep = 0;
    s_usb.diagnostics.interrupt_in_ep = 0;
    s_usb.last_device_valid = false;
    s_usb.active_generation = 0;
    portEXIT_CRITICAL(&s_lock);
    emit_event(MODEM_USB_EVENT_DISCONNECTED);
}

static void usb_host_library_task(void *arg)
{
    (void)arg;

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        work_event_t event = {.id = WORK_EVENT_ERROR, .data = {.error = {.error = err}}};
        xQueueSend(s_usb.work_queue, &event, 0);
        vTaskDelete(NULL);
        return;
    }

    /* Register the raw Huawei mode-switch client before CDC. Cold-attached modems can
     * enumerate immediately after usb_host_install(), so wait until this client is
     * actually registered before installing the CDC client. */
    if (xTaskCreate(mode_switch_task, "huawei_switch", MODEM_USB_SWITCH_TASK_STACK, xTaskGetCurrentTaskHandle(),
                    MODEM_USB_WORKER_TASK_PRIORITY + 1, &s_usb.mode_switch_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create Huawei mode-switch client task");
    } else {
        uint32_t switch_ready = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &switch_ready, pdMS_TO_TICKS(2000)) != pdTRUE || switch_ready != 1U) {
            ESP_LOGW(TAG, "Huawei mode-switch USB client did not become ready");
        }
    }

    const cdc_acm_host_driver_config_t cdc_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = MODEM_USB_CDC_TASK_PRIORITY,
        .xCoreID = tskNO_AFFINITY,
        .new_dev_cb = new_usb_device_callback,
    };
    err = cdc_acm_host_install(&cdc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
        /* The mode-switch client is already registered. Keep the USB Host library
         * alive instead of attempting an invalid uninstall with a live client; the
         * diagnostics path reports the fatal CDC startup failure. */
        work_event_t event = {.id = WORK_EVENT_ERROR, .data = {.error = {.error = err}}};
        xQueueSend(s_usb.work_queue, &event, 0);
        goto host_event_loop;
    }

    work_event_t ready = {.id = WORK_EVENT_HOST_READY};
    xQueueSend(s_usb.work_queue, &ready, portMAX_DELAY);

host_event_loop:
    while (true) {
        uint32_t event_flags = 0;
        err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB host event loop failed: %s", esp_err_to_name(err));
            diagnostics_set_error(err);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void modem_usb_worker_task(void *arg)
{
    (void)arg;
    work_event_t event;

    while (xQueueReceive(s_usb.work_queue, &event, portMAX_DELAY) == pdTRUE) {
        switch (event.id) {
        case WORK_EVENT_HOST_READY:
            portENTER_CRITICAL(&s_lock);
            s_usb.diagnostics.host_ready = true;
            s_usb.diagnostics.last_error = ESP_OK;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "USB Host and CDC modem driver ready");
            emit_event(MODEM_USB_EVENT_HOST_READY);
            break;
        case WORK_EVENT_DEVICE_FOUND:
            handle_device_found(&event, true);
            break;
        case WORK_EVENT_DISCONNECTED:
            handle_disconnect(event.data.disconnected.handle);
            break;
        case WORK_EVENT_ERROR: {
            diagnostics_set_error(event.data.error.error);
            const uint32_t generation = event.data.error.generation;
            bool retry = false;
            work_event_t saved = {0};
            portENTER_CRITICAL(&s_lock);
            if (generation != 0 && generation == s_usb.active_generation && s_usb.last_device_valid) {
                retry = true;
                saved = s_usb.last_device_event;
            }
            portEXIT_CRITICAL(&s_lock);
            if (generation != 0) close_current_handle(NULL);
            emit_event(MODEM_USB_EVENT_ERROR);
            if (retry) {
                vTaskDelay(pdMS_TO_TICKS(250));
                ESP_LOGW(TAG, "reprobing Huawei AT interface after CDC transport failure");
                handle_device_found(&saved, false);
            }
            break;
        }
        default:
            ESP_LOGW(TAG, "unknown worker event=%d", event.id);
            break;
        }
    }
}


static void reset_start_state(void)
{
    if (s_usb.host_task != NULL) {
        vTaskDelete(s_usb.host_task);
        s_usb.host_task = NULL;
    }
    if (s_usb.worker_task != NULL) {
        vTaskDelete(s_usb.worker_task);
        s_usb.worker_task = NULL;
    }
    if (s_usb.mode_switch_task != NULL) {
        vTaskDelete(s_usb.mode_switch_task);
        s_usb.mode_switch_task = NULL;
    }
    if (s_usb.io_mutex != NULL) {
        vSemaphoreDelete(s_usb.io_mutex);
        s_usb.io_mutex = NULL;
    }
    if (s_usb.probe_events != NULL) {
        vEventGroupDelete(s_usb.probe_events);
        s_usb.probe_events = NULL;
    }
    if (s_usb.work_queue != NULL) {
        vQueueDelete(s_usb.work_queue);
        s_usb.work_queue = NULL;
    }

    portENTER_CRITICAL(&s_lock);
    s_usb.started = false;
    s_usb.event_cb = NULL;
    s_usb.event_user_ctx = NULL;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t modem_usb_transport_start(modem_usb_event_callback_t event_cb, void *user_ctx)
{
    portENTER_CRITICAL(&s_lock);
    if (s_usb.started) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_usb.started = true;
    s_usb.event_cb = event_cb;
    s_usb.event_user_ctx = user_ctx;
    s_usb.diagnostics.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);

    s_usb.work_queue = xQueueCreate(MODEM_USB_QUEUE_DEPTH, sizeof(work_event_t));
    s_usb.probe_events = xEventGroupCreate();
    s_usb.io_mutex = xSemaphoreCreateMutex();
    if (s_usb.work_queue == NULL || s_usb.probe_events == NULL || s_usb.io_mutex == NULL) {
        reset_start_state();
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreate(modem_usb_worker_task,
                                     "modem_usb_worker",
                                     MODEM_USB_WORKER_TASK_STACK,
                                     NULL,
                                     MODEM_USB_WORKER_TASK_PRIORITY,
                                     &s_usb.worker_task);
    if (created != pdPASS) {
        reset_start_state();
        return ESP_ERR_NO_MEM;
    }

    created = xTaskCreate(usb_host_library_task,
                          "usb_host_lib",
                          MODEM_USB_HOST_TASK_STACK,
                          NULL,
                          MODEM_USB_HOST_TASK_PRIORITY,
                          &s_usb.host_task);
    if (created != pdPASS) {
        reset_start_state();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t modem_usb_transport_set_rx_callback(modem_usb_rx_callback_t rx_cb, void *user_ctx)
{
    portENTER_CRITICAL(&s_lock);
    s_usb.rx_cb = rx_cb;
    s_usb.rx_user_ctx = user_ctx;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t modem_usb_transport_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    portENTER_CRITICAL(&s_lock);
    const bool ready = s_usb.diagnostics.at_ready;
    portEXIT_CRITICAL(&s_lock);
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return write_open_handle(data, len, timeout_ms);
}

bool modem_usb_transport_is_ready(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool ready = s_usb.diagnostics.at_ready;
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

esp_err_t modem_usb_transport_get_diagnostics(modem_usb_diagnostics_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    diagnostics_copy(out);
    return ESP_OK;
}
