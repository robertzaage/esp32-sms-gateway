#include "network_service.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gateway_security.h"
#include "nvs.h"
#include "provisioning_portal.h"

/*
 * All Wi-Fi state transitions run on one task (network_task). ESP-IDF event
 * handlers and the portal only post commands to it, so there are no races
 * between reconnect timers, the setup portal and credential tests.
 */

#define NET_TASK_STACK 4096
#define NET_TASK_PRIORITY 5
#define NET_QUEUE_DEPTH 12
#define NET_TICK_MS 1000

#define RECONNECT_MIN_MS 1000U
#define RECONNECT_MAX_MS 30000U
/* While the SoftAP is up, STA scans pull the radio off the AP channel. Retry rarely. */
#define RECONNECT_WITH_PORTAL_MS 30000U
#define SETUP_CONNECT_ATTEMPTS 3U
#define SETUP_CONNECT_TIMEOUT_MS 30000U
#define SETUP_RESTART_DELAY_MS 10000U

#define PORTAL_PASSWORD_CHARS 12
#define NET_NVS_NAMESPACE "net_cfg"
#define NET_NVS_PORTAL_PASS "ap_pass"

static const char *TAG = "network";

typedef enum {
    NET_CMD_STA_START = 0,
    NET_CMD_DISCONNECTED,
    NET_CMD_GOT_IP,
    NET_CMD_OPEN_PORTAL,
    NET_CMD_SETUP_SUBMITTED,
    NET_CMD_TICK,
} net_cmd_type_t;

typedef struct {
    net_cmd_type_t type;
    int reason; /* disconnect reason, or network_portal_reason_t for OPEN_PORTAL */
    char ipv4[NETWORK_IPV4_MAX];
    char ssid[NETWORK_SSID_MAX];
    char password[65];
} net_cmd_t;

static network_service_snapshot_t s_snapshot;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static network_service_config_t s_config;
static QueueHandle_t s_queue;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_sntp_started;

/* Owned by network_task only. */
static uint32_t s_reconnect_delay_ms = RECONNECT_MIN_MS;
static int64_t s_next_connect_ms;      /* 0 = no reconnect scheduled */
static int64_t s_offline_since_ms;     /* when the STA link was last lost (or boot) */
static int64_t s_portal_opened_ms;
static int64_t s_setup_deadline_ms;
static int64_t s_restart_at_ms;
static uint32_t s_setup_failures;
static bool s_sta_connecting;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void build_identity(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_snapshot.device_id, sizeof(s_snapshot.device_id),
             "smsgw-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_snapshot.hostname, sizeof(s_snapshot.hostname),
             "sms-gateway-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_snapshot.portal_ssid, sizeof(s_snapshot.portal_ssid),
             "SMS-Gateway-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void copy_snapshot(network_service_snapshot_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}

static void emit_snapshot(void)
{
    if (s_config.on_change == NULL) return;
    network_service_snapshot_t snapshot;
    copy_snapshot(&snapshot);
    s_config.on_change(&snapshot, s_config.user_ctx);
    gateway_security_wipe(snapshot.portal_passphrase, sizeof(snapshot.portal_passphrase));
}

static bool post(const net_cmd_t *cmd)
{
    if (s_queue == NULL || xQueueSend(s_queue, cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "network command %d dropped", (int)cmd->type);
        return false;
    }
    return true;
}

/*
 * The setup AP password is generated once and kept in NVS so it stays the same
 * across reboots. Uppercase letters and digits only, without look-alikes
 * (0/O, 1/I), so it can be typed reliably from the display.
 */
static esp_err_t load_portal_password(char out[NETWORK_PORTAL_PASSWORD_MAX])
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open network NVS");
    size_t len = NETWORK_PORTAL_PASSWORD_MAX;
    esp_err_t err = nvs_get_str(nvs, NET_NVS_PORTAL_PASS, out, &len);
    if (err == ESP_OK && strlen(out) >= 8) {
        nvs_close(nvs);
        return ESP_OK;
    }
    uint8_t random[PORTAL_PASSWORD_CHARS];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) {
        out[i] = alphabet[random[i] % (sizeof(alphabet) - 1)];
    }
    out[PORTAL_PASSWORD_CHARS] = '\0';
    gateway_security_wipe(random, sizeof(random));
    err = nvs_set_str(nvs, NET_NVS_PORTAL_PASS, out);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static bool sta_credentials_saved(char ssid_out[NETWORK_SSID_MAX])
{
    wifi_config_t saved = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) != ESP_OK) return false;
    const bool provisioned = saved.sta.ssid[0] != '\0';
    if (ssid_out != NULL) {
        memcpy(ssid_out, saved.sta.ssid, NETWORK_SSID_MAX - 1);
        ssid_out[NETWORK_SSID_MAX - 1] = '\0';
    }
    gateway_security_wipe(&saved, sizeof(saved));
    return provisioned;
}

