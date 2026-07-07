#pragma once

// HTTP + WebSocket control/stream server for the networked car.
//
// Launches two esp_http_server instances (the standard ESP32 CameraWebServer
// split so a long-lived MJPEG stream never starves control):
//   - HTTP_PORT   (80): GET /  test page, GET /control WebSocket,
//                        GET /capture single JPEG, GET /status JSON.
//   - STREAM_PORT (81): GET /stream  multipart MJPEG.
// The WebSocket handler translates discrete JSON commands into protocol byte
// values and feeds command_set(). Full wire contract is in NETWORK_API.md.

// Starts both server instances. Call after wifi_sta_start() and camera_init().
void web_server_start(void);
