#include "at_engine.h"

#include <string.h>

#include "at_parser.h"
#include "at_protocol.h"
#include "at_router.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AT_REQUEST_QUEUE_DEPTH 8
#define AT_RX_QUEUE_DEPTH 20
#define AT_URC_QUEUE_DEPTH 16
#define AT_RX_CHUNK_SIZE 256
#define AT_WORKER_TASK_STACK 7168
#define AT_URC_TASK_STACK 4096
#define AT_WORKER_TASK_PRIORITY 11
#define AT_URC_TASK_PRIORITY 10
#define AT_IDLE_RX_WAIT_MS 20
#define AT_DEFAULT_TIMEOUT_MS 3000
#define AT_DEFAULT_WRITE_TIMEOUT_MS 1000

static const char *TAG = "at_engine";

typedef enum {
    RX_EVENT_DATA = 0,
    RX_EVENT_TRANSPORT_LOST,
    RX_EVENT_CANCEL,
} rx_event_kind_t;

typedef struct {
    rx_event_kind_t kind;
    uint16_t length;
    uint8_t data[AT_RX_CHUNK_SIZE];
} rx_event_t;

typedef struct {
    at_request_t request;
    at_response_t *response;
    SemaphoreHandle_t completion;
} request_message_t;

typedef struct {
    char line[AT_ENGINE_MAX_URC_LENGTH + 1];
} urc_message_t;

typedef struct {
    const at_request_t *request;
    at_response_t *response;
    bool done;
    bool payload_sent;
    at_router_t router;
    at_parser_t *parser;
} transaction_ctx_t;

typedef struct {
    bool initialized;
    at_transport_t transport;
    QueueHandle_t request_queue;
    QueueHandle_t rx_queue;
    QueueHandle_t urc_queue;
    TaskHandle_t worker_task;
    TaskHandle_t urc_task;
    at_urc_callback_t urc_callback;
    void *urc_user_ctx;
    at_engine_diagnostics_t diagnostics;
    uint32_t rx_overflow_epoch;
    uint32_t transport_lost_epoch;
    uint32_t cancel_epoch;
} at_engine_state_t;

static at_engine_state_t s_engine;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void diag_increment(uint32_t *counter)
{
    portENTER_CRITICAL(&s_lock);
    ++(*counter);
    portEXIT_CRITICAL(&s_lock);
}

static uint32_t get_epoch(const uint32_t *epoch)
{
    portENTER_CRITICAL(&s_lock);
    const uint32_t value = *epoch;
    portEXIT_CRITICAL(&s_lock);
    return value;
}

static size_t bounded_strlen(const char *value, size_t maximum)
{
    if (value == NULL) {
        return 0;
    }
    size_t length = 0;
    while (length < maximum && value[length] != '\0') {
        ++length;
    }
    return length;
}

static void response_reset(at_response_t *response)
{
    memset(response, 0, sizeof(*response));
    response->result = AT_RESULT_PROTOCOL_ERROR;
    response->error_code = -1;
}

static void response_capture_final(at_response_t *response, const char *line)
{
    if (response == NULL || line == NULL) {
        return;
    }
    const size_t length = bounded_strlen(line, AT_ENGINE_MAX_FINAL_LENGTH);
    const size_t copy_length = length < AT_ENGINE_MAX_FINAL_LENGTH ? length : AT_ENGINE_MAX_FINAL_LENGTH - 1;
    memcpy(response->final_line, line, copy_length);
    response->final_line[copy_length] = '\0';
    if (length >= AT_ENGINE_MAX_FINAL_LENGTH) {
        response->truncated = true;
    }
}

static void response_capture_line(at_response_t *response, const char *line)
{
    if (response == NULL || line == NULL) {
        return;
    }

    const size_t length = strlen(line);
    if (response->line_count >= AT_ENGINE_MAX_RESPONSE_LINES ||
        response->data_length + length + 1 > sizeof(response->data)) {
        response->truncated = true;
        return;
    }

    response->line_offsets[response->line_count++] = (uint16_t)response->data_length;
    memcpy(response->data + response->data_length, line, length + 1);
    response->data_length += length + 1;
}

