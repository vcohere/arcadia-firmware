#include "web_server.h"

#include <string.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#include "camera.h"
#include "command.h"
#include "protocol.h"
#include "wifi_sta.h"
#include "config.h"

static const char *TAG = "web";

// Two independent server handles. Splitting /capture onto its own instance
// (STREAM_PORT) keeps a ~25 KB blocking JPEG send from occupying the single
// worker of the control server and starving /control and /status.
static httpd_handle_t s_http = NULL;   // port 80: control/API
static httpd_handle_t s_camera = NULL; // port 81: camera snapshot

// Two-step stringify so STREAM_PORT's numeric value (not the macro name) lands
// in the served HTML.
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

// --- Built-in test page ---------------------------------------------------
// Minimal so the board is usable before the separate webapp exists: shows a
// self-clocked snapshot loop (fetch /capture, render, fetch again -- bounds
// latency to one frame instead of letting a push stream's backlog build up)
// and drives /control over the WebSocket with a ~10 Hz repeat while a
// direction is held (pointer down), snapping back to neutral on release --
// matching the failsafe-friendly cadence documented in NETWORK_API.md. Kept
// as a single static string, no external assets (the CSP on this device is
// nonexistent but the page must work offline on the LAN).
static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>WLtoys 6401</title><style>"
"body{font-family:sans-serif;background:#111;color:#eee;text-align:center;margin:0;padding:12px}"
"img{max-width:100%;border-radius:8px;background:#000}"
"#pad{display:inline-grid;grid-template-columns:repeat(3,72px);gap:8px;margin-top:12px}"
"button{font-size:20px;padding:16px;border:0;border-radius:8px;background:#333;color:#eee;user-select:none;touch-action:none}"
"button:active{background:#0a7}"
"#s{margin-top:8px;font-size:13px;color:#9ad}"
".sp{visibility:hidden}"
"</style></head><body>"
"<h3>WLtoys 6401</h3>"
"<img id=v><div id=pad>"
"<span class=sp></span><button data-s=center data-t=forward>Fwd</button><span class=sp></span>"
"<button data-s=left data-t=neutral>Left</button>"
"<button data-s=center data-t=neutral>Stop</button>"
"<button data-s=right data-t=neutral>Right</button>"
"<span class=sp></span><button data-s=center data-t=reverse>Rev</button><span class=sp></span>"
"</div><div id=s>connecting...</div>"
"<script>"
"var host=location.hostname;"
"var img=document.getElementById('v');"
"function nextFrame(){img.src='http://'+host+':' + " STRINGIFY(STREAM_PORT) " + '/capture?t='+Date.now();}"
"img.onload=nextFrame;img.onerror=function(){setTimeout(nextFrame,500);};"
"nextFrame();"
"var ws,cur={steer:'center',throttle:'neutral'},timer;"
"function send(){if(ws&&ws.readyState==1)ws.send(JSON.stringify(cur));}"
"function connect(){ws=new WebSocket('ws://'+host+'/control');"
"ws.onopen=function(){document.getElementById('s').textContent='connected';send();};"
"ws.onclose=function(){document.getElementById('s').textContent='disconnected, retrying';setTimeout(connect,1000);};}"
"connect();"
"function hold(s,t){cur={steer:s,throttle:t};send();clearInterval(timer);timer=setInterval(send,100);}"
"function release(){cur={steer:'center',throttle:'neutral'};clearInterval(timer);send();}"
"document.querySelectorAll('#pad button').forEach(function(b){"
"var s=b.dataset.s,t=b.dataset.t;"
"b.addEventListener('pointerdown',function(e){e.preventDefault();hold(s,t);});"
"b.addEventListener('pointerup',release);"
"b.addEventListener('pointerleave',release);"
"b.addEventListener('pointercancel',release);});"
"</script></body></html>";

// --- Discrete enum -> protocol byte mapping -------------------------------
// The webapp speaks the discrete enums; the wire speaks the verified capture
// bytes from protocol.h. Unknown/missing strings fall back to the neutral
// value so a malformed frame can never command motion.
static uint8_t steer_from_str(const char *s) {
    if (s == NULL) return STEER_CENTER;
    if (strcmp(s, "left") == 0) return STEER_LEFT;
    if (strcmp(s, "right") == 0) return STEER_RIGHT;
    return STEER_CENTER;
}

