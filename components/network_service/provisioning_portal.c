#include "provisioning_portal.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_diag.h"
#include "gateway_security.h"
#include "gateway_settings.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "network_service.h"

#define PORTAL_BODY_MAX 1536
#define PORTAL_SCAN_MAX 20
#define PORTAL_HOST_MAX 200
#define DNS_TASK_EXIT_WAIT_MS 2500

static const char *TAG = "portal";
static httpd_handle_t s_httpd;
static volatile bool s_running;
static volatile bool s_dns_running;

static const char PORTAL_PAGE[] =
"<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>SMS Gateway setup</title><style>#diag{display:none;background:#fde8e8;padding:.7rem;border-radius:.3rem;margin-bottom:1rem;font-size:.9rem}"
"body{max-width:34rem;margin:1.5rem auto;padding:0 1rem;font:16px system-ui,sans-serif;color:#15202b;background:#fff}"
"fieldset{border:1px solid #ccd;border-radius:.4rem;margin:0 0 1rem;padding:.6rem 1rem}legend{font-weight:600}"
"label{display:block;margin:.4rem 0}input[type=text],input[type=password],input[type=number]{box-sizing:border-box;width:100%;padding:.6rem;margin-top:.2rem}"
".row{display:flex;gap:.5rem}.row>*{flex:1}small{color:#556}button{padding:.7rem 1rem;background:#0057b8;color:#fff;border:0;border-radius:.3rem;font-size:1rem}"
"button.s{background:#e8edf5;color:#0057b8;padding:.4rem .7rem;font-size:.9rem}#status{margin-top:1rem;padding:.7rem;border-radius:.3rem;display:none}"
".ok{background:#e3f6e8}.err{background:#fde8e8}.busy{background:#eef3fb}code{word-break:break-all;font-size:1rem}"
"</style><h1>SMS Gateway setup</h1><div id=diag></div>"
"<form id=f><fieldset><legend>Wi-Fi</legend>"
"<label>Network (SSID)<input type=text id=ssid name=ssid maxlength=32 required list=nets autocomplete=off></label><datalist id=nets></datalist>"
"<button type=button class=s id=scan>Scan for networks</button>"
"<label>Password<input type=password id=wpass maxlength=64 autocomplete=new-password></label>"
"<small id=wkeep></small></fieldset>"
"<fieldset><legend>MQTT / Home Assistant</legend><small>Leave the broker empty to disable MQTT.</small>"
"<div class=row><label>Broker host<input type=text id=mhost maxlength=200 placeholder=homeassistant.local autocomplete=off></label>"
"<label style='flex:0 0 6rem'>Port<input type=number id=mport min=1 max=65535 value=1883></label></div>"
"<label><input type=checkbox id=mtls> Use TLS (mqtts://)</label>"
"<label>Username<input type=text id=muser maxlength=95 autocomplete=off></label>"
"<label>Password<input type=password id=mpass maxlength=159 autocomplete=new-password></label><small id=mkeep></small>"
"<label><input type=checkbox id=ha checked> Home Assistant discovery</label>"
"<label>Default SMS recipient for the HA notify entity<input type=text id=rcpt maxlength=16 placeholder=+491701234567 autocomplete=off></label>"
"</fieldset><fieldset><legend>REST API token</legend>"
"<label>Token (32-128 characters)<input type=text id=token minlength=32 maxlength=128 autocomplete=off></label>"
"<small id=tkeep>Leave empty to generate one.</small></fieldset>"
"<button id=save>Save and connect</button></form><div id=tok class=ok style=display:none></div><div id=status role=status></div>"
"<script>"
"const $=i=>document.getElementById(i),st=(c,h)=>{let s=$('status');s.className=c;s.style.display='block';s.innerHTML=h};"
"const esc=t=>String(t).replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';');"
"fetch('/api/setup/info').then(r=>r.json()).then(i=>{if(i.ssid){$('ssid').value=i.ssid;$('wkeep').textContent='Leave the password empty to keep the saved one.'}"
"let m=i.mqtt||{};$('mhost').value=m.host||'';if(m.port)$('mport').value=m.port;$('mtls').checked=!!m.tls;$('muser').value=m.username||'';"
"$('ha').checked=m.home_assistant!==false;$('rcpt').value=m.default_recipient||'';if(m.has_password)$('mkeep').textContent='Leave empty to keep the saved password.';"
"if(i.has_token)$('tkeep').textContent='Leave empty to keep the current token.';"
"if(i.last_crash||i.safe_mode){let d=$('diag');d.style.display='block';d.innerHTML=(i.safe_mode?'<b>Safe mode</b> after repeated crashes (modem disabled for this boot).<br>':'')+"
"(i.last_crash?'Last crash: <code>'+esc(i.last_crash)+'</code><br>':'')+'Reset reason: '+esc(i.reset_reason)}}).catch(()=>{});"
"$('scan').onclick=async()=>{let b=$('scan');b.disabled=true;b.textContent='Scanning...';"
"try{let n=await (await fetch('/api/setup/scan')).json();$('nets').innerHTML=n.map(x=>'<option value=\"'+esc(x.ssid)+'\">'+x.rssi+' dBm'+(x.secure?'':' open')+'</option>').join('');"
"b.textContent=n.length+' networks found';}catch(e){b.textContent='Scan failed'}b.disabled=false};"
"async function poll(){try{let s=await (await fetch('/api/setup/status')).json();"
"if(s.state=='connected'){st('ok','Connected to <b>'+esc(s.ssid)+'</b> with IP <b>'+esc(s.ip)+'</b>. The gateway restarts now and leaves setup mode.');return}"
"if(s.state=='failed'){st('err','Could not connect to <b>'+esc(s.ssid)+'</b> (reason '+s.reason+'). Check the network name and password, then save again.');$('save').disabled=false;return}"
"}catch(e){st('busy','Connecting... if this page stops updating, your phone left the setup network while the gateway switched channels. The display shows the result.')}"
"setTimeout(poll,2000)}"
"$('f').onsubmit=async e=>{e.preventDefault();$('save').disabled=true;st('busy','Saving...');"
"let b={ssid:$('ssid').value,password:$('wpass').value,mqtt_host:$('mhost').value.trim(),mqtt_port:+$('mport').value,mqtt_tls:$('mtls').checked,"
"mqtt_username:$('muser').value,mqtt_password:$('mpass').value,home_assistant:$('ha').checked,default_recipient:$('rcpt').value.trim(),token:$('token').value};"
"try{let r=await fetch('/api/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});let j=await r.json();"
"if(!r.ok){st('err',esc(j.error||'Setup failed'));$('save').disabled=false;return}"
"if(j.token){let t=$('tok');t.style.display='block';t.style.padding='.7rem';t.innerHTML='Your new REST API token. It is shown only once, save it now:<br><code>'+esc(j.token)+'</code>'}"
"st('busy','Saved. Connecting to <b>'+esc(b.ssid)+'</b>...');setTimeout(poll,2000)}"
"catch(e){st('err','Request failed: '+esc(e));$('save').disabled=false}};"
"</script></html>";

