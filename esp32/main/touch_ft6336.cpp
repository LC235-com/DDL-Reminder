/**
 * @file touch_ft6336.cpp
 * @brief FT6336U I2C touch driver — register map same as FT6336/CST816:
 *   0xA3: ChipID (0x36 for FT6336U)
 *   0x00: Device mode (0=normal)
 *   0x02-0x07: Touch data (6 bytes)
 *
 * I2C address: 0x38
 */

#include "touch_ft6336.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char* TAG = "FT6336";
static uint8_t i2c_addr = 0x38;  // configurable

esp_err_t TouchFT6336::init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl,
                             gpio_num_t rst, gpio_num_t irq,
                             uint16_t w, uint16_t h, uint8_t addr) {
    i2c_addr = addr;
    port_ = port; w_ = w; h_ = h;
    rst_pin_ = rst; irq_pin_ = irq;

    // Hardware reset
    if (rst != GPIO_NUM_NC) {
        gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        gpio_set_level(rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(rst, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Verify device responds (skip strict ChipID check — some clones use different IDs)
    uint8_t chip_id = 0;
    read_regs(0xA3, &chip_id, 1);  // try standard ChipID register
    if (chip_id == 0 || chip_id == 0xFF) {
        read_regs(0xA8, &chip_id, 1);  // alternate ChipID
    }
    ESP_LOGI(TAG, "FT6336U init at 0x%02X (chip_id=0x%02X, %dx%d) %s",
             i2c_addr, chip_id, w_, h_,
             (chip_id != 0 && chip_id != 0xFF) ? "OK" : "proceeding anyway");

    // Set to normal operating mode
    write_reg(0x00, 0x00);

    inited_ = true;
    return ESP_OK;
}

TouchFT6336::Point TouchFT6336::read() {
    Point p;
    if (!inited_) return p;

    uint8_t buf[6] = {};
    if (read_regs(0x02, buf, 6) != ESP_OK) return p;

    uint8_t touch_count = buf[0] & 0x0F;
    if (touch_count == 0) { p.pressed = false; return p; }

    // Parse point 0 (12-bit X and Y)
    uint16_t x = ((buf[1] & 0x0F) << 8) | buf[2];
    uint16_t y = ((buf[3] & 0x0F) << 8) | buf[4];

    // Scale to display resolution if needed (raw values may differ)
    if (x >= w_) x = w_ - 1;
    if (y >= h_) y = h_ - 1;

    p.x = x; p.y = y; p.pressed = true;
    return p;
}

esp_err_t TouchFT6336::write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(port_, i2c_addr, buf, 2,
                                      50 / portTICK_PERIOD_MS);
}

esp_err_t TouchFT6336::read_regs(uint8_t reg, uint8_t* buf, size_t len) {
    return i2c_master_write_read_device(port_, i2c_addr, &reg, 1, buf, len,
                                        50 / portTICK_PERIOD_MS);
}

void TouchFT6336::reset() {
    if (rst_pin_ != GPIO_NUM_NC) {
        gpio_set_level(rst_pin_, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(rst_pin_, 1);
    }
}