static void sta_connect_now(void)
{
    s_next_connect_ms = 0;
    s_sta_connecting = true;
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_sta_connecting = false;
        ESP_LOGW(TAG, "Wi-Fi connect request failed: %s", esp_err_to_name(err));
    }
}

static void schedule_reconnect(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool portal = s_snapshot.provisioning;
    ++s_snapshot.reconnects;
    portEXIT_CRITICAL(&s_lock);
    uint32_t delay = s_reconnect_delay_ms;
    if (portal && delay < RECONNECT_WITH_PORTAL_MS) delay = RECONNECT_WITH_PORTAL_MS;
    s_next_connect_ms = now_ms() + delay;
    if (s_reconnect_delay_ms < RECONNECT_MAX_MS) {
        s_reconnect_delay_ms *= 2;
        if (s_reconnect_delay_ms > RECONNECT_MAX_MS) s_reconnect_delay_ms = RECONNECT_MAX_MS;
    }
}

static esp_err_t open_portal(network_portal_reason_t reason)
{
    if (s_snapshot.provisioning) {
        /* Already open; a manual request upgrades the reason so it is not auto-closed. */
        portENTER_CRITICAL(&s_lock);
        if (reason == NETWORK_PORTAL_MANUAL) s_snapshot.portal_reason = reason;
        portEXIT_CRITICAL(&s_lock);
        s_portal_opened_ms = now_ms();
        emit_snapshot();
        return ESP_OK;
    }
    if (s_config.before_portal_start != NULL) s_config.before_portal_start(s_config.user_ctx);
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) return ESP_ERR_NO_MEM;
    }

    char passphrase[NETWORK_PORTAL_PASSWORD_MAX] = {0};
    esp_err_t err = load_portal_password(passphrase);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not load setup AP password: %s", esp_err_to_name(err));
        return err;
    }
    wifi_config_t ap = {0};
    const size_t ssid_len = strlen(s_snapshot.portal_ssid);
    memcpy(ap.ap.ssid, s_snapshot.portal_ssid, ssid_len);
    ap.ap.ssid_len = ssid_len;
    memcpy(ap.ap.password, passphrase, strlen(passphrase));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    gateway_security_wipe(&ap, sizeof(ap));
    if (err == ESP_OK) err = provisioning_portal_start();
    if (err != ESP_OK) {
        gateway_security_wipe(passphrase, sizeof(passphrase));
        ESP_LOGE(TAG, "could not open setup portal: %s", esp_err_to_name(err));
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    s_snapshot.provisioning = true;
    s_snapshot.portal_reason = reason;
    memcpy(s_snapshot.portal_passphrase, passphrase, sizeof(s_snapshot.portal_passphrase));
    portEXIT_CRITICAL(&s_lock);
    gateway_security_wipe(passphrase, sizeof(passphrase));
    s_portal_opened_ms = now_ms();
    ESP_LOGI(TAG, "setup portal open (reason=%d): join Wi-Fi \"%s\" and browse to http://192.168.4.1",
             (int)reason, s_snapshot.portal_ssid);
    emit_snapshot();
    return ESP_OK;
}

