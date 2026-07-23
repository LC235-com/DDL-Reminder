#include "led_controller.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char* T = "LED";
LEDController::LEDController(gpio_num_t gpio, int n) : gpio_(gpio), num_(n), handle_(nullptr), pat_(IDLE), last_(0), step_(0) {}
LEDController::~LEDController() { if (handle_) led_strip_del(handle_); }
esp_err_t LEDController::init() {
    led_strip_config_t sc = {};
    sc.strip_gpio_num = (int)gpio_;
    sc.max_leds = (uint32_t)num_;
    sc.led_model = LED_MODEL_SK6812;
    sc.led_pixel_format = LED_PIXEL_FORMAT_GRB;
    sc.flags.invert_out = false;
    led_strip_rmt_config_t rc = {};
    rc.clk_src = RMT_CLK_SRC_DEFAULT;
    rc.resolution_hz = 10*1000*1000;
    rc.mem_block_symbols = 64;
    rc.flags.with_dma = false;
    esp_err_t r = led_strip_new_rmt_device(&sc, &rc, &handle_);
    if (r) { ESP_LOGE(T, "init fail: %s", esp_err_to_name(r)); return r; }
    clear(); ESP_LOGI(T, "ready: %d LEDs GPIO%d", num_, (int)gpio_); return ESP_OK;
}
void LEDController::px(int i, uint8_t r, uint8_t g, uint8_t b) { if (handle_ && i < num_) led_strip_set_pixel(handle_, i, r, g, b); }
void LEDController::rf() { if (handle_) led_strip_refresh(handle_); }
void LEDController::clear() { for (int i=0;i<num_;i++) px(i,0,0,0); rf(); }
void LEDController::set_solid(uint8_t r, uint8_t g, uint8_t b, uint8_t br) { r=r*br/255; g=g*br/255; b=b*br/255; for(int i=0;i<num_;i++) px(i,r,g,b); rf(); }
void LEDController::flash(uint8_t r, uint8_t g, uint8_t b, int ms) { set_solid(r,g,b); vTaskDelay(pdMS_TO_TICKS(ms)); clear(); }
void LEDController::set_pattern(Pattern p) { pat_=p; step_=0; if(p==OFF) clear(); }
void LEDController::update() {
    uint32_t n = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (n-last_<50) return;
    last_=n;
    switch(pat_){
        case IDLE: { uint8_t b=20+(step_%60<30?(step_%30)*4:(30-step_%30)*4); set_solid(0,50,150,b); break; }
        case RECORDING: set_solid(0,200,200); break;
        case PROCESSING: { uint8_t b=100+(step_%20<10?(step_%10)*15:(10-step_%10)*15); set_solid(255,200,0,b); break; }
        case SPEAKING: for(int i=0;i<num_;i++) px(i,0,(i==step_%num_)?255:30,0); rf(); break;
        case REMINDER: if((step_/10)%2) set_solid(255,0,0); else clear(); break;
        default: break;
    } step_++;
}