const char *at_response_line(const at_response_t *response, size_t index)
{
    if (response == NULL || index >= response->line_count) {
        return NULL;
    }
    const size_t offset = response->line_offsets[index];
    if (offset >= response->data_length || offset >= sizeof(response->data)) {
        return NULL;
    }
    return response->data + offset;
}

static void queue_urc(const char *line)
{
    if (line == NULL || s_engine.urc_queue == NULL) {
        return;
    }

    urc_message_t message = {0};
    const size_t length = bounded_strlen(line, AT_ENGINE_MAX_URC_LENGTH + 1);
    if (length > AT_ENGINE_MAX_URC_LENGTH) {
        diag_increment(&s_engine.diagnostics.urcs_dropped);
        ESP_LOGW(TAG, "dropping overlong URC");
        return;
    }
    memcpy(message.line, line, length + 1);

    if (xQueueSend(s_engine.urc_queue, &message, 0) != pdTRUE) {
        diag_increment(&s_engine.diagnostics.urcs_dropped);
        ESP_LOGW(TAG, "URC queue full; dropping line");
    }
}

static at_result_t map_final_kind(at_final_kind_t kind)
{
    switch (kind) {
    case AT_FINAL_OK: return AT_RESULT_OK;
    case AT_FINAL_ERROR: return AT_RESULT_ERROR;
    case AT_FINAL_CME_ERROR: return AT_RESULT_CME_ERROR;
    case AT_FINAL_CMS_ERROR: return AT_RESULT_CMS_ERROR;
    case AT_FINAL_NONE:
    default: return AT_RESULT_PROTOCOL_ERROR;
    }
}

static void process_token(const at_token_t *token, void *user_ctx)
{
    transaction_ctx_t *transaction = (transaction_ctx_t *)user_ctx;

    if (token->type == AT_TOKEN_LINE_OVERFLOW) {
        diag_increment(&s_engine.diagnostics.parser_overflows);
        if (transaction != NULL) {
            transaction->response->result = AT_RESULT_PROTOCOL_ERROR;
            transaction->response->truncated = true;
            transaction->done = true;
        }
        return;
    }

    if (token->type == AT_TOKEN_PROMPT) {
        if (transaction == NULL || !transaction->request->wait_for_prompt) {
            queue_urc(">");
            return;
        }

        at_parser_set_prompt_enabled(transaction->parser, false);
        const uint32_t write_timeout = s_engine.transport.write_timeout_ms != 0
                                           ? s_engine.transport.write_timeout_ms
                                           : AT_DEFAULT_WRITE_TIMEOUT_MS;
        const esp_err_t err = s_engine.transport.write(
            s_engine.transport.ctx,
            transaction->request->prompt_payload,
            transaction->request->prompt_payload_len,
            write_timeout);
        if (err != ESP_OK) {
            transaction->response->result = AT_RESULT_TRANSPORT_ERROR;
            transaction->done = true;
            diag_increment(&s_engine.diagnostics.transport_errors);
        } else {
            transaction->payload_sent = true;
        }
        return;
    }

    if (token->type != AT_TOKEN_LINE || token->text == NULL) {
        return;
    }

    const char *line = token->text;
    if (transaction == NULL || transaction->done) {
        queue_urc(line);
        return;
    }

    /* Ignore local command echo when modem echo has not yet been disabled. */
    if (transaction->request->command != NULL && strcmp(line, transaction->request->command) == 0) {
        return;
    }

    /*
     * Route URC continuation lines before checking final results. An incoming
     * SMS body is allowed to literally be "OK" or "ERROR" and must never
     * complete an unrelated command.
     */
    if (at_router_route_line(&transaction->router,
                             line,
                             transaction->request->expected_prefixes,
                             transaction->request->expected_prefix_count) == AT_ROUTE_URC) {
        queue_urc(line);
        return;
    }

    const at_final_status_t final = at_protocol_final_status(line);
    if (final.kind != AT_FINAL_NONE) {
        response_capture_final(transaction->response, line);
        if (final.kind == AT_FINAL_OK && transaction->request->wait_for_prompt && !transaction->payload_sent) {
            transaction->response->result = AT_RESULT_PROTOCOL_ERROR;
        } else {
            transaction->response->result = map_final_kind(final.kind);
            transaction->response->error_code = final.error_code;
        }
        transaction->done = true;
        return;
    }

    response_capture_line(transaction->response, line);
}

