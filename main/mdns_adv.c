#include "mdns_adv.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"

#include "config.h"

static const char *TAG = "mdns_adv";

// Two-step stringify so the numeric macro value lands in the TXT record.
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

void mdns_adv_start(void) {
    ESP_ERROR_CHECK(mdns_init());

    // The instance name doubles as the car's stable car_id: the relay's
    // allowlist, logs, and metrics all key on this exact string (see
    // NETWORK_API.md §7). Derived from the STA MAC so it survives reflashes.
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char car_id[32];
    snprintf(car_id, sizeof(car_id), "arcadia-car-%02x%02x%02x", mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(mdns_hostname_set(car_id));
    ESP_ERROR_CHECK(mdns_instance_name_set(car_id));

    mdns_txt_item_t txt[] = {
        { "version", "1" },
        { "capture_port", STRINGIFY(STREAM_PORT) },
    };
    ESP_ERROR_CHECK(mdns_service_add(car_id, "_arcadia-car", "_tcp", HTTP_PORT,
                                     txt, sizeof(txt) / sizeof(txt[0])));

    ESP_LOGI(TAG, "advertising _arcadia-car._tcp as \"%s\" (port %d, capture on %d)",
             car_id, HTTP_PORT, STREAM_PORT);
}