static esp_err_t send_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORTAL_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *json)
{
    char *encoded = json != NULL ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (encoded == NULL) return httpd_resp_send_500(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, encoded);
    gateway_security_wipe(encoded, strlen(encoded));
    cJSON_free(encoded);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    if (json != NULL) cJSON_AddStringToObject(json, "error", message);
    return send_json(req, status, json);
}

/* Parses "mqtt[s]://host[:port]" back into the form fields. */
static void split_broker_uri(const char *uri, char *host, size_t host_size, int *port, bool *tls)
{
    host[0] = '\0';
    *tls = gateway_mqtt_uri_is_tls(uri);
    *port = *tls ? 8883 : 1883;
    const char *start = strstr(uri, "://");
    if (start == NULL) return;
    start += 3;
    const char *colon = strrchr(start, ':');
    const size_t len = colon != NULL ? (size_t)(colon - start) : strcspn(start, "/");
    snprintf(host, host_size, "%.*s", (int)(len < host_size ? len : host_size - 1), start);
    if (colon != NULL) *port = atoi(colon + 1);
}

static esp_err_t info_handler(httpd_req_t *req)
{
    gateway_mqtt_config_t *mqtt = calloc(1, sizeof(*mqtt));
    if (mqtt == NULL) return httpd_resp_send_500(req);
    const bool have_mqtt = gateway_settings_get_mqtt(mqtt) == ESP_OK;
    network_service_snapshot_t net = {0};
    (void)network_service_get_snapshot(&net);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "device_id", network_service_device_id());
    cJSON_AddStringToObject(json, "ssid", net.sta_ssid);
    cJSON_AddBoolToObject(json, "has_token", gateway_security_has_token());
    cJSON_AddStringToObject(json, "reset_reason", gateway_diag_reset_reason());
    cJSON_AddStringToObject(json, "last_crash", gateway_diag_last_crash(NULL));
    cJSON_AddNumberToObject(json, "safe_mode", gateway_diag_safe_mode());
    cJSON *m = cJSON_AddObjectToObject(json, "mqtt");
    if (have_mqtt && m != NULL) {
        char host[PORTAL_HOST_MAX + 1];
        int port = 1883;
        bool tls = false;
        split_broker_uri(mqtt->broker_uri, host, sizeof(host), &port, &tls);
        cJSON_AddStringToObject(m, "host", mqtt->enabled ? host : "");
        cJSON_AddNumberToObject(m, "port", port);
        cJSON_AddBoolToObject(m, "tls", tls);
        cJSON_AddStringToObject(m, "username", mqtt->username);
        cJSON_AddBoolToObject(m, "has_password", mqtt->password[0] != '\0');
        cJSON_AddBoolToObject(m, "home_assistant", mqtt->home_assistant_enabled);
        cJSON_AddStringToObject(m, "default_recipient", mqtt->default_recipient);
        cJSON_AddStringToObject(m, "base_topic", mqtt->base_topic);
    }
    gateway_security_wipe(mqtt, sizeof(*mqtt));
    free(mqtt);
    gateway_security_wipe(net.portal_passphrase, sizeof(net.portal_passphrase));
    return send_json(req, "200 OK", json);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    const wifi_scan_config_t scan = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) return send_error(req, "503 Service Unavailable", "Wi-Fi scan failed");
    uint16_t count = PORTAL_SCAN_MAX;
    wifi_ap_record_t *records = calloc(PORTAL_SCAN_MAX, sizeof(*records));
    if (records == NULL) {
        (void)esp_wifi_clear_ap_list();
        return httpd_resp_send_500(req);
    }
    err = esp_wifi_scan_get_ap_records(&count, records);
    cJSON *list = cJSON_CreateArray();
    for (uint16_t i = 0; err == ESP_OK && i < count; ++i) {
        if (records[i].ssid[0] == '\0') continue;
        bool duplicate = false;
        for (uint16_t j = 0; j < i && !duplicate; ++j) {
            duplicate = strcmp((const char *)records[j].ssid, (const char *)records[i].ssid) == 0;
        }
        if (duplicate) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddBoolToObject(item, "secure", records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(list, item);
    }
    free(records);
    return send_json(req, "200 OK", list);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    network_service_snapshot_t net = {0};
    (void)network_service_get_snapshot(&net);
    gateway_security_wipe(net.portal_passphrase, sizeof(net.portal_passphrase));
    static const char *const names[] = {"idle", "connecting", "connected", "failed"};
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "state", names[net.setup_state]);
    cJSON_AddStringToObject(json, "ssid", net.sta_ssid);
    cJSON_AddStringToObject(json, "ip", net.ipv4);
    cJSON_AddNumberToObject(json, "reason", net.last_disconnect_reason);
    return send_json(req, "200 OK", json);
}

