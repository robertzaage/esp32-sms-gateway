#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODEM_OPERATOR_NAME_MAX 64

typedef enum {
    MODEM_SIM_UNKNOWN = 0,
    MODEM_SIM_READY,
    MODEM_SIM_PIN_REQUIRED,
    MODEM_SIM_PUK_REQUIRED,
    MODEM_SIM_NOT_INSERTED,
    MODEM_SIM_BLOCKED,
    MODEM_SIM_ERROR,
} modem_sim_state_t;

typedef enum {
    MODEM_REG_UNKNOWN = -1,
    MODEM_REG_NOT_REGISTERED = 0,
    MODEM_REG_HOME = 1,
    MODEM_REG_SEARCHING = 2,
    MODEM_REG_DENIED = 3,
    MODEM_REG_UNKNOWN_NETWORK = 4,
    MODEM_REG_ROAMING = 5,
} modem_registration_status_t;

typedef struct {
    modem_registration_status_t status;
    int access_technology;
} modem_registration_t;

typedef struct {
    int rssi;      /* 0..31, or 99 when unknown */
    int rssi_dbm;  /* INT16_MIN when unknown */
    int ber;       /* 0..7, or 99 when unknown */
} modem_signal_t;

typedef struct {
    int mode;
    int format;
    char name[MODEM_OPERATOR_NAME_MAX];
    int access_technology;
} modem_operator_t;

bool modem_status_parse_cpin(const char *line, modem_sim_state_t *out);
bool modem_status_parse_csq(const char *line, modem_signal_t *out);
bool modem_status_parse_registration(const char *line,
                                     const char *prefix,
                                     modem_registration_t *out);
bool modem_status_parse_operator(const char *line, modem_operator_t *out);

bool modem_registration_is_registered(modem_registration_status_t status);
bool modem_registration_is_roaming(modem_registration_status_t status);
const char *modem_sim_state_name(modem_sim_state_t state);
const char *modem_registration_status_name(modem_registration_status_t status);

#ifdef __cplusplus
}
#endif
