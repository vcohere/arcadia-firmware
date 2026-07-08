#pragma once

// User-editable configuration for the networked WLtoys 6401 firmware.
//
// The two WiFi lines below are the ones you normally touch; everything else is
// a tunable with a sensible default. See NETWORK_API.md for how the control
// and stream endpoints behave.

// --- WiFi station credentials (2.4 GHz network) ---------------------------
#define WIFI_SSID     "@Wyne438_2.4G"
#define WIFI_PASSWORD "Val438vv"

// --- Camera ---------------------------------------------------------------
// FRAMESIZE_VGA = 640x480. Drop to FRAMESIZE_QVGA (320x240) for lower latency
// on a congested link. Valid values come from esp_camera.h (sensor.h).
#define CAM_FRAME_SIZE   FRAMESIZE_QVGA
// JPEG quality: lower number = higher quality + bigger frames (0..63).
#define CAM_JPEG_QUALITY 12

// --- Control failsafe -----------------------------------------------------
// If no fresh WebSocket command arrives within this window, command_get()
// returns neutral so the car coasts to a stop when the webapp disconnects or
// stalls. Keep comfortably above the webapp's send cadence (~10 Hz).
#define CONTROL_FAILSAFE_MS 800

// --- HTTP servers ---------------------------------------------------------
// Port 80 hosts the control API + WebSocket + test page; port 81 hosts the
// long-lived MJPEG stream on its own server instance so streaming never
// starves control (the standard ESP32 CameraWebServer split).
#define HTTP_PORT   80
#define STREAM_PORT 81