static const char *json_string(const cJSON *json, const char *name, size_t max_len, bool *valid)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (item == NULL || cJSON_IsNull(item)) return "";
    if (!cJSON_IsString(item) || item->valuestring == NULL || strlen(item->valuestring) > max_len) {
        *valid = false;
        return "";
    }
    return item->valuestring;
}

static bool host_valid(const char *host)
{
    const size_t len = strlen(host);
    if (len == 0 || len > PORTAL_HOST_MAX) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)host[i];
        if (!isalnum(c) && c != '.' && c != '-' && c != '_') return false;
    }
    return true;
}

/* Builds the new MQTT config from the form; empty secrets keep the saved values. */
static const char *apply_mqtt_form(const cJSON *json, gateway_mqtt_config_t *config)
{
    bool valid = true;
    const char *host = json_string(json, "mqtt_host", PORTAL_HOST_MAX, &valid);
    const char *username = json_string(json, "mqtt_username", MQTT_USERNAME_MAX - 1, &valid);
    const char *password = json_string(json, "mqtt_password", MQTT_PASSWORD_MAX - 1, &valid);
    const char *recipient = json_string(json, "default_recipient", MQTT_DEFAULT_RECIPIENT_MAX - 1, &valid);
    const cJSON *port_item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_port");
    const cJSON *tls_item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_tls");
    const cJSON *ha_item = cJSON_GetObjectItemCaseSensitive(json, "home_assistant");
    if (!valid) return "MQTT field too long";

    snprintf(config->default_recipient, sizeof(config->default_recipient), "%s", recipient);
    if (ha_item != NULL) config->home_assistant_enabled = cJSON_IsTrue(ha_item);
    if (host[0] == '\0') {
        config->enabled = false;
        return NULL;
    }
    if (!host_valid(host)) return "MQTT broker host may only contain letters, digits, '.', '-' and '_'";
    const bool tls = cJSON_IsTrue(tls_item);
    const int port = cJSON_IsNumber(port_item) ? port_item->valueint : (tls ? 8883 : 1883);
    if (port < 1 || port > 65535) return "MQTT port must be 1-65535";

    char previous_host[PORTAL_HOST_MAX + 1];
    int previous_port = 0;
    bool previous_tls = false;
    split_broker_uri(config->broker_uri, previous_host, sizeof(previous_host), &previous_port, &previous_tls);
    const bool same_broker = config->enabled && strcmp(previous_host, host) == 0;

    snprintf(config->broker_uri, sizeof(config->broker_uri), "%s://%s:%d", tls ? "mqtts" : "mqtt", host, port);
    snprintf(config->username, sizeof(config->username), "%s", username);
    if (password[0] != '\0') {
        snprintf(config->password, sizeof(config->password), "%s", password);
    } else if (!same_broker) {
        config->password[0] = '\0';
    }
    if (!tls) config->ca_pem[0] = '\0';
    config->enabled = true;
    return NULL;
}

