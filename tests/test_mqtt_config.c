#include <assert.h>
#include <string.h>
#include "mqtt_config.h"

int main(void)
{
    gateway_mqtt_config_t c;
    gateway_mqtt_config_defaults(&c, "smsgw-a1b2c3");
    assert(!c.enabled);
    assert(strcmp(c.base_topic, "sms-gateway/smsgw-a1b2c3") == 0);
    assert(gateway_mqtt_config_validate(&c) == ESP_OK);

    c.enabled = true;
    strcpy(c.broker_uri, "mqtts://mqtt.example.com:8883");
    strcpy(c.default_recipient, "+491701234567");
    assert(gateway_mqtt_uri_is_tls(c.broker_uri));
    assert(gateway_mqtt_config_validate(&c) == ESP_OK);

    strcpy(c.broker_uri, "wss://mqtt.example.com/ws");
    assert(gateway_mqtt_config_validate(&c) == ESP_ERR_INVALID_ARG);
    strcpy(c.broker_uri, "mqtt://user:pass@example.com");
    assert(gateway_mqtt_config_validate(&c) == ESP_ERR_INVALID_ARG);
    strcpy(c.broker_uri, "mqtt://mqtt.example.com");
    strcpy(c.ca_pem, "-----BEGIN CERTIFICATE-----\nX\n-----END CERTIFICATE-----\n");
    assert(gateway_mqtt_config_validate(&c) == ESP_ERR_INVALID_ARG);
    c.ca_pem[0] = 0;
    strcpy(c.default_recipient, "01701234567");
    assert(gateway_mqtt_config_validate(&c) == ESP_ERR_INVALID_ARG);
    strcpy(c.default_recipient, "+491701234567");
    strcpy(c.base_topic, "bad/+/topic");
    assert(gateway_mqtt_config_validate(&c) == ESP_ERR_INVALID_ARG);
    return 0;
}
