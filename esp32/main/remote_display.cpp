/**
 * @file remote_display.cpp
 * @brief UDP remote display — sends LVGL framebuffer to PC client, receives touch.
 *
 * Adapted from CubeCoders/LVGLRemoteServer for ESP-IDF (lwip sockets + LVGL 8.3).
 */

#include "remote_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>

static const char* TAG = "RemoteDisp";

// Static members
RemoteDisplay* RemoteDisplay::instance_ = nullptr;
lv_coord_t RemoteDisplay::touch_x_ = 0;
lv_coord_t RemoteDisplay::touch_y_ = 0;
lv_indev_state_t RemoteDisplay::touch_state_ = LV_INDEV_STATE_REL;

RemoteDisplay::RemoteDisplay() = default;

RemoteDisplay::~RemoteDisplay() {
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}

esp_err_t RemoteDisplay::init(lv_disp_t* disp) {
    disp_ = disp;
    if (!disp_) {
        ESP_LOGE(TAG, "No display handle provided");
        return ESP_ERR_INVALID_ARG;
    }

    instance_ = this;

    // Intercept LVGL flush callback to mirror pixels to remote client
    lv_disp_drv_t* drv = disp->driver;
    original_flush_cb_ = drv->flush_cb;
    drv->flush_cb = flush_cb;

    // Register LVGL touch input device (LVGL 8.3 API)
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    indev_ = lv_indev_drv_register(&indev_drv);

    ESP_LOGI(TAG, "Remote display ready (UDP deferred). Port %d", REMOTE_DISP_PORT);
    return ESP_OK;
}

void RemoteDisplay::flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map) {
    RemoteDisplay* rd = instance_;
    if (!rd) {
        // Fallback: no remote display, chain to original
        return;
    }

    // Tile and send pixels to remote client
    uint16_t fw = area->x2 - area->x1 + 1;
    uint16_t fh = area->y2 - area->y1 + 1;
    const uint8_t* px = (const uint8_t*)color_map;

    for (uint16_t row = 0; row < fh; row += REMOTE_TILE_H) {
        uint16_t th = (row + REMOTE_TILE_H <= fh) ? REMOTE_TILE_H : (fh - row);
        for (uint16_t col = 0; col < fw; col += REMOTE_TILE_W) {
            uint16_t tw = (col + REMOTE_TILE_W <= fw) ? REMOTE_TILE_W : (fw - col);
            rd->send_tile(area->x1 + col, area->y1 + row, tw, th,
                          px + (row * fw + col) * 2, fw);
        }
    }

    // Chain to original (physical LCD) flush
    if (rd->original_flush_cb_) {
        rd->original_flush_cb_(drv, area, color_map);
    }
}

/**
 * @brief Lazily create UDP socket (called on first update after WiFi is ready).
 */
static bool ensure_socket(int& sock) {
    if (sock >= 0) return true;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(REMOTE_DISP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        sock = -1;
        return false;
    }

    // Set non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(TAG, "Remote display UDP ready on port %d", REMOTE_DISP_PORT);
    return true;
}

void RemoteDisplay::update() {
    // Lazy socket creation (deferred until WiFi/LWIP is up)
    if (!ensure_socket(sock_)) return;

    poll_touch();

    // Periodic full refresh to keep remote client in sync
    if (remote_addr_ != 0) {
        uint32_t now = esp_timer_get_time() / 1000;
        if (now - last_full_refresh_ > 2000) {
            last_full_refresh_ = now;
            lv_obj_invalidate(lv_scr_act());  // trigger full redraw via flush callback
        }
    }
}

void RemoteDisplay::full_refresh() {
    // Trigger info packet + full screen redraw
    send_info_packet();
    lv_obj_invalidate(lv_scr_act());
}

