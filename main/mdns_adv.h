#pragma once

// Advertises this car on the LAN over mDNS so the relay can discover it
// without hardcoded IPs. Call once after wifi_sta_start(); safe to call
// before the IP is acquired (the mdns component tracks netif events itself).
void mdns_adv_start(void);
