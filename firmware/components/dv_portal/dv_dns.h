#pragma once

// Minimal UDP DNS server that answers every A-record with a fixed IPv4 address
// (the AP gateway). Used by the captive portal to redirect all name lookups to
// the config page.

#include "esp_err.h"
#include <stdint.h>

esp_err_t dv_dns_start(uint32_t redirect_ip);   // ip in network byte order
void      dv_dns_stop(void);