static bool should_retry(at_result_t result, uint32_t policy)
{
    switch (result) {
    case AT_RESULT_TIMEOUT: return (policy & AT_RETRY_ON_TIMEOUT) != 0;
    case AT_RESULT_ERROR: return (policy & AT_RETRY_ON_ERROR) != 0;
    case AT_RESULT_CME_ERROR: return (policy & AT_RETRY_ON_CME_ERROR) != 0;
    case AT_RESULT_CMS_ERROR: return (policy & AT_RETRY_ON_CMS_ERROR) != 0;
    case AT_RESULT_TRANSPORT_ERROR:
    case AT_RESULT_TRANSPORT_UNAVAILABLE:
        return (policy & AT_RETRY_ON_TRANSPORT_ERROR) != 0;
    default:
        return false;
    }
}

static bool valid_request(const at_request_t *request)
{
    if (request == NULL || request->command == NULL || request->command[0] == '\0') {
        return false;
    }
    const size_t command_length = bounded_strlen(request->command, AT_ENGINE_MAX_COMMAND_LENGTH + 1);
    if (command_length == 0 || command_length > AT_ENGINE_MAX_COMMAND_LENGTH ||
        strchr(request->command, '\r') != NULL || strchr(request->command, '\n') != NULL) {
        return false;
    }
    if (request->expected_prefix_count > AT_ENGINE_MAX_EXPECTED_PREFIXES ||
        (request->expected_prefix_count > 0 && request->expected_prefixes == NULL)) {
        return false;
    }
    if (request->wait_for_prompt &&
        (request->prompt_payload == NULL || request->prompt_payload_len == 0)) {
        return false;
    }
    if (!request->wait_for_prompt &&
        (request->prompt_payload != NULL || request->prompt_payload_len > 0)) {
        return false;
    }
    return true;
}

static esp_err_t send_command(const at_request_t *request)
{
    uint8_t command[AT_ENGINE_MAX_COMMAND_LENGTH + 2];
    const size_t command_length = strlen(request->command);
    memcpy(command, request->command, command_length);
    command[command_length] = '\r';

    if (s_engine.transport.is_ready != NULL && !s_engine.transport.is_ready(s_engine.transport.ctx)) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t write_timeout = s_engine.transport.write_timeout_ms != 0
                                       ? s_engine.transport.write_timeout_ms
                                       : AT_DEFAULT_WRITE_TIMEOUT_MS;
    return s_engine.transport.write(s_engine.transport.ctx, command, command_length + 1, write_timeout);
}

static void process_idle_rx(at_parser_t *parser)
{
    rx_event_t event;
    while (xQueueReceive(s_engine.rx_queue, &event, 0) == pdTRUE) {
        if (event.kind == RX_EVENT_DATA && event.length > 0) {
            at_parser_feed(parser, event.data, event.length, process_token, NULL);
        } else if (event.kind == RX_EVENT_TRANSPORT_LOST || event.kind == RX_EVENT_CANCEL) {
            at_parser_reset(parser);
        }
    }
}

