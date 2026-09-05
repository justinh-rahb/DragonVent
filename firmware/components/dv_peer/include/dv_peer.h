// SPDX-License-Identifier: MIT
#pragma once
//
// dv_peer — DragonVent's dc_peer provider. Broadcasts the ANNOUNCE descriptor (so a
// console can discover and label the Vent) plus the VENT status capability. Status
// only: it publishes, it never accepts commands. Start after dc_peer_start().

#include "esp_err.h"

esp_err_t dv_peer_start(void);
