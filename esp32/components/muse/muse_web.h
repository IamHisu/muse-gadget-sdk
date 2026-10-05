#pragma once

#include "esp_err.h"

/* Starts the local Hisu setup page and REST API on port 80. The server binds
 * before Wi-Fi has an address and becomes reachable as soon as STA connects. */
esp_err_t muse_web_start(void);
