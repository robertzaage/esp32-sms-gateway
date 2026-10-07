#include <stdio.h>
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "gateway_board.h"
#include "display_service.h"
#include "gateway_security.h"
#include "api_idempotency.h"
#include "network_service.h"
#include "gateway_settings.h"
#include "mqtt_service.h"
#include "api_server.h"
#include "modem_core.h"
#include "ota_service.h"

static const char *TAG = "gateway";
static TaskHandle_t s_supervisor;
static SemaphoreHandle_t s_api_lock;

/*
 * The REST API and the setup portal both need port 80. The supervisor starts
 * the API whenever the gateway is online and the portal is closed; the network
 * service stops it synchronously before opening the portal.
 */
static void supervisor_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        network_service_snapshot_t net;
        if (network_service_get_snapshot(&net) != ESP_OK) continue;
        const bool want_api = net.connected && !net.provisioning;
        if (xSemaphoreTake(s_api_lock, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        if (want_api && !api_server_running()) {
            const esp_err_t err = api_server_init();
            if (err == ESP_OK) ESP_LOGI(TAG, "management API on http://%s/", net.ipv4);
            else ESP_LOGE(TAG, "management API did not start: %s", esp_err_to_name(err));
        }
        xSemaphoreGive(s_api_lock);
    }
}

static void network_changed(const network_service_snapshot_t *snapshot, void *user_ctx)
{
    (void)user_ctx;
    display_service_network_event(snapshot, NULL);
    gateway_board_status_led_set(snapshot->connected);
    if (s_supervisor != NULL) xTaskNotifyGive(s_supervisor);
}

static void before_portal_start(void *user_ctx)
{
    (void)user_ctx;
    if (xSemaphoreTake(s_api_lock, portMAX_DELAY) == pdTRUE) {
        api_server_stop();
        xSemaphoreGive(s_api_lock);
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requires reinitialization: %s", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "failed to erase NVS");
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "ESP32 SMS Gateway %s", app->version);
    ESP_LOGI(TAG, "project=%s idf=%s", app->project_name, app->idf_ver);

    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(gateway_board_init());
    ESP_ERROR_CHECK(gateway_ota_service_init());

    if (gateway_board_usb_overcurrent()) {
        ESP_LOGW(TAG, "USB host over-current is asserted at boot");
    }

    bool token_generated = false;
    ESP_ERROR_CHECK(gateway_security_init(NULL, 0, &token_generated));
    (void)token_generated;
    ESP_ERROR_CHECK(gateway_idempotency_init());
    /* Settings first: the setup portal reads and writes them. */
    ESP_ERROR_CHECK(gateway_settings_init(network_service_device_id()));
    ESP_ERROR_CHECK(display_service_init());
    ESP_ERROR_CHECK(modem_core_init());

    s_api_lock = xSemaphoreCreateMutex();
    if (s_api_lock == NULL ||
        xTaskCreate(supervisor_task, "supervisor", 4096, NULL, 4, &s_supervisor) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    const network_service_config_t net_cfg = {
        .on_change = network_changed,
        .before_portal_start = before_portal_start,
    };
    ESP_ERROR_CHECK(network_service_init(&net_cfg));
    ESP_ERROR_CHECK(mqtt_service_init());

    /*
     * All critical services reached their startup boundary. A newly booted OTA
     * image remains pending for an additional stability window before it is
     * marked valid; any reset before then triggers bootloader rollback.
     */
    ESP_ERROR_CHECK(gateway_ota_mark_services_ready());

    ESP_LOGI(TAG, "USB modem discovery started; connect the Huawei modem to the Type-A host port");
}
