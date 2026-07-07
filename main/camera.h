#pragma once

#include "esp_err.h"
#include "esp_camera.h"

// Camera bring-up for the Seeed Studio XIAO ESP32-S3 Sense onboard OV2640/
// OV3660. Pin map is fixed by the board (internal-only GPIOs, see camera.c) and
// does not collide with the car signal line on GPIO4. Frame size and JPEG
// quality come from config.h. Framebuffers live in the board's octal PSRAM.

// Initializes the camera in JPEG mode with 2 PSRAM framebuffers. Returns
// ESP_OK on success; on failure the web server still starts so the device
// stays reachable for control, but stream/capture endpoints report the error.
esp_err_t camera_init(void);

// True once camera_init() succeeded.
bool camera_ready(void);

// Thin pass-throughs over esp_camera so the web handlers don't include the
// full driver header directly. camera_capture() returns NULL if the camera is
// not ready or a frame could not be grabbed; the caller must camera_return()
// every non-NULL frame.
camera_fb_t *camera_capture(void);
void camera_return(camera_fb_t *fb);
