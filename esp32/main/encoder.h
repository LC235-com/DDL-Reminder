/**
 * @file encoder.h
 * @brief EC11 rotary encoder driver — GPIO interrupt-based quadrature decoding
 */

#ifndef ENCODER_H
#define ENCODER_H

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include <atomic>
#include <functional>

class EncoderManager {
public:
    using RotateCallback = std::function<void(bool clockwise)>;
    using ButtonCallback = std::function<void(bool long_press)>;

    EncoderManager(gpio_num_t clk_pin = GPIO_NUM_38,
                   gpio_num_t dt_pin = GPIO_NUM_39,
                   gpio_num_t sw_pin = GPIO_NUM_40);

    ~EncoderManager();

    esp_err_t init();

    void on_rotate(RotateCallback cb) { rotate_cb_ = cb; }
    void on_button(ButtonCallback cb) { button_cb_ = cb; }
    bool is_pressed() const;

    // Public for ISR access
    std::atomic<int32_t> encoder_count_{0};
    std::atomic<bool> button_pressed_{false};
    std::atomic<uint32_t> button_press_time_{0};

private:
    gpio_num_t clk_pin_, dt_pin_, sw_pin_;
    RotateCallback rotate_cb_;
    ButtonCallback button_cb_;

    static void IRAM_ATTR clk_isr_handler(void* arg);
    static void IRAM_ATTR dt_isr_handler(void* arg);
    static void IRAM_ATTR sw_isr_handler(void* arg);
};

#endif