static esp_err_t setup_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= PORTAL_BODY_MAX) {
        return send_error(req, "400 Bad Request", "Invalid setup request");
    }
    char *body = calloc(1, PORTAL_BODY_MAX);
    gateway_mqtt_config_t *mqtt = calloc(1, sizeof(*mqtt));
    if (body == NULL || mqtt == NULL) {
        free(body);
        free(mqtt);
        return httpd_resp_send_500(req);
    }
    int received = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            gateway_security_wipe(body, PORTAL_BODY_MAX);
            free(body);
            free(mqtt);
            return ESP_FAIL;
        }
        received += n;
    }
    cJSON *json = cJSON_ParseWithLength(body, (size_t)received);
    gateway_security_wipe(body, PORTAL_BODY_MAX);
    free(body);

    const char *error = NULL;
    bool valid = json != NULL;
    const char *ssid = valid ? json_string(json, "ssid", 32, &valid) : "";
    const char *password = valid ? json_string(json, "password", 64, &valid) : "";
    const char *token = valid ? json_string(json, "token", 128, &valid) : "";
    network_service_snapshot_t net = {0};
    (void)network_service_get_snapshot(&net);
    gateway_security_wipe(net.portal_passphrase, sizeof(net.portal_passphrase));
    char saved_password[65] = {0};

    if (!valid || ssid[0] == '\0') {
        error = "Enter the Wi-Fi network name (1-32 bytes).";
    } else if (password[0] != '\0' && strlen(password) < 8) {
        error = "Wi-Fi passwords have at least 8 characters.";
    } else if (token[0] != '\0' && strlen(token) < 32) {
        error = "The API token needs 32-128 characters (or leave it empty).";
    } else if (gateway_settings_get_mqtt(mqtt) != ESP_OK) {
        error = "Settings storage is not ready; try again in a few seconds.";
    } else if ((error = apply_mqtt_form(json, mqtt)) == NULL && gateway_mqtt_config_validate(mqtt) != ESP_OK) {
        error = "Invalid MQTT settings (the default recipient must look like +491701234567).";
    }
    if (error == NULL && password[0] == '\0' && strcmp(ssid, net.sta_ssid) == 0) {
        /* Same network and no new password: keep the stored one. */
        wifi_config_t current = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &current) == ESP_OK) {
            memcpy(saved_password, current.sta.password, sizeof(current.sta.password));
        }
        gateway_security_wipe(&current, sizeof(current));
        password = saved_password;
    }

    char generated[GATEWAY_API_TOKEN_HEX_LEN + 1] = {0};
    if (error == NULL && gateway_settings_set_mqtt(mqtt) != ESP_OK) error = "Could not save MQTT settings.";
    if (error == NULL && token[0] != '\0' && gateway_security_set_token(token) != ESP_OK) error = "Could not save the API token.";
    if (error == NULL && token[0] == '\0' && !gateway_security_has_token() &&
        gateway_security_generate_token(generated, sizeof(generated)) != ESP_OK) {
        error = "Could not generate an API token.";
    }
    if (error == NULL && network_service_submit_credentials(ssid, password) != ESP_OK) {
        error = "Could not apply the Wi-Fi settings.";
    }
    gateway_security_wipe(saved_password, sizeof(saved_password));
    gateway_security_wipe(mqtt, sizeof(*mqtt));
    free(mqtt);
    if (json != NULL) {
        /* Wipe secrets held inside the parsed tree before freeing it. */
        static const char *const secrets[] = {"password", "token", "mqtt_password"};
        for (size_t i = 0; i < sizeof(secrets) / sizeof(secrets[0]); ++i) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(json, secrets[i]);
            if (cJSON_IsString(item) && item->valuestring != NULL) {
                gateway_security_wipe(item->valuestring, strlen(item->valuestring));
            }
        }
        cJSON_Delete(json);
    }
    if (error != NULL) return send_error(req, "400 Bad Request", error);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "saved", true);
    if (generated[0] != '\0') cJSON_AddStringToObject(out, "token", generated);
    gateway_security_wipe(generated, sizeof(generated));
    ESP_LOGI(TAG, "setup saved; testing Wi-Fi");
    return send_json(req, "200 OK", out);
}