static at_result_t execute_attempt(const at_request_t *request,
                                   at_response_t *response,
                                   at_parser_t *parser)
{
    const uint32_t overflow_epoch = get_epoch(&s_engine.rx_overflow_epoch);
    const uint32_t lost_epoch = get_epoch(&s_engine.transport_lost_epoch);
    const uint32_t cancel_epoch = get_epoch(&s_engine.cancel_epoch);

    const esp_err_t write_err = send_command(request);
    if (write_err != ESP_OK) {
        diag_increment(&s_engine.diagnostics.transport_errors);
        return write_err == ESP_ERR_INVALID_STATE ? AT_RESULT_TRANSPORT_UNAVAILABLE
                                                  : AT_RESULT_TRANSPORT_ERROR;
    }

    transaction_ctx_t transaction = {
        .request = request,
        .response = response,
        .done = false,
        .payload_sent = false,
        .parser = parser,
    };
    at_router_init(&transaction.router);
    at_parser_set_prompt_enabled(parser, request->wait_for_prompt);

    const uint32_t timeout_ms = request->timeout_ms != 0 ? request->timeout_ms : AT_DEFAULT_TIMEOUT_MS;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (!transaction.done) {
        if (get_epoch(&s_engine.rx_overflow_epoch) != overflow_epoch) {
            return AT_RESULT_RX_OVERFLOW;
        }
        if (get_epoch(&s_engine.transport_lost_epoch) != lost_epoch) {
            diag_increment(&s_engine.diagnostics.transport_errors);
            return AT_RESULT_TRANSPORT_ERROR;
        }
        if (get_epoch(&s_engine.cancel_epoch) != cancel_epoch) {
            diag_increment(&s_engine.diagnostics.cancellations);
            return AT_RESULT_CANCELED;
        }

        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            diag_increment(&s_engine.diagnostics.timeouts);
            return AT_RESULT_TIMEOUT;
        }

        uint32_t remaining_ms = (uint32_t)((remaining_us + 999) / 1000);
        TickType_t wait_ticks = pdMS_TO_TICKS(remaining_ms);
        if (wait_ticks == 0) {
            wait_ticks = 1;
        }

        rx_event_t event;
        if (xQueueReceive(s_engine.rx_queue, &event, wait_ticks) != pdTRUE) {
            continue;
        }

        switch (event.kind) {
        case RX_EVENT_DATA:
            if (event.length > 0) {
                at_parser_feed(parser, event.data, event.length, process_token, &transaction);
            }
            break;
        case RX_EVENT_TRANSPORT_LOST:
            diag_increment(&s_engine.diagnostics.transport_errors);
            return AT_RESULT_TRANSPORT_ERROR;
        case RX_EVENT_CANCEL:
            diag_increment(&s_engine.diagnostics.cancellations);
            return AT_RESULT_CANCELED;
        default:
            return AT_RESULT_PROTOCOL_ERROR;
        }
    }

    return response->result;
}

static void execute_request(const request_message_t *message, at_parser_t *parser)
{
    at_response_t *response = message->response;
    const at_request_t *request = &message->request;
    response_reset(response);

    /* Route stale complete lines as idle traffic before starting a command. */
    process_idle_rx(parser);

    const uint8_t max_attempts = request->max_attempts == 0 ? 1 : request->max_attempts;
    for (uint8_t attempt = 1; attempt <= max_attempts; ++attempt) {
        response_reset(response);
        response->attempts = attempt;

        const at_result_t result = execute_attempt(request, response, parser);
        at_parser_set_prompt_enabled(parser, false);
        response->result = result;
        if (result != AT_RESULT_OK) {
            at_parser_reset(parser);
        }

        if (result == AT_RESULT_OK ||
            !should_retry(result, request->retry_policy) || attempt == max_attempts) {
            break;
        }

        diag_increment(&s_engine.diagnostics.commands_retried);
        at_parser_reset(parser);
        process_idle_rx(parser);
        if (request->retry_delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(request->retry_delay_ms));
            process_idle_rx(parser);
        }
    }

    diag_increment(&s_engine.diagnostics.commands_completed);
}

