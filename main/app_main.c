#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "gateway_board.h"
#include "modem_core.h"

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

static void confirm_ota_image_after_self_test(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "boot self-test passed; confirming OTA image");
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "ESP32 SMS Gateway %s", app->version);
    ESP_LOGI(TAG, "project=%s idf=%s", app->project_name, app->idf_ver);

    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(gateway_board_init());

    if (gateway_board_usb_overcurrent()) {
        ESP_LOGW(TAG, "USB host over-current is asserted at boot");
    }

    ESP_ERROR_CHECK(modem_core_init());

    /*
     * Minimum boot self-test boundary for OTA rollback. As services are added,
     * this boundary will include persistence integrity and core task startup.
     */
    confirm_ota_image_after_self_test();

    ESP_LOGI(TAG, "foundation initialized; modem USB transport is the next milestone");
}