static void close_portal(void)
{
    if (!s_snapshot.provisioning) return;
    provisioning_portal_stop();
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    portENTER_CRITICAL(&s_lock);
    s_snapshot.provisioning = false;
    s_snapshot.portal_reason = NETWORK_PORTAL_CLOSED;
    gateway_security_wipe(s_snapshot.portal_passphrase, sizeof(s_snapshot.portal_passphrase));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "setup portal closed");
    emit_snapshot();
}

static void set_setup_state(network_setup_state_t state)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.setup_state = state;
    portEXIT_CRITICAL(&s_lock);
    emit_snapshot();
}

static void start_sntp_once(void)
{
    if (s_sntp_started) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&config) == ESP_OK) s_sntp_started = true;
}

static void handle_disconnected(int reason)
{
    s_sta_connecting = false;
    portENTER_CRITICAL(&s_lock);
    const bool was_connected = s_snapshot.connected;
    s_snapshot.connected = false;
    s_snapshot.ipv4[0] = '\0';
    ++s_snapshot.disconnects;
    s_snapshot.last_disconnect_reason = reason;
    const network_setup_state_t setup = s_snapshot.setup_state;
    const bool provisioned = s_snapshot.provisioned;
    portEXIT_CRITICAL(&s_lock);
    if (was_connected) {
        s_offline_since_ms = now_ms();
        s_reconnect_delay_ms = RECONNECT_MIN_MS;
    }
    ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d", reason);

    if (setup == NETWORK_SETUP_CONNECTING) {
        /* ASSOC_LEAVE is our own disconnect from the previous network. */
        if (reason == WIFI_REASON_ASSOC_LEAVE) return;
        if (++s_setup_failures >= SETUP_CONNECT_ATTEMPTS) {
            ESP_LOGW(TAG, "new Wi-Fi credentials failed (reason=%d)", reason);
            set_setup_state(NETWORK_SETUP_FAILED);
            schedule_reconnect();
        } else {
            s_next_connect_ms = now_ms() + 2000;
            emit_snapshot();
        }
        return;
    }
    if (setup == NETWORK_SETUP_CONNECTED) return; /* restarting */
    if (provisioned) schedule_reconnect();
    emit_snapshot();
}

static void handle_got_ip(const char *ip)
{
    s_sta_connecting = false;
    s_next_connect_ms = 0;
    s_reconnect_delay_ms = RECONNECT_MIN_MS;
    char ssid[NETWORK_SSID_MAX] = {0};
    (void)sta_credentials_saved(ssid);
    portENTER_CRITICAL(&s_lock);
    s_snapshot.connected = true;
    s_snapshot.provisioned = true;
    snprintf(s_snapshot.ipv4, sizeof(s_snapshot.ipv4), "%s", ip);
    memcpy(s_snapshot.sta_ssid, ssid, sizeof(s_snapshot.sta_ssid));
    /* A failed test is obsolete once the stored network connects after all. */
    if (s_snapshot.setup_state == NETWORK_SETUP_FAILED) s_snapshot.setup_state = NETWORK_SETUP_IDLE;
    const network_setup_state_t setup = s_snapshot.setup_state;
    const network_portal_reason_t portal = s_snapshot.provisioning ? s_snapshot.portal_reason : NETWORK_PORTAL_CLOSED;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "connected to \"%s\", ip=%s", ssid, ip);
    start_sntp_once();

    if (setup == NETWORK_SETUP_CONNECTING) {
        /* Let the browser fetch the result, then restart so every service picks
         * up the new Wi-Fi, MQTT and token settings from a clean state. */
        s_restart_at_ms = now_ms() + SETUP_RESTART_DELAY_MS;
        set_setup_state(NETWORK_SETUP_CONNECTED);
        return;
    }
    if (portal == NETWORK_PORTAL_FALLBACK || portal == NETWORK_PORTAL_FIRST_SETUP) {
        close_portal();
        return;
    }
    emit_snapshot();
}