static uint8_t thr_from_str(const char *s) {
    if (s == NULL) return THR_NEUTRAL;
    if (strcmp(s, "forward") == 0) return THR_FORWARD;
    if (strcmp(s, "reverse") == 0) return THR_REVERSE;
    return THR_NEUTRAL;
}

// --- Handlers -------------------------------------------------------------

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// WebSocket control endpoint. Receives text frames of the form
//   {"steer":"left|center|right","throttle":"forward|neutral|reverse"}
// and feeds command_set(). Malformed frames are ignored (logged at debug), so
// they never move the car; the handshake request itself returns immediately.
static esp_err_t control_ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // WebSocket handshake -- esp_http_server completes it internally.
        ESP_LOGI(TAG, "control WS connected");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { 0 };
    frame.type = HTTPD_WS_TYPE_TEXT;
    // First call with len=0 to learn the payload length.
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (frame.len == 0 || frame.len > 256) {
        // Empty control frame or an implausibly large one -- ignore.
        return ESP_OK;
    }

    uint8_t buf[257];
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        return ret;
    }
    buf[frame.len] = '\0';

    cJSON *root = cJSON_Parse((const char *)buf);
    if (root == NULL) {
        ESP_LOGD(TAG, "ignoring malformed WS frame");
        return ESP_OK;
    }
    const cJSON *steer = cJSON_GetObjectItemCaseSensitive(root, "steer");
    const cJSON *throttle = cJSON_GetObjectItemCaseSensitive(root, "throttle");
    uint8_t s = steer_from_str(cJSON_IsString(steer) ? steer->valuestring : NULL);
    uint8_t t = thr_from_str(cJSON_IsString(throttle) ? throttle->valuestring : NULL);
    command_set(s, t);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t capture_handler(httpd_req_t *req) {
    camera_fb_t *fb = camera_capture();
    if (fb == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not available");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    camera_return(fb);
    return ret;
}

static esp_err_t status_handler(httpd_req_t *req) {
    uint8_t steer, thr;
    command_get(&steer, &thr);

    const char *steer_str = (steer == STEER_LEFT) ? "left"
                          : (steer == STEER_RIGHT) ? "right" : "center";
    const char *thr_str = (thr == THR_FORWARD) ? "forward"
                        : (thr == THR_REVERSE) ? "reverse" : "neutral";

    char ip_str[16] = "0.0.0.0";
    esp_ip4_addr_t ip;
    if (wifi_sta_get_ip(&ip)) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));
    }

    uint32_t age = command_age_ms();
    bool ws_fresh = age < CONTROL_FAILSAFE_MS;
    int64_t uptime_ms = esp_timer_get_time() / 1000;

    char body[256];
    int n = snprintf(body, sizeof(body),
        "{\"ip\":\"%s\",\"steer\":\"%s\",\"throttle\":\"%s\","
        "\"ws_active\":%s,\"camera\":%s,\"uptime_ms\":%lld}",
        ip_str, steer_str, thr_str,
        ws_fresh ? "true" : "false",
        camera_ready() ? "true" : "false",
        (long long)uptime_ms);
    (void)n;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

void web_server_start(void) {
    // --- Control/API server on HTTP_PORT ---
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.ctrl_port = 32080; // distinct from the camera server's ctrl socket
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_http, &cfg) == ESP_OK) {
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
        httpd_uri_t control_uri = {
            .uri = "/control", .method = HTTP_GET, .handler = control_ws_handler,
            .is_websocket = true,
        };
        httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_handler };
        httpd_register_uri_handler(s_http, &index_uri);
        httpd_register_uri_handler(s_http, &control_uri);
        httpd_register_uri_handler(s_http, &status_uri);
        ESP_LOGI(TAG, "control/API server on port %d", HTTP_PORT);
    } else {
        ESP_LOGE(TAG, "failed to start control server");
    }

    // --- Camera snapshot server on STREAM_PORT ---
    // A ~25 KB blocking JPEG send must never occupy the control server's
    // single worker while a WS control frame waits, so /capture lives here.
    httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
    scfg.server_port = STREAM_PORT;
    scfg.ctrl_port = 32081;
    scfg.lru_purge_enable = true;

    if (httpd_start(&s_camera, &scfg) == ESP_OK) {
        httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler };
        httpd_register_uri_handler(s_camera, &capture_uri);
        ESP_LOGI(TAG, "camera server on port %d", STREAM_PORT);
    } else {
        ESP_LOGE(TAG, "failed to start camera server");
    }
}
