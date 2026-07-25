/**
 * @file touch_ft6336.h
 * @brief FT6336U capacitive touch controller driver (I2C addr 0x38).
 *
 * Register map (6 bytes from 0x02):
 *   [0]: gesture + touch count (low 4 bits = points)
 *   [1]: X[11:8] + event flag
 *   [2]: X[7:0]
 *   [3]: Y[11:8] + touch ID
 *   [4]: Y[7:0]
 *   [5]: pressure (optional)
 *
 * Adapted from xiaozhi-esp32 Ft6336 driver.
 */
#pragma once

#include "driver/i2c.h"
#include "esp_err.h"
#include <cstdint>

class TouchFT6336 {
public:
    struct Point { uint16_t x=0, y=0; bool pressed=false; };

    esp_err_t init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl,
                   gpio_num_t rst = GPIO_NUM_NC, gpio_num_t irq = GPIO_NUM_NC,
                   uint16_t w=240, uint16_t h=320, uint8_t addr=0x38);
    Point read();
    void reset();

private:
    i2c_port_t port_ = I2C_NUM_0;
    uint16_t w_=240, h_=320;
    gpio_num_t rst_pin_ = GPIO_NUM_NC, irq_pin_ = GPIO_NUM_NC;
    bool inited_ = false;

    esp_err_t write_reg(uint8_t reg, uint8_t val);
    esp_err_t read_regs(uint8_t reg, uint8_t* buf, size_t len);
};
