#include <stdio.h>
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "gateway_board.h"
#include "gateway_security.h"
#include "api_idempotency.h"
#include "network_service.h"
#include "gateway_settings.h"
#include "mqtt_service.h"
#include "api_server.h"
#include "modem_core.h"
#include "ota_service.h"

static const char *TAG = "gateway";

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

    char bootstrap_token[GATEWAY_API_TOKEN_HEX_LEN + 1] = {0};
    bool token_generated = false;
    ESP_ERROR_CHECK(gateway_security_init(bootstrap_token, sizeof(bootstrap_token), &token_generated));
    ESP_ERROR_CHECK(gateway_idempotency_init());
    if (token_generated) {
        /* Deliberate one-time serial bootstrap secret; it is never persisted in plaintext. */
        printf("INITIAL_API_TOKEN=%s\n", bootstrap_token);
        gateway_security_wipe(bootstrap_token, sizeof(bootstrap_token));
    }

    ESP_ERROR_CHECK(modem_core_init());
    ESP_ERROR_CHECK(network_service_init(NULL, NULL));
    ESP_ERROR_CHECK(gateway_settings_init(network_service_device_id()));
    ESP_ERROR_CHECK(mqtt_service_init());
    ESP_ERROR_CHECK(api_server_init());

    /*
     * All critical services reached their startup boundary. A newly booted OTA
     * image remains pending for an additional stability window before it is
     * marked valid; any reset before then triggers bootloader rollback.
     */
    ESP_ERROR_CHECK(gateway_ota_mark_services_ready());

    ESP_LOGI(TAG, "USB modem discovery started; connect Huawei %04x:%04x to the Type-A host port",
             (unsigned)CONFIG_GATEWAY_MODEM_USB_VID, (unsigned)CONFIG_GATEWAY_MODEM_USB_PID);
}