static void dns_task(void *arg)
{
    (void)arg;
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
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
                /* Answer every A query with the portal address. */
                packet[2] = 0x81; packet[3] = 0x80; packet[6] = 0; packet[7] = 1;
                memset(packet + 8, 0, 4);
                uint8_t *answer = packet + question_end;
                const uint8_t record[] = {0xc0,0x0c,0,1,0,1,0,0,0,30,0,4,192,168,4,1};
                memcpy(answer, record, sizeof(record));
                (void)sendto(fd, packet, question_end + sizeof(record), 0, (struct sockaddr *)&peer, peer_len);
            }
        } else {
            ESP_LOGW(TAG, "captive DNS could not bind port 53");
        }
        close(fd);
    }
    s_dns_running = false;
    vTaskDelete(NULL);
}

esp_err_t provisioning_portal_start(void)
{
    if (s_running) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 8;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &config), TAG, "start HTTP portal");
    /* Specific routes first: the wildcard captive route must match last. */
    const httpd_uri_t routes[] = {
        {.uri = "/api/setup/info", .method = HTTP_GET, .handler = info_handler},
        {.uri = "/api/setup/scan", .method = HTTP_GET, .handler = scan_handler},
        {.uri = "/api/setup/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/setup", .method = HTTP_POST, .handler = setup_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = send_page},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        if (httpd_register_uri_handler(s_httpd, &routes[i]) != ESP_OK) {
            httpd_stop(s_httpd);
            s_httpd = NULL;
            return ESP_FAIL;
        }
    }
    s_running = true;
    s_dns_running = true;
    if (xTaskCreate(dns_task, "portal_dns", 3072, NULL, 4, NULL) != pdPASS) {
        s_dns_running = false;
        ESP_LOGW(TAG, "captive DNS unavailable; browse to http://192.168.4.1 manually");
    }
    ESP_LOGI(TAG, "setup portal listening on http://192.168.4.1");
    return ESP_OK;
}

void provisioning_portal_stop(void)
{
    s_running = false;
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    /* Let the DNS task leave recvfrom() so a later restart can bind port 53 again. */
    for (int waited = 0; s_dns_running && waited < DNS_TASK_EXIT_WAIT_MS; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
