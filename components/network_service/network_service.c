#include "network_service.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "gateway_security.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "provisioning_portal.h"

#define RECONNECT_MIN_MS 1000U
#define RECONNECT_MAX_MS 60000U
#define PORTAL_PASSWORD_CHARS 16

static const char *TAG = "network";
static network_service_snapshot_t s_snapshot;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static network_service_event_callback_t s_event_cb;
static void *s_event_ctx;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_reconnect_delay_ms = RECONNECT_MIN_MS;
static bool s_sntp_started;

static void copy_snapshot(network_service_snapshot_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}

static void emit_snapshot(void)
{
    if (s_event_cb == NULL) {
        return;
    }
    network_service_snapshot_t snapshot;
    copy_snapshot(&snapshot);
    s_event_cb(&snapshot, s_event_ctx);
}

static void build_identity(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_snapshot.device_id, sizeof(s_snapshot.device_id),
             "smsgw-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_snapshot.hostname, sizeof(s_snapshot.hostname),
             "sms-gateway-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static void generate_portal_password(char out[PORTAL_PASSWORD_CHARS + 1])
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    uint8_t random[PORTAL_PASSWORD_CHARS];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) {
        out[i] = alphabet[random[i] % (sizeof(alphabet) - 1)];
    }
    out[PORTAL_PASSWORD_CHARS] = '\0';
    volatile uint8_t *wipe = random;
    for (size_t i = 0; i < sizeof(random); ++i) {
        wipe[i] = 0;
    }
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    network_service_snapshot_t snapshot;
    copy_snapshot(&snapshot);
    if (snapshot.provisioning || snapshot.connected) {
        return;
    }
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s", esp_err_to_name(err));
    }
}

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL) {
        return;
    }
    (void)esp_timer_stop(s_reconnect_timer);
    const uint64_t delay_us = (uint64_t)s_reconnect_delay_ms * 1000ULL;
    if (esp_timer_start_once(s_reconnect_timer, delay_us) == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        ++s_snapshot.reconnects;
        portEXIT_CRITICAL(&s_lock);
    }
    if (s_reconnect_delay_ms < RECONNECT_MAX_MS) {
        s_reconnect_delay_ms *= 2;
        if (s_reconnect_delay_ms > RECONNECT_MAX_MS) {
            s_reconnect_delay_ms = RECONNECT_MAX_MS;
        }
    }
}

static void start_sntp_once(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&config) == ESP_OK) {
        s_sntp_started = true;
    }
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        network_service_snapshot_t snapshot;
        copy_snapshot(&snapshot);
        if (!snapshot.provisioning) {
            (void)esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        portENTER_CRITICAL(&s_lock);
        s_snapshot.connected = false;
        s_snapshot.ipv4[0] = '\0';
        ++s_snapshot.disconnects;
        s_snapshot.last_disconnect_reason = event != NULL ? event->reason : 0;
        const bool provisioning = s_snapshot.provisioning;
        portEXIT_CRITICAL(&s_lock);
        emit_snapshot();
        if (!provisioning) {
            schedule_reconnect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        char ip[NETWORK_IPV4_MAX] = {0};
        if (event != NULL) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        }
        portENTER_CRITICAL(&s_lock);
        s_snapshot.connected = true;
        s_snapshot.provisioned = true;
        s_snapshot.provisioning = false;
        gateway_security_wipe(s_snapshot.portal_passphrase, sizeof(s_snapshot.portal_passphrase));
        snprintf(s_snapshot.ipv4, sizeof(s_snapshot.ipv4), "%s", ip);
        portEXIT_CRITICAL(&s_lock);
        s_reconnect_delay_ms = RECONNECT_MIN_MS;
        (void)esp_timer_stop(s_reconnect_timer);
        start_sntp_once();
        emit_snapshot();
    }
}

esp_err_t network_service_init(network_service_event_callback_t cb, void *user_ctx)
{
    if (s_snapshot.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_event_cb = cb;
    s_event_ctx = user_ctx;
    build_identity();

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (void)esp_netif_set_hostname(s_sta_netif, s_snapshot.hostname);

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL), TAG, "ip handler");

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG, "reconnect timer");

    wifi_config_t saved = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &saved), TAG, "read saved Wi-Fi");
    const bool provisioned = saved.sta.ssid[0] != '\0';
    memset(&saved, 0, sizeof(saved));
    portENTER_CRITICAL(&s_lock);
    s_snapshot.provisioned = provisioned;
    s_snapshot.initialized = true;
    portEXIT_CRITICAL(&s_lock);

    if (!provisioned) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) return ESP_ERR_NO_MEM;
        char passphrase[PORTAL_PASSWORD_CHARS + 1];
        generate_portal_password(passphrase);
        snprintf(s_snapshot.portal_ssid, sizeof(s_snapshot.portal_ssid), "%s-setup", s_snapshot.device_id);
        snprintf(s_snapshot.portal_passphrase, sizeof(s_snapshot.portal_passphrase), "%s", passphrase);
        wifi_config_t ap = {0};
        snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_snapshot.portal_ssid);
        snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", passphrase);
        ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.max_connection = 4;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "portal Wi-Fi mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "portal AP config");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "portal Wi-Fi start");
        err = provisioning_portal_start(s_snapshot.portal_ssid, passphrase);
        gateway_security_wipe(passphrase, sizeof(passphrase));
        gateway_security_wipe(&ap, sizeof(ap));
        if (err != ESP_OK) return err;
        portENTER_CRITICAL(&s_lock);
        s_snapshot.provisioning = true;
        portEXIT_CRITICAL(&s_lock);
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    }
    emit_snapshot();
    return ESP_OK;
}

esp_err_t network_service_get_snapshot(network_service_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_snapshot.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    copy_snapshot(out);
    if (s_sntp_started) {
        time_t now = 0;
        time(&now);
        out->time_synced = now > 1700000000;
    }
    return ESP_OK;
}

bool network_service_is_online(void)
{
    network_service_snapshot_t snapshot;
    return network_service_get_snapshot(&snapshot) == ESP_OK && snapshot.connected;
}

const char *network_service_device_id(void)
{
    return s_snapshot.device_id;
}

const char *network_service_hostname(void)
{
    return s_snapshot.hostname;
}
