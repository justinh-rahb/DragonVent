#pragma once

// DragonVent API/product-settings adapter for the shared dc_portal service.
// Call after dc_wifi_start() and product policy initialization.

#include "esp_err.h"

esp_err_t dv_portal_start(void);
esp_err_t dv_portal_stop(void);