static void worker_task(void *arg)
{
    (void)arg;
    at_parser_t parser;
    at_parser_init(&parser);

    for (;;) {
        request_message_t request_message;
        if (xQueueReceive(s_engine.request_queue, &request_message, 0) == pdTRUE) {
            execute_request(&request_message, &parser);
            xSemaphoreGive(request_message.completion);
            continue;
        }

        rx_event_t rx_event;
        if (xQueueReceive(s_engine.rx_queue, &rx_event, pdMS_TO_TICKS(AT_IDLE_RX_WAIT_MS)) == pdTRUE) {
            if (rx_event.kind == RX_EVENT_DATA && rx_event.length > 0) {
                at_parser_feed(&parser, rx_event.data, rx_event.length, process_token, NULL);
            } else {
                at_parser_reset(&parser);
            }
        }
    }
}

static void cleanup_failed_init(void)
{
    if (s_engine.urc_task != NULL) {
        vTaskDelete(s_engine.urc_task);
        s_engine.urc_task = NULL;
    }
    if (s_engine.worker_task != NULL) {
        vTaskDelete(s_engine.worker_task);
        s_engine.worker_task = NULL;
    }
    if (s_engine.urc_queue != NULL) {
        vQueueDelete(s_engine.urc_queue);
        s_engine.urc_queue = NULL;
    }
    if (s_engine.rx_queue != NULL) {
        vQueueDelete(s_engine.rx_queue);
        s_engine.rx_queue = NULL;
    }
    if (s_engine.request_queue != NULL) {
        vQueueDelete(s_engine.request_queue);
        s_engine.request_queue = NULL;
    }

    portENTER_CRITICAL(&s_lock);
    s_engine.initialized = false;
    memset(&s_engine.transport, 0, sizeof(s_engine.transport));
    s_engine.urc_callback = NULL;
    s_engine.urc_user_ctx = NULL;
    memset(&s_engine.diagnostics, 0, sizeof(s_engine.diagnostics));
    s_engine.rx_overflow_epoch = 0;
    s_engine.transport_lost_epoch = 0;
    s_engine.cancel_epoch = 0;
    portEXIT_CRITICAL(&s_lock);
}

