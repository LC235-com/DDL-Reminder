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

void IRAM_ATTR EncoderManager::ab_isr_handler(void* arg) {
    auto* self = static_cast<EncoderManager*>(arg);
    // Gray-code transition table. Invalid/bouncing transitions add zero and
    // valid reverse transitions naturally cancel, which is much more reliable
    // than counting CLK and DT independently.
    static constexpr int8_t transition[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0,
    };
    uint8_t current = (gpio_get_level(self->clk_pin_) << 1) |
                      gpio_get_level(self->dt_pin_);
    uint8_t previous = self->last_ab_state_.exchange(current);
    self->encoder_count_.fetch_add(transition[(previous << 2) | current]);
}

void IRAM_ATTR EncoderManager::sw_isr_handler(void* arg) {
    auto* self = static_cast<EncoderManager*>(arg);
    int level = gpio_get_level(self->sw_pin_);
    self->button_pressed_.store(level == 0);
}

esp_err_t EncoderManager::init() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << clk_pin_) | (1ULL << dt_pin_),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;

    io_conf.pin_bit_mask = (1ULL << sw_pin_);
    err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;

    last_ab_state_.store((gpio_get_level(clk_pin_) << 1) | gpio_get_level(dt_pin_));
    button_pressed_.store(gpio_get_level(sw_pin_) == 0);

    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if ((err = gpio_isr_handler_add(clk_pin_, ab_isr_handler, this)) != ESP_OK) return err;
    if ((err = gpio_isr_handler_add(dt_pin_, ab_isr_handler, this)) != ESP_OK) return err;
    if ((err = gpio_isr_handler_add(sw_pin_, sw_isr_handler, this)) != ESP_OK) return err;

    ESP_LOGI(TAG, "Encoder: CLK=%d DT=%d SW=%d", (int)clk_pin_, (int)dt_pin_, (int)sw_pin_);
    return ESP_OK;
}

bool EncoderManager::is_pressed() const {
    return gpio_get_level(sw_pin_) == 0;
}