static esp_err_t apply_sta_credentials(const char *ssid, const char *password)
{
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strnlen(ssid, sizeof(config.sta.ssid)));
    memcpy(config.sta.password, password, strnlen(password, sizeof(config.sta.password)));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.pmf_cfg.capable = true;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    /* The driver refuses a new STA config while a connection attempt is running. */
    (void)esp_wifi_disconnect();
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 5; ++attempt) {
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (err != ESP_ERR_WIFI_STATE) break;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    gateway_security_wipe(&config, sizeof(config));
    return err;
}

static void handle_setup_submitted(net_cmd_t *cmd)
{
    const esp_err_t err = apply_sta_credentials(cmd->ssid, cmd->password);
    gateway_security_wipe(cmd->password, sizeof(cmd->password));
    s_sta_connecting = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not store Wi-Fi credentials: %s", esp_err_to_name(err));
        set_setup_state(NETWORK_SETUP_FAILED);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_snapshot.provisioned = true;
    s_snapshot.setup_state = NETWORK_SETUP_CONNECTING;
    memcpy(s_snapshot.sta_ssid, cmd->ssid, sizeof(s_snapshot.sta_ssid));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "testing new Wi-Fi credentials for \"%s\"", cmd->ssid);
    s_setup_failures = 0;
    s_setup_deadline_ms = now_ms() + SETUP_CONNECT_TIMEOUT_MS;
    s_reconnect_delay_ms = RECONNECT_MIN_MS;
    /* Give the driver a moment to finish leaving the previous network. */
    s_next_connect_ms = now_ms() + 1000;
    emit_snapshot();
}

static void handle_tick(void)
{
    const int64_t now = now_ms();
    network_service_snapshot_t snap;
    copy_snapshot(&snap);
    gateway_security_wipe(snap.portal_passphrase, sizeof(snap.portal_passphrase));

    if (s_restart_at_ms != 0 && now >= s_restart_at_ms) {
        ESP_LOGI(TAG, "setup complete; restarting");
        esp_restart();
    }
    if (snap.setup_state == NETWORK_SETUP_CONNECTING && now >= s_setup_deadline_ms) {
        ESP_LOGW(TAG, "new Wi-Fi credentials timed out");
        set_setup_state(NETWORK_SETUP_FAILED);
        (void)esp_wifi_disconnect();
        schedule_reconnect();
    }
    if (s_next_connect_ms != 0 && now >= s_next_connect_ms && !snap.connected && snap.provisioned) {
        sta_connect_now();
    }
    if (!snap.connected && snap.provisioned && !snap.provisioning &&
        now - s_offline_since_ms >= (int64_t)CONFIG_GATEWAY_WIFI_PORTAL_FALLBACK_SECONDS * 1000) {
        ESP_LOGW(TAG, "Wi-Fi offline for %d s; opening setup portal (still retrying \"%s\")",
                 CONFIG_GATEWAY_WIFI_PORTAL_FALLBACK_SECONDS, snap.sta_ssid);
        if (open_portal(NETWORK_PORTAL_FALLBACK) != ESP_OK) s_offline_since_ms = now; /* retry later */
    }
    if (snap.provisioning && snap.portal_reason == NETWORK_PORTAL_MANUAL &&
        snap.setup_state != NETWORK_SETUP_CONNECTING && snap.setup_state != NETWORK_SETUP_CONNECTED &&
        now - s_portal_opened_ms >= (int64_t)CONFIG_GATEWAY_WIFI_MANUAL_PORTAL_SECONDS * 1000) {
        ESP_LOGI(TAG, "manual setup portal timed out");
        close_portal();
        if (!snap.connected) s_offline_since_ms = now;
    }
}