static void urc_dispatch_task(void *arg)
{
    (void)arg;
    urc_message_t message;
    for (;;) {
        if (xQueueReceive(s_engine.urc_queue, &message, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        at_urc_callback_t callback;
        void *user_ctx;
        portENTER_CRITICAL(&s_lock);
        callback = s_engine.urc_callback;
        user_ctx = s_engine.urc_user_ctx;
        portEXIT_CRITICAL(&s_lock);

        if (callback != NULL) {
            callback(message.line, user_ctx);
            diag_increment(&s_engine.diagnostics.urcs_dispatched);
        } else {
            ESP_LOGD(TAG, "unhandled URC/idle line: %s", message.line);
        }
    }
}

esp_err_t at_engine_init(const at_transport_t *transport)
{
    if (transport == NULL || transport->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_engine.initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_engine.initialized = true;
    s_engine.transport = *transport;
    s_engine.urc_callback = NULL;
    s_engine.urc_user_ctx = NULL;
    memset(&s_engine.diagnostics, 0, sizeof(s_engine.diagnostics));
    s_engine.rx_overflow_epoch = 0;
    s_engine.transport_lost_epoch = 0;
    s_engine.cancel_epoch = 0;
    portEXIT_CRITICAL(&s_lock);

    s_engine.request_queue = xQueueCreate(AT_REQUEST_QUEUE_DEPTH, sizeof(request_message_t));
    s_engine.rx_queue = xQueueCreate(AT_RX_QUEUE_DEPTH, sizeof(rx_event_t));
    s_engine.urc_queue = xQueueCreate(AT_URC_QUEUE_DEPTH, sizeof(urc_message_t));
    if (s_engine.request_queue == NULL || s_engine.rx_queue == NULL || s_engine.urc_queue == NULL) {
        cleanup_failed_init();
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(worker_task,
                    "at_worker",
                    AT_WORKER_TASK_STACK,
                    NULL,
                    AT_WORKER_TASK_PRIORITY,
                    &s_engine.worker_task) != pdPASS) {
        cleanup_failed_init();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(urc_dispatch_task,
                    "at_urc",
                    AT_URC_TASK_STACK,
                    NULL,
                    AT_URC_TASK_PRIORITY,
                    &s_engine.urc_task) != pdPASS) {
        cleanup_failed_init();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "AT engine initialized");
    return ESP_OK;
}

esp_err_t at_engine_execute(const at_request_t *request, at_response_t *response)
{
    if (!s_engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid_request(request) || response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    StaticSemaphore_t completion_buffer;
    SemaphoreHandle_t completion = xSemaphoreCreateBinaryStatic(&completion_buffer);
    if (completion == NULL) {
        return ESP_ERR_NO_MEM;
    }

    request_message_t message = {
        .request = *request,
        .response = response,
        .completion = completion,
    };

    diag_increment(&s_engine.diagnostics.commands_submitted);
    if (xQueueSend(s_engine.request_queue, &message, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(completion, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void at_engine_feed_rx(const uint8_t *data, size_t data_len)
{
    if (!s_engine.initialized || data == NULL || data_len == 0 || s_engine.rx_queue == NULL) {
        return;
    }

    while (data_len > 0) {
        const size_t chunk = data_len > AT_RX_CHUNK_SIZE ? AT_RX_CHUNK_SIZE : data_len;
        rx_event_t event = {
            .kind = RX_EVENT_DATA,
            .length = (uint16_t)chunk,
        };
        memcpy(event.data, data, chunk);
        if (xQueueSend(s_engine.rx_queue, &event, 0) != pdTRUE) {
            portENTER_CRITICAL(&s_lock);
            ++s_engine.rx_overflow_epoch;
            ++s_engine.diagnostics.rx_overflows;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG, "RX queue overflow; dropping %u byte(s)", (unsigned)data_len);
            return;
        }
        data += chunk;
        data_len -= chunk;
    }
}

void at_engine_notify_transport_lost(void)
{
    if (!s_engine.initialized) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    ++s_engine.transport_lost_epoch;
    portEXIT_CRITICAL(&s_lock);

    if (s_engine.rx_queue != NULL) {
        const rx_event_t event = {.kind = RX_EVENT_TRANSPORT_LOST};
        (void)xQueueSendToFront(s_engine.rx_queue, &event, 0);
    }
}

esp_err_t at_engine_cancel_current(void)
{
    if (!s_engine.initialized || s_engine.rx_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    ++s_engine.cancel_epoch;
    portEXIT_CRITICAL(&s_lock);

    const rx_event_t event = {.kind = RX_EVENT_CANCEL};
    return xQueueSendToFront(s_engine.rx_queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t at_engine_set_urc_callback(at_urc_callback_t callback, void *user_ctx)
{
    if (!s_engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    s_engine.urc_callback = callback;
    s_engine.urc_user_ctx = user_ctx;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t at_engine_get_diagnostics(at_engine_diagnostics_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_engine.diagnostics;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

const char *at_engine_result_name(at_result_t result)
{
    switch (result) {
    case AT_RESULT_OK: return "ok";
    case AT_RESULT_ERROR: return "error";
    case AT_RESULT_CME_ERROR: return "cme_error";
    case AT_RESULT_CMS_ERROR: return "cms_error";
    case AT_RESULT_TIMEOUT: return "timeout";
    case AT_RESULT_CANCELED: return "canceled";
    case AT_RESULT_TRANSPORT_UNAVAILABLE: return "transport_unavailable";
    case AT_RESULT_TRANSPORT_ERROR: return "transport_error";
    case AT_RESULT_RX_OVERFLOW: return "rx_overflow";
    case AT_RESULT_PROTOCOL_ERROR: return "protocol_error";
    default: return "unknown";
    }
}
