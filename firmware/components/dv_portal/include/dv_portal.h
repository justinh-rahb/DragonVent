#pragma once

// DragonVent config web UI. Serves the same page in both AP and STA modes so
// initial setup and later reconfiguration use the same URL. In AP mode it
// also runs a DNS redirector so mobile OSes pop the "Sign in to network"
// prompt automatically.
//
// Call after dc_wifi_start() (and ideally after dc_moonraker_start() and
// dv_policy_start(), so pre-fill values are correct). The portal decides
// AP vs. STA behavior from dc_wifi_state().

#include "esp_err.h"

esp_err_t dv_portal_start(void);
esp_err_t dv_portal_stop(void);