static void network_task(void *arg)
{
    (void)arg;
    net_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(NET_TICK_MS)) != pdTRUE) cmd.type = NET_CMD_TICK;
        switch (cmd.type) {
        case NET_CMD_STA_START: {
            network_service_snapshot_t snap;
            copy_snapshot(&snap);
            if (snap.provisioned && snap.portal_reason != NETWORK_PORTAL_FIRST_SETUP) sta_connect_now();
            break;
        }
        case NET_CMD_DISCONNECTED: handle_disconnected(cmd.reason); break;
        case NET_CMD_GOT_IP: handle_got_ip(cmd.ipv4); break;
        case NET_CMD_OPEN_PORTAL: (void)open_portal((network_portal_reason_t)cmd.reason); break;
        case NET_CMD_SETUP_SUBMITTED: handle_setup_submitted(&cmd); break;
        case NET_CMD_TICK:
        default: break;
        }
        gateway_security_wipe(&cmd, sizeof(cmd));
        handle_tick();
    }
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    net_cmd_t cmd = {0};
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        cmd.type = NET_CMD_STA_START;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        cmd.type = NET_CMD_DISCONNECTED;
        cmd.reason = event != NULL ? event->reason : 0;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        cmd.type = NET_CMD_GOT_IP;
        if (event != NULL) snprintf(cmd.ipv4, sizeof(cmd.ipv4), IPSTR, IP2STR(&event->ip_info.ip));
    } else {
        return;
    }
    post(&cmd);
}

esp_err_t network_service_init(const network_service_config_t *config)
{
    if (s_snapshot.initialized) return ESP_ERR_INVALID_STATE;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    if (config != NULL) s_config = *config;
    build_identity();

    s_queue = xQueueCreate(NET_QUEUE_DEPTH, sizeof(net_cmd_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) return ESP_ERR_NO_MEM;
    (void)esp_netif_set_hostname(s_sta_netif, s_snapshot.hostname);

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL), TAG, "ip handler");

    char ssid[NETWORK_SSID_MAX] = {0};
    const bool provisioned = sta_credentials_saved(ssid);
    portENTER_CRITICAL(&s_lock);
    s_snapshot.provisioned = provisioned;
    memcpy(s_snapshot.sta_ssid, ssid, sizeof(s_snapshot.sta_ssid));
    s_snapshot.initialized = true;
    portEXIT_CRITICAL(&s_lock);
    s_offline_since_ms = now_ms();

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    if (!provisioned) {
        ESP_LOGI(TAG, "no Wi-Fi credentials stored");
        const net_cmd_t cmd = {.type = NET_CMD_OPEN_PORTAL, .reason = NETWORK_PORTAL_FIRST_SETUP};
        (void)post(&cmd);
    } else {
        ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    }
    if (xTaskCreate(network_task, "network", NET_TASK_STACK, NULL, NET_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    emit_snapshot();
    return ESP_OK;
}

esp_err_t network_service_open_portal(void)
{
    if (!s_snapshot.initialized) return ESP_ERR_INVALID_STATE;
    const net_cmd_t cmd = {.type = NET_CMD_OPEN_PORTAL, .reason = NETWORK_PORTAL_MANUAL};
    return post(&cmd) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t network_service_submit_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_snapshot.initialized) return ESP_ERR_INVALID_STATE;
    const size_t ssid_len = strlen(ssid);
    const size_t password_len = strlen(password);
    if (ssid_len == 0 || ssid_len > 32 || password_len > 64 || (password_len > 0 && password_len < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    net_cmd_t cmd = {.type = NET_CMD_SETUP_SUBMITTED};
    memcpy(cmd.ssid, ssid, ssid_len);
    memcpy(cmd.password, password, password_len);
    const bool queued = post(&cmd);
    gateway_security_wipe(&cmd, sizeof(cmd));
    return queued ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t network_service_get_snapshot(network_service_snapshot_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_snapshot.initialized) return ESP_ERR_INVALID_STATE;
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
    const bool online = network_service_get_snapshot(&snapshot) == ESP_OK && snapshot.connected;
    gateway_security_wipe(snapshot.portal_passphrase, sizeof(snapshot.portal_passphrase));
    return online;
}

const char *network_service_device_id(void)
{
    if (s_snapshot.device_id[0] == '\0') build_identity();
    return s_snapshot.device_id;
}

const char *network_service_hostname(void)
{
    if (s_snapshot.hostname[0] == '\0') build_identity();
    return s_snapshot.hostname;
}
