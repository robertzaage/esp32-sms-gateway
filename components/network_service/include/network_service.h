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

typedef struct {
    bool initialized;
    bool provisioned;
    bool provisioning;
    bool connected;
    bool time_synced;
    uint32_t reconnects;
    uint32_t disconnects;
    int last_disconnect_reason;
    char device_id[NETWORK_DEVICE_ID_MAX];
    char hostname[NETWORK_HOSTNAME_MAX];
    char ipv4[NETWORK_IPV4_MAX];
} network_service_snapshot_t;

typedef void (*network_service_event_callback_t)(const network_service_snapshot_t *snapshot,
                                                 void *user_ctx);

esp_err_t network_service_init(network_service_event_callback_t cb, void *user_ctx);
esp_err_t network_service_get_snapshot(network_service_snapshot_t *out);
bool network_service_is_online(void);
const char *network_service_device_id(void);
const char *network_service_hostname(void);

#ifdef __cplusplus
}
#endif
