/**
 * @file encoder.cpp
 * @brief EC11 rotary encoder — ISR-based quadrature reading
 */

#include "encoder.h"
#include "esp_log.h"

static const char* TAG = "Encoder";

EncoderManager::EncoderManager(gpio_num_t clk_pin, gpio_num_t dt_pin, gpio_num_t sw_pin)
    : clk_pin_(clk_pin), dt_pin_(dt_pin), sw_pin_(sw_pin) {}

EncoderManager::~EncoderManager() {
    gpio_isr_handler_remove(clk_pin_);
    gpio_isr_handler_remove(dt_pin_);
    gpio_isr_handler_remove(sw_pin_);
}

void IRAM_ATTR EncoderManager::clk_isr_handler(void* arg) {
    auto* self = static_cast<EncoderManager*>(arg);
    self->encoder_count_.store(
        self->encoder_count_.load() + (gpio_get_level(self->dt_pin_) == 0 ? 1 : -1));
}

void IRAM_ATTR EncoderManager::dt_isr_handler(void* arg) {
    auto* self = static_cast<EncoderManager*>(arg);
    self->encoder_count_.store(
        self->encoder_count_.load() + (gpio_get_level(self->clk_pin_) == 0 ? -1 : 1));
}

void IRAM_ATTR EncoderManager::sw_isr_handler(void* arg) {
    auto* self = static_cast<EncoderManager*>(arg);
    int level = gpio_get_level(self->sw_pin_);
    self->button_pressed_.store(level == 0);
    if (level == 0) self->button_press_time_.store(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

esp_err_t EncoderManager::init() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << clk_pin_) | (1ULL << dt_pin_),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << sw_pin_);
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(clk_pin_, clk_isr_handler, this);
    gpio_isr_handler_add(dt_pin_, dt_isr_handler, this);
    gpio_isr_handler_add(sw_pin_, sw_isr_handler, this);

    ESP_LOGI(TAG, "Encoder: CLK=%d DT=%d SW=%d", (int)clk_pin_, (int)dt_pin_, (int)sw_pin_);
    return ESP_OK;
}

bool EncoderManager::is_pressed() const {
    return gpio_get_level(sw_pin_) == 0;
}
