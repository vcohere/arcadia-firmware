#include "camera.h"

#include "esp_log.h"
#include "config.h"

static const char *TAG = "camera";

static bool s_ready = false;

// XIAO ESP32-S3 Sense camera pin map. These are all internal-only GPIOs wired
// on the board to the OV2640/OV3660; none are broken out to pads, and none
// collide with GPIO4 (car signal). See the plan's pin-finding table.
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  10
#define CAM_PIN_SIOD  40
#define CAM_PIN_SIOC  39
#define CAM_PIN_D7    48
#define CAM_PIN_D6    11
#define CAM_PIN_D5    12
#define CAM_PIN_D4    14
#define CAM_PIN_D3    16
#define CAM_PIN_D2    18
#define CAM_PIN_D1    17
#define CAM_PIN_D0    15
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF  47
#define CAM_PIN_PCLK  13

esp_err_t camera_init(void) {
    camera_config_t cfg = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = CAM_FRAME_SIZE,
        .jpeg_quality = CAM_JPEG_QUALITY,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        // GRAB_LATEST drops stale buffered frames so the stream shows the
        // newest frame after a link hiccup instead of lagging permanently.
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)", err, esp_err_to_name(err));
        s_ready = false;
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "camera ready (JPEG, frame_size=%d, quality=%d)",
             CAM_FRAME_SIZE, CAM_JPEG_QUALITY);
    return ESP_OK;
}

bool camera_ready(void) {
    return s_ready;
}

camera_fb_t *camera_capture(void) {
    if (!s_ready) {
        return NULL;
    }
    return esp_camera_fb_get();
}

void camera_return(camera_fb_t *fb) {
    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }
}
