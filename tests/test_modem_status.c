#include "modem_status.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void test_cpin(void)
{
    modem_sim_state_t sim = MODEM_SIM_UNKNOWN;
    assert(modem_status_parse_cpin("+CPIN: READY", &sim));
    assert(sim == MODEM_SIM_READY);
    assert(modem_status_parse_cpin("+CPIN: SIM PIN", &sim));
    assert(sim == MODEM_SIM_PIN_REQUIRED);
    assert(modem_status_parse_cpin("+CPIN: SIM PUK", &sim));
    assert(sim == MODEM_SIM_PUK_REQUIRED);
    assert(modem_status_parse_cpin("+CPIN: NOT INSERTED", &sim));
    assert(sim == MODEM_SIM_NOT_INSERTED);
    assert(!modem_status_parse_cpin("+CSQ: 20,99", &sim));
}

static void test_csq(void)
{
    modem_signal_t signal;
    assert(modem_status_parse_csq("+CSQ: 19,99", &signal));
    assert(signal.rssi == 19);
    assert(signal.rssi_dbm == -75);
    assert(signal.ber == 99);

    assert(modem_status_parse_csq("+CSQ: 99,0", &signal));
    assert(signal.rssi_dbm == INT16_MIN);
    assert(!modem_status_parse_csq("+CSQ: 32,0", &signal));
    assert(!modem_status_parse_csq("+CSQ: 10,8", &signal));
}

static void test_registration(void)
{
    modem_registration_t reg;

    assert(modem_status_parse_registration("+CREG: 2,1,\"01AF\",\"1234ABCD\",2", "+CREG:", &reg));
    assert(reg.status == MODEM_REG_HOME);
    assert(reg.access_technology == 2);

    assert(modem_status_parse_registration("+CREG: 5,\"01AF\",\"1234ABCD\",2", "+CREG:", &reg));
    assert(reg.status == MODEM_REG_ROAMING);
    assert(reg.access_technology == 2);

    assert(modem_status_parse_registration("+CGREG: 1", "+CGREG:", &reg));
    assert(reg.status == MODEM_REG_HOME);
    assert(reg.access_technology == -1);

    assert(modem_status_parse_registration("+CEREG: 2,0", "+CEREG:", &reg));
    assert(reg.status == MODEM_REG_NOT_REGISTERED);

    assert(!modem_status_parse_registration("+CREG: 9", "+CREG:", &reg));
    assert(!modem_status_parse_registration("+COPS: 0", "+CREG:", &reg));
    assert(modem_registration_is_registered(MODEM_REG_HOME));
    assert(modem_registration_is_registered(MODEM_REG_ROAMING));
    assert(!modem_registration_is_registered(MODEM_REG_SEARCHING));
}

static void test_operator(void)
{
    modem_operator_t op;
    assert(modem_status_parse_operator("+COPS: 0,0,\"Telekom.de\",2", &op));
    assert(op.mode == 0);
    assert(op.format == 0);
    assert(strcmp(op.name, "Telekom.de") == 0);
    assert(op.access_technology == 2);

    assert(modem_status_parse_operator("+COPS: 0", &op));
    assert(op.mode == 0);
    assert(op.format == -1);
    assert(op.name[0] == '\0');
    assert(!modem_status_parse_operator("+COPS: 0,0,Telekom,2", &op));
}

int main(void)
{
    test_cpin();
    test_csq();
    test_registration();
    test_operator();
    puts("modem status parser tests passed");
    return 0;
}
