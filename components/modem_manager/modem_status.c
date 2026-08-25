#include "modem_status.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_space(const char *p)
{
    while (p != NULL && (*p == ' ' || *p == '\t')) {
        ++p;
    }
    return p;
}

static bool parse_int(const char **cursor, int *value)
{
    if (cursor == NULL || *cursor == NULL || value == NULL) {
        return false;
    }
    const char *p = skip_space(*cursor);
    char *end = NULL;
    const long parsed = strtol(p, &end, 10);
    if (end == p || parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    *value = (int)parsed;
    *cursor = end;
    return true;
}

static bool consume_char(const char **cursor, char expected)
{
    if (cursor == NULL || *cursor == NULL) {
        return false;
    }
    const char *p = skip_space(*cursor);
    if (*p != expected) {
        return false;
    }
    *cursor = p + 1;
    return true;
}

static bool has_prefix(const char *line, const char *prefix)
{
    return line != NULL && prefix != NULL && strncmp(line, prefix, strlen(prefix)) == 0;
}

bool modem_status_parse_cpin(const char *line, modem_sim_state_t *out)
{
    static const char prefix[] = "+CPIN:";
    if (!has_prefix(line, prefix) || out == NULL) {
        return false;
    }

    const char *value = skip_space(line + sizeof(prefix) - 1);
    if (strcmp(value, "READY") == 0) {
        *out = MODEM_SIM_READY;
    } else if (strcmp(value, "SIM PIN") == 0) {
        *out = MODEM_SIM_PIN_REQUIRED;
    } else if (strcmp(value, "SIM PUK") == 0) {
        *out = MODEM_SIM_PUK_REQUIRED;
    } else if (strcmp(value, "NOT INSERTED") == 0 || strcmp(value, "SIM NOT INSERTED") == 0) {
        *out = MODEM_SIM_NOT_INSERTED;
    } else if (strcmp(value, "SIM BLOCK") == 0 || strcmp(value, "SIM BLOCKED") == 0) {
        *out = MODEM_SIM_BLOCKED;
    } else {
        *out = MODEM_SIM_ERROR;
    }
    return true;
}

bool modem_status_parse_csq(const char *line, modem_signal_t *out)
{
    static const char prefix[] = "+CSQ:";
    if (!has_prefix(line, prefix) || out == NULL) {
        return false;
    }

    const char *cursor = line + sizeof(prefix) - 1;
    int rssi = 0;
    int ber = 0;
    if (!parse_int(&cursor, &rssi) || !consume_char(&cursor, ',') || !parse_int(&cursor, &ber)) {
        return false;
    }
    cursor = skip_space(cursor);
    if (*cursor != '\0' || !((rssi >= 0 && rssi <= 31) || rssi == 99) ||
        !((ber >= 0 && ber <= 7) || ber == 99)) {
        return false;
    }

    out->rssi = rssi;
    out->ber = ber;
    out->rssi_dbm = rssi == 99 ? INT16_MIN : -113 + (2 * rssi);
    return true;
}

bool modem_status_parse_registration(const char *line,
                                     const char *prefix,
                                     modem_registration_t *out)
{
    if (!has_prefix(line, prefix) || out == NULL) {
        return false;
    }

    const char *cursor = line + strlen(prefix);
    int first = 0;
    if (!parse_int(&cursor, &first)) {
        return false;
    }

    int status = first;
    int act = -1;
    cursor = skip_space(cursor);
    if (*cursor == ',') {
        ++cursor;
        const char *second_start = skip_space(cursor);
        if (*second_start != '"') {
            int second = 0;
            const char *after_second = second_start;
            if (parse_int(&after_second, &second)) {
                /* Query form is +xREG: <n>,<stat>[,...]. */
                if (first >= 0 && first <= 2 && second >= 0 && second <= 5) {
                    status = second;
                    cursor = after_second;
                } else {
                    /* URC form can start directly with <stat>. */
                    status = first;
                    cursor = second_start;
                }
            } else {
                cursor = second_start;
            }
        } else {
            cursor = second_start;
        }
    }

    /* Access technology, when present, is the final numeric field after LAC/CI. */
    const char *last_comma = strrchr(cursor, ',');
    if (last_comma != NULL) {
        const char *act_cursor = last_comma + 1;
        int parsed_act = -1;
        if (parse_int(&act_cursor, &parsed_act) && *skip_space(act_cursor) == '\0') {
            act = parsed_act;
        }
    }

    if (status < 0 || status > 5) {
        return false;
    }
    out->status = (modem_registration_status_t)status;
    out->access_technology = act;
    return true;
}

bool modem_status_parse_operator(const char *line, modem_operator_t *out)
{
    static const char prefix[] = "+COPS:";
    if (!has_prefix(line, prefix) || out == NULL) {
        return false;
    }

    modem_operator_t parsed = {
        .mode = -1,
        .format = -1,
        .access_technology = -1,
    };
    const char *cursor = line + sizeof(prefix) - 1;
    if (!parse_int(&cursor, &parsed.mode)) {
        return false;
    }

    cursor = skip_space(cursor);
    if (*cursor == '\0') {
        *out = parsed;
        return true;
    }
    if (!consume_char(&cursor, ',') || !parse_int(&cursor, &parsed.format) || !consume_char(&cursor, ',')) {
        return false;
    }

    cursor = skip_space(cursor);
    if (*cursor != '"') {
        return false;
    }
    ++cursor;
    size_t length = 0;
    while (*cursor != '\0' && *cursor != '"') {
        if (length + 1 < sizeof(parsed.name)) {
            parsed.name[length++] = *cursor;
        }
        ++cursor;
    }
    if (*cursor != '"') {
        return false;
    }
    parsed.name[length] = '\0';
    ++cursor;

    cursor = skip_space(cursor);
    if (*cursor == ',') {
        ++cursor;
        if (!parse_int(&cursor, &parsed.access_technology)) {
            return false;
        }
    }
    if (*skip_space(cursor) != '\0') {
        return false;
    }

    *out = parsed;
    return true;
}

bool modem_registration_is_registered(modem_registration_status_t status)
{
    return status == MODEM_REG_HOME || status == MODEM_REG_ROAMING;
}

bool modem_registration_is_roaming(modem_registration_status_t status)
{
    return status == MODEM_REG_ROAMING;
}

const char *modem_sim_state_name(modem_sim_state_t state)
{
    switch (state) {
    case MODEM_SIM_UNKNOWN: return "unknown";
    case MODEM_SIM_READY: return "ready";
    case MODEM_SIM_PIN_REQUIRED: return "pin_required";
    case MODEM_SIM_PUK_REQUIRED: return "puk_required";
    case MODEM_SIM_NOT_INSERTED: return "not_inserted";
    case MODEM_SIM_BLOCKED: return "blocked";
    case MODEM_SIM_ERROR: return "error";
    default: return "unknown";
    }
}

const char *modem_registration_status_name(modem_registration_status_t status)
{
    switch (status) {
    case MODEM_REG_NOT_REGISTERED: return "not_registered";
    case MODEM_REG_HOME: return "home";
    case MODEM_REG_SEARCHING: return "searching";
    case MODEM_REG_DENIED: return "denied";
    case MODEM_REG_UNKNOWN_NETWORK: return "unknown_network";
    case MODEM_REG_ROAMING: return "roaming";
    case MODEM_REG_UNKNOWN:
    default: return "unknown";
    }
}