void RemoteDisplay::send_info_packet() {
    if (remote_addr_ == 0 || sock_ < 0 || !ensure_socket(sock_)) return;

    uint16_t w = (uint16_t)LV_HOR_RES;
    uint16_t h = (uint16_t)LV_VER_RES;
    uint16_t control = 0xFFFF;
    uint16_t zero = 0;

    size_t off = 0;
    memcpy(tx_buf_ + off, &control, 2); off += 2;
    memcpy(tx_buf_ + off, &w, 2); off += 2;
    memcpy(tx_buf_ + off, &h, 2); off += 2;
    memcpy(tx_buf_ + off, &zero, 2); off += 2;
    memcpy(tx_buf_ + off, &zero, 2); off += 2;

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(remote_port_);
    dest.sin_addr.s_addr = remote_addr_;
    sendto(sock_, tx_buf_, off, 0, (struct sockaddr*)&dest, sizeof(dest));
}

void RemoteDisplay::send_tile(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               const uint8_t* pixels, uint16_t stride) {
    if (remote_addr_ == 0 || sock_ < 0 || !ensure_socket(sock_)) return;
    if (w == 0 || h == 0) return;

    uint16_t control = 0x0000;  // raw tile
    size_t hdr_sz = 10;  // control(2) + x(2) + y(2) + w(2) + h(2)
    size_t max_px = (REMOTE_MAX_PKT - hdr_sz) / 2;

    // Truncate if tile exceeds UDP packet
    uint16_t send_h = h;
    size_t px_count = (size_t)w * send_h;
    if (px_count > max_px) {
        send_h = (uint16_t)(max_px / w);
        if (send_h == 0) send_h = 1;
    }

    size_t off = 0;
    memcpy(tx_buf_ + off, &control, 2); off += 2;
    memcpy(tx_buf_ + off, &x, 2); off += 2;
    memcpy(tx_buf_ + off, &y, 2); off += 2;
    memcpy(tx_buf_ + off, &w, 2); off += 2;
    memcpy(tx_buf_ + off, &send_h, 2); off += 2;

    // Copy rows with stride (no inversion — LCD handles its own)
    for (uint16_t row = 0; row < send_h; row++) {
        memcpy(tx_buf_ + off, pixels + row * stride * 2, w * 2);
        off += w * 2;
    }

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(remote_port_);
    dest.sin_addr.s_addr = remote_addr_;
    sendto(sock_, tx_buf_, off, 0, (struct sockaddr*)&dest, sizeof(dest));

    // If truncated, recursively send remaining rows
    if (send_h < h) {
        send_tile(x, y + send_h, w, h - send_h, pixels + send_h * stride * 2, stride);
    }
}

void RemoteDisplay::poll_touch() {
    if (!ensure_socket(sock_)) return;

    uint8_t buf[5];
    struct sockaddr_in src = {};
    socklen_t srclen = sizeof(src);

    int n = recvfrom(sock_, buf, sizeof(buf), MSG_DONTWAIT,
                     (struct sockaddr*)&src, &srclen);
    if (n != 5) return;

    uint8_t status = buf[0];
    uint16_t rx = (buf[1] << 8) | buf[2];
    uint16_t ry = (buf[3] << 8) | buf[4];

    switch (status) {
    case 0:  // Release
        touch_state_ = LV_INDEV_STATE_REL;
        break;
    case 1:  // Touch
        touch_state_ = LV_INDEV_STATE_PR;
        touch_x_ = rx;
        touch_y_ = ry;
        break;
    case 2:  // Connect/refresh
        remote_addr_ = src.sin_addr.s_addr;
        remote_port_ = ntohs(src.sin_port);
        ESP_LOGI(TAG, "PC client connected, sending display...");
        send_info_packet();
        full_refresh();
        break;
    }
}

void RemoteDisplay::touch_read_cb(lv_indev_drv_t* indev_drv, lv_indev_data_t* data) {
    data->state = touch_state_;
    data->point.x = touch_x_;
    data->point.y = touch_y_;
    // Let the remote client control press/release — DO NOT auto-release
}
