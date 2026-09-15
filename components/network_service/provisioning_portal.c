#include "provisioning_portal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_security.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define PORTAL_BODY_MAX 512
static const char *TAG = "portal";
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static volatile bool s_running;
static volatile bool s_setup_pending;

static const char PORTAL_PAGE[] =
"<!doctype html><html lang=en><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>SMS Gateway setup</title><style>body{max-width:38rem;margin:2rem auto;padding:0 1rem;font:16px system-ui;color:#15202b}input{box-sizing:border-box;width:100%;padding:.7rem;margin:.25rem 0 1rem}button{padding:.8rem 1rem;background:#0057b8;color:white;border:0;border-radius:.3rem}#status{margin-top:1rem}</style>"
"<h1>SMS Gateway setup</h1><p>Connect this gateway to Wi-Fi and create its management token. Save the token: it is shown only here and is not stored in plaintext.</p>"
"<form id=f><label>Wi-Fi network (SSID)<input id=ssid maxlength=32 required autocomplete=off></label><label>Wi-Fi password<input id=password type=password maxlength=64 autocomplete=new-password></label><label>API token <input id=token minlength=32 maxlength=128 required autocomplete=new-password></label><button>Save and connect</button></form><p id=status role=status></p>"
"<script>f.onsubmit=async e=>{e.preventDefault();status.textContent='Saving…';let r=await fetch('/api/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid.value,password:password.value,token:token.value})});status.textContent=r.ok?'Saved. The gateway is joining Wi-Fi; this page will disconnect.':'Setup failed: '+await r.text()};</script></html>";

static esp_err_t send_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORTAL_PAGE, HTTPD_RESP_USE_STRLEN);
}

static bool valid_text(const cJSON *item, size_t max, bool allow_empty)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    const size_t len = strlen(item->valuestring);
    return len <= max && (allow_empty || len > 0);
}

static void connect_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(750)); /* let the HTTP response leave the AP */
    provisioning_portal_stop();
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    (void)esp_wifi_start();
    (void)esp_wifi_connect();
    vTaskDelete(NULL);
}

static esp_err_t setup_handler(httpd_req_t *req)
{
    if (s_setup_pending || req->content_len <= 0 || req->content_len >= PORTAL_BODY_MAX) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid setup request");
    }
    char body[PORTAL_BODY_MAX] = {0};
    int received = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) { gateway_security_wipe(body, sizeof(body)); return ESP_FAIL; }
        received += n;
    }
    cJSON *json = cJSON_ParseWithLength(body, (size_t)received);
    cJSON *ssid = json ? cJSON_GetObjectItemCaseSensitive(json, "ssid") : NULL;
    cJSON *password = json ? cJSON_GetObjectItemCaseSensitive(json, "password") : NULL;
    cJSON *token = json ? cJSON_GetObjectItemCaseSensitive(json, "token") : NULL;
    const bool valid = valid_text(ssid, 32, false) && valid_text(password, 64, true) && valid_text(token, 128, false);
    if (!valid || gateway_security_set_token(token->valuestring) != ESP_OK) {
        if (json) cJSON_Delete(json);
        gateway_security_wipe(body, sizeof(body));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Use a 1-32 byte SSID and a 32-128 character token.");
    }

    wifi_config_t config = {0};
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", ssid->valuestring);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", password->valuestring);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.failure_retry_cnt = 5;
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    gateway_security_wipe(&config, sizeof(config));
    cJSON_Delete(json);
    gateway_security_wipe(body, sizeof(body));
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Could not save Wi-Fi credentials.");
    }
    s_setup_pending = true;
    const esp_err_t response = httpd_resp_sendstr(req, "saved");
    if (xTaskCreate(connect_task, "portal_connect", 4096, NULL, 5, NULL) != pdPASS) s_setup_pending = false;
    return response;
}

static void dns_task(void *arg)
{
    (void)arg;
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) { close(fd); vTaskDelete(NULL); return; }
    struct timeval timeout = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    while (s_running) {
        uint8_t packet[512]; struct sockaddr_in peer; socklen_t peer_len = sizeof(peer);
        const int len = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&peer, &peer_len);
        if (len < 13) continue;
        int question_end = 12;
        while (question_end < len && packet[question_end] != 0) question_end += packet[question_end] + 1;
        question_end += 5; /* zero label + QTYPE + QCLASS */
        if (question_end > len || question_end + 16 > (int)sizeof(packet)) continue;
        packet[2] = 0x81; packet[3] = 0x80; packet[6] = 0; packet[7] = 1;
        memset(packet + 8, 0, 4);
        uint8_t *answer = packet + question_end;
        const uint8_t record[] = {0xc0,0x0c,0,1,0,1,0,0,0,30,0,4,192,168,4,1};
        memcpy(answer, record, sizeof(record));
        (void)sendto(fd, packet, question_end + sizeof(record), 0, (struct sockaddr *)&peer, peer_len);
    }
    close(fd);
    vTaskDelete(NULL);
}

esp_err_t provisioning_portal_start(const char *ssid, const char *passphrase)
{
    if (s_running || ssid == NULL || passphrase == NULL) return ESP_ERR_INVALID_STATE;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 4;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &config), TAG, "start HTTP portal");
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = send_page};
    const httpd_uri_t captive = {.uri = "/*", .method = HTTP_GET, .handler = send_page};
    const httpd_uri_t setup = {.uri = "/api/setup", .method = HTTP_POST, .handler = setup_handler};
    if (httpd_register_uri_handler(s_httpd, &root) != ESP_OK || httpd_register_uri_handler(s_httpd, &captive) != ESP_OK || httpd_register_uri_handler(s_httpd, &setup) != ESP_OK) {
        httpd_stop(s_httpd); s_httpd = NULL; return ESP_FAIL;
    }
    s_running = true;
    if (xTaskCreate(dns_task, "portal_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        provisioning_portal_stop(); return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "setup portal at http://192.168.4.1 (SSID %s)", ssid);
    return ESP_OK;
}

void provisioning_portal_stop(void)
{
    s_running = false;
    if (s_httpd != NULL) { httpd_stop(s_httpd); s_httpd = NULL; }
    s_dns_task = NULL;
}
