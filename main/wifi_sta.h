#pragma once

#include <stdbool.h>
#include "esp_netif_ip_addr.h"

// Minimal WiFi station bring-up. Joins the network named by WIFI_SSID /
// WIFI_PASSWORD in config.h and keeps itself connected (auto-reconnect). No
// security beyond the AP's own WPA -- this device is meant to live on a trusted
// LAN, per the project decisions.

// Initializes NVS, the default event loop, netif, and the WiFi driver in STA
// mode, then starts connecting. Non-blocking: returns once the connect attempt
// has been kicked off. The acquired IP is logged when the GOT_IP event fires.
void wifi_sta_start(void);

// True once an IP has been acquired. Safe to call from any task.
bool wifi_sta_is_connected(void);

// Copies the current IPv4 address into *out. Returns false (and leaves *out
// untouched) if not yet connected.
bool wifi_sta_get_ip(esp_ip4_addr_t *out);
