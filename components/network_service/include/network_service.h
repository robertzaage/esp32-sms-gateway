#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETWORK_DEVICE_ID_MAX 24
#define NETWORK_HOSTNAME_MAX 40
#define NETWORK_IPV4_MAX 16
#define NETWORK_SSID_MAX 33
#define NETWORK_PORTAL_SSID_MAX 33
#define NETWORK_PORTAL_PASSWORD_MAX 17

typedef enum {
    NETWORK_PORTAL_CLOSED = 0,
    NETWORK_PORTAL_FIRST_SETUP, /* no saved Wi-Fi credentials */
    NETWORK_PORTAL_FALLBACK,    /* saved network unreachable for a while; STA keeps retrying */
    NETWORK_PORTAL_MANUAL,      /* opened from the board button */
} network_portal_reason_t;

typedef enum {
    NETWORK_SETUP_IDLE = 0,
    NETWORK_SETUP_CONNECTING, /* portal submitted new credentials, testing them */
    NETWORK_SETUP_CONNECTED,  /* credentials work; gateway restarts shortly */
    NETWORK_SETUP_FAILED,     /* credentials did not work; portal stays open */
} network_setup_state_t;

typedef struct {
    bool initialized;
    bool provisioned;
    bool provisioning; /* setup portal (SoftAP) is open */
    bool connected;
    bool time_synced;
    network_portal_reason_t portal_reason;
    network_setup_state_t setup_state;
    uint32_t reconnects;
    uint32_t disconnects;
    int last_disconnect_reason;
    char device_id[NETWORK_DEVICE_ID_MAX];
    char hostname[NETWORK_HOSTNAME_MAX];
    char ipv4[NETWORK_IPV4_MAX];
    char sta_ssid[NETWORK_SSID_MAX];
    char portal_ssid[NETWORK_PORTAL_SSID_MAX];
    char portal_passphrase[NETWORK_PORTAL_PASSWORD_MAX];
} network_service_snapshot_t;

typedef void (*network_service_event_callback_t)(const network_service_snapshot_t *snapshot,
                                                 void *user_ctx);

typedef struct {
    /* Called from the network task whenever the snapshot changes. Must not block. */
    network_service_event_callback_t on_change;
    /*
     * Called synchronously before the setup portal binds port 80. The management
     * API shares that port and must be stopped here.
     */
    void (*before_portal_start)(void *user_ctx);
    void *user_ctx;
} network_service_config_t;

esp_err_t network_service_init(const network_service_config_t *config);
esp_err_t network_service_get_snapshot(network_service_snapshot_t *out);
bool network_service_is_online(void);
/** Stable identity derived from the Wi-Fi MAC; valid before network_service_init(). */
const char *network_service_device_id(void);
const char *network_service_hostname(void);

/** Open the setup portal on request (board button). The STA link stays up. */
esp_err_t network_service_open_portal(void);

/**
 * Store new STA credentials from the setup portal and start testing them.
 * Progress is reported through snapshot.setup_state.
 */
esp_err_t network_service_submit_credentials(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
