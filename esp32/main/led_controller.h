#pragma once
#include "driver/gpio.h"
#include <led_strip.h>
#include <stdint.h>

class LEDController {
public:
    enum Pattern { IDLE, RECORDING, PROCESSING, SPEAKING, REMINDER, DONE, OFF };
    LEDController(gpio_num_t gpio = GPIO_NUM_41, int n = 4);
    ~LEDController();
    esp_err_t init();
    void set_pattern(Pattern p);
    void flash(uint8_t r, uint8_t g, uint8_t b, int ms = 500);
    void clear();
    void set_solid(uint8_t r, uint8_t g, uint8_t b, uint8_t br = 255);
    void update();
private:
    gpio_num_t gpio_; int num_; led_strip_handle_t handle_;
    Pattern pat_; uint32_t last_; uint8_t step_;
    void px(int i, uint8_t r, uint8_t g, uint8_t b);
    void rf();
};
