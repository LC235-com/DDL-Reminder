/**
 * @file remote_display.h
 * @brief UDP-based remote LVGL display mirroring for PC development.
 *
 * Adapted from CubeCoders/LVGLRemoteServer for ESP-IDF + LVGL 8.3.
 *
 * Protocol: ESP32 sends RLE-encoded display tiles via UDP port 24680.
 *           PC client sends 5-byte touch packets back (status, X, Y).
 *
 * Usage:
 *   RemoteDisplay rd;
 *   rd.init(disp);   // disp = lv_disp_t* from lvgl_port_add_disp()
 *   // In main loop: rd.update();  // polls UDP for touch, sends display diffs
 */
#pragma once

#include "lvgl.h"
#include "esp_err.h"
#include <cstdint>
#include <string>

#define REMOTE_DISP_PORT    24680
#define REMOTE_MAX_PKT      1430
#define REMOTE_TILE_W       40
#define REMOTE_TILE_H       16

class RemoteDisplay {
public:
    RemoteDisplay();
    ~RemoteDisplay();

    /**
     * @brief Initialize UDP socket and LVGL input device.
     * @param disp  LVGL display handle (from lvgl_port_add_disp)
     */
    esp_err_t init(lv_disp_t* disp);

    /**
     * @brief Called every frame to send display updates and receive touch.
     * Should be called after lv_timer_handler() in the main loop.
     */
    void update();

    /**
     * @brief Check if remote display is active (client connected).
     */
    bool is_connected() const { return remote_addr_ != 0; }

private:
    int sock_ = -1;
    uint32_t remote_addr_ = 0;   // PC client IP (network byte order)
    uint16_t remote_port_ = 0;
    lv_disp_t* disp_ = nullptr;
    void* indev_ = nullptr;
    uint8_t tx_buf_[REMOTE_MAX_PKT];
    uint32_t last_full_refresh_ = 0;

    // Chained flush callback (LVGL 8.3 uses raw function pointer)
    void (*original_flush_cb_)(lv_disp_drv_t*, const lv_area_t*, lv_color_t*) = nullptr;

    // Touch state
    static lv_coord_t touch_x_, touch_y_;
    static lv_indev_state_t touch_state_;

    // Public for static flush callback
    static RemoteDisplay* instance_;

    void send_tile(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   const uint8_t* pixels, uint16_t stride = 0);
    void send_info_packet();
    void poll_touch();
    void full_refresh();

    static void touch_read_cb(lv_indev_drv_t* indev_drv, lv_indev_data_t* data);
    static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map);
};
