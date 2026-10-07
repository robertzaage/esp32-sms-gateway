#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t api_server_init(void);
/** Stop the management API (frees port 80 for the setup portal). Safe when stopped. */
void api_server_stop(void);
bool api_server_running(void);
#ifdef __cplusplus
}
#endif
