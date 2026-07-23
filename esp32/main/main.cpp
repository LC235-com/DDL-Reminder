#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lvgl_port.h"
#include "bsp_board.h"
#include "wifi_manager.h"
#include "websocket_client.h"
#include "audio_manager.h"
#include "protocol.h"
#include "led_controller.h"
#include "encoder.h"
#include "ui/ui_manager.h"
static const char* TAG = "DDL";

#define WIFI_SSID  "LC235的场域"
#define WIFI_PASS  "Lily1314"
#define WS_URI     "ws://192.168.49.4:8888"
#define LCD_H_RES  240
#define LCD_V_RES  240
static bool has_display = false;

enum State { IDLE, RECORDING, PROCESSING, SPEAKING, REMINDER };
static State st = IDLE;
static WiFiManager* wifi = nullptr;
static WebSocketClient* ws = nullptr;
static AudioManager* audio = nullptr;
static LEDController* leds = nullptr;
static bool rec = false;
static uint32_t rec_start = 0;
static bool tts = false;
static std::vector<DDLEvent> events;
#define MAX_REC_MS 10000
#define SR 16000

static void ws_cb(const WebSocketClient::EventData& e);
static void proc_msg(const std::string& m);
static void rec_start_fn(), rec_stop_fn();

static void ws_cb(const WebSocketClient::EventData& e) {
    switch(e.type){
        case WebSocketClient::EventType::CONNECTED:
            ESP_LOGI(TAG,"WS connected");
            if (has_display) UIManager::instance().set_connected(true);
            ws->sendText(ProtocolBuilder::build_hello());
            ws->sendText(ProtocolBuilder::build_request_sync()); break;
        case WebSocketClient::EventType::DISCONNECTED:
            ESP_LOGI(TAG,"WS disconnected"); if(has_display)UIManager::instance().set_connected(false);
            if(rec) { rec_stop_fn(); }
            st=IDLE; ws->connect(); break;
        case WebSocketClient::EventType::DATA_TEXT:
            proc_msg(std::string((const char*)e.data,e.data_len)); break;
        case WebSocketClient::EventType::DATA_BINARY:
            if(audio&&e.data_len>0){if(!tts){audio->startStreamingPlayback();tts=true;} audio->addStreamingAudioChunk(e.data,e.data_len);} break;
        case WebSocketClient::EventType::PING:
            if(tts&&audio){audio->finishStreamingPlayback();tts=false;if(st==SPEAKING){st=IDLE;leds->set_pattern(LEDController::IDLE);}} break;
        default: break;
    }
}

static void proc_msg(const std::string& m) {
    auto t=ProtocolParser::get_message_type(m);
    if(t==ProtocolParser::MessageType::SYNC){auto d=ProtocolParser::parse_sync(m);events=d.events;if(has_display)UIManager::instance().update_ddl_list(events);}
    else if(t==ProtocolParser::MessageType::NEW_EVENT){auto e=ProtocolParser::parse_new_event(m);events.push_back(e);if(has_display)UIManager::instance().update_ddl_list(events);}
    else if(t==ProtocolParser::MessageType::DELETE_EVENT){std::string id=ProtocolParser::parse_delete_event(m);events.erase(std::remove_if(events.begin(),events.end(),[&](auto&x){return x.id==id;}),events.end());if(has_display)UIManager::instance().update_ddl_list(events);}
    else if(t==ProtocolParser::MessageType::REMIND){auto d=ProtocolParser::parse_remind(m);if(has_display)UIManager::instance().show_reminder(d.event);leds->set_pattern(LEDController::REMINDER);st=REMINDER;}
    else if(t==ProtocolParser::MessageType::SPEAK){auto d=ProtocolParser::parse_speak(m);st=SPEAKING;leds->set_pattern(LEDController::SPEAKING);if(has_display)UIManager::instance().set_emotion(d.emotion);}
    else if(t==ProtocolParser::MessageType::EMOTION){if(has_display)UIManager::instance().set_emotion(ProtocolParser::parse_emotion(m));}
    else if(t==ProtocolParser::MessageType::LED){std::string a,c;ProtocolParser::parse_led(m,a,c);if(a=="flash")leds->set_pattern(LEDController::REMINDER);else if(a=="off")leds->set_pattern(LEDController::OFF);}
}

static void rec_start_fn() {
    if(rec) return;
    rec=true; rec_start=xTaskGetTickCount()*portTICK_PERIOD_MS;
    leds->set_pattern(LEDController::RECORDING); if(has_display)UIManager::instance().set_emotion("neutral");
    audio->startRecording();
    if(ws&&ws->isConnected()) ws->sendText(ProtocolBuilder::build_audio_start());
}
static void rec_stop_fn() {
    if(!rec) return;
    rec=false; leds->set_pattern(LEDController::PROCESSING); if(has_display)UIManager::instance().set_emotion("thinking");
    audio->stopRecording();
    size_t len=0; const int16_t* d=audio->getRecordingBuffer(len);
    if(ws&&ws->isConnected()&&len>SR/4){
        auto*raw=(const uint8_t*)d; size_t bytes=len*2;
        for(size_t off=0;off<bytes;off+=4096) ws->sendBinary(raw+off,std::min((size_t)4096,bytes-off));
        ws->sendText(ProtocolBuilder::build_audio_end()); st=PROCESSING;
    }else{audio->clearRecordingBuffer();st=IDLE;leds->set_pattern(LEDController::IDLE);}
}

static esp_lcd_panel_handle_t panel_handle = nullptr;

static esp_err_t init_lcd() {
    // Pins (from schematic)
    const gpio_num_t BL  = GPIO_NUM_11;
    const gpio_num_t RST = GPIO_NUM_10;
    const gpio_num_t DC  = GPIO_NUM_12;
    const gpio_num_t CS  = GPIO_NUM_9;
    const gpio_num_t MOSI= GPIO_NUM_3;
    const gpio_num_t CLK = GPIO_NUM_46;

    // 1. Backlight PWM
    ledc_timer_config_t lt = {};
    lt.speed_mode = LEDC_LOW_SPEED_MODE;
    lt.duty_resolution = LEDC_TIMER_10_BIT;
    lt.timer_num = LEDC_TIMER_0;
    lt.freq_hz = 5000;
    lt.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&lt);

    ledc_channel_config_t lc = {};
    lc.gpio_num = BL;
    lc.speed_mode = LEDC_LOW_SPEED_MODE;
    lc.channel = LEDC_CHANNEL_0;
    lc.timer_sel = LEDC_TIMER_0;
    lc.duty = 1023;  // full brightness for testing
    ledc_channel_config(&lc);

    // 2. SPI bus
    spi_bus_config_t sb = {};
    sb.mosi_io_num = MOSI;
    sb.miso_io_num = -1;
    sb.sclk_io_num = CLK;
    sb.quadwp_io_num = -1;
    sb.quadhd_io_num = -1;
    sb.max_transfer_sz = LCD_H_RES * LCD_V_RES * 2;
    spi_bus_initialize(SPI2_HOST, &sb, SPI_DMA_CH_AUTO);

    // 3. Panel IO (SPI)
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = CS;
    io.dc_gpio_num = DC;
    io.spi_mode = 3;  // SPI mode 3 for ST7789
    io.pclk_hz = 40000000;  // 40MHz (stable for most ST7789 panels)
    io.trans_queue_depth = 10;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    esp_lcd_panel_io_handle_t io_h = nullptr;
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &io_h);

    // 4. ST7789 240x240 panel (1.54" IPS TFT)
    esp_lcd_panel_dev_config_t pd = {};
    pd.reset_gpio_num = RST;
    pd.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;  // 1.54" IPS ST7789 uses RGB order
    pd.bits_per_pixel = 16;
    esp_lcd_new_panel_st7789(io_h, &pd, &panel_handle);

    // 5. ST7789 init (240x240 typically needs invert + swap_xy depending on panel)
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, true);
    esp_lcd_panel_swap_xy(panel_handle, false);
    esp_lcd_panel_mirror(panel_handle, false, false);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // 6. LVGL port
    lvgl_port_cfg_t lvcfg = {};
    lvcfg.task_priority = 4;
    lvcfg.task_stack = 6144;
    lvcfg.task_max_sleep_ms = 10;
    lvcfg.timer_period_ms = 5;
    lvgl_port_init(&lvcfg);

    lvgl_port_display_cfg_t dc = {};
    dc.io_handle = io_h;
    dc.panel_handle = panel_handle;
    dc.buffer_size = LCD_H_RES * 30;  // 30 lines buffer for 240px width
    dc.double_buffer = false;
    dc.hres = LCD_H_RES;
    dc.vres = LCD_V_RES;
    dc.monochrome = false;
    dc.flags.buff_dma = true;
    dc.flags.buff_spiram = false;  // DMA can't use SPIRAM, must use internal RAM
    lv_disp_t* disp = lvgl_port_add_disp(&dc);
    if (!disp) {
        ESP_LOGE(TAG, "LVGL display registration failed");
        return ESP_FAIL;
    }

    // 7. Touch I2C
    i2c_config_t i2c = {};
    i2c.mode = I2C_MODE_MASTER;
    i2c.sda_io_num = GPIO_NUM_21;
    i2c.scl_io_num = GPIO_NUM_45;
    i2c.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c.master.clk_speed = 400000;
    i2c_param_config(I2C_NUM_0, &i2c);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

    has_display = true;
    ESP_LOGI(TAG,"LCD+Touch ready");
    return ESP_OK;
}

void init_ui_cb() {
    g_on_mic_press = [](){ if(st==IDLE) rec_start_fn(); };
    g_on_mic_release = [](){ if(rec) rec_stop_fn(); };
    g_on_event_action = [](const std::string& a){ if(ws&&ws->isConnected()) ws->sendText(ProtocolBuilder::build_event_action(UIManager::instance().current_detail_id(),a)); };
    g_on_request_sync = [](){ if(ws&&ws->isConnected()) ws->sendText(ProtocolBuilder::build_request_sync()); };
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG,"=== DDL Reminder ESP32-S3 ===");
    esp_err_t r=nvs_flash_init();
    if(r==ESP_ERR_NVS_NO_FREE_PAGES||r==ESP_ERR_NVS_NEW_VERSION_FOUND){nvs_flash_erase();nvs_flash_init();}

    init_lcd();
    leds=new LEDController(GPIO_NUM_41,4); leds->init(); leds->set_pattern(LEDController::IDLE);
    auto* enc = new EncoderManager(GPIO_NUM_38,GPIO_NUM_39,GPIO_NUM_40);
    enc->init();

    bsp_board_init(16000,1,16); bsp_audio_init(16000,1,16);
    audio=new AudioManager(16000,10,32); audio->init();

    wifi=new WiFiManager(WIFI_SSID,WIFI_PASS); wifi->connect();
    // NTP time sync (after WiFi is up)
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    setenv("TZ", "CST-8", 1); tzset();
    ESP_LOGI(TAG,"NTP sync started");

    ws=new WebSocketClient(WS_URI,false,5000); ws->setEventCallback(ws_cb); ws->connect();

    if (has_display) { UIManager::instance().init(); init_ui_cb(); }
    ESP_LOGI(TAG,"Ready. Display=%s", has_display?"yes":"no");

    // Audio capture buffer (320 samples = 20ms at 16kHz)
    const int chunk_samples = 320;
    int16_t* audio_buf = (int16_t*)malloc(chunk_samples * sizeof(int16_t));

    uint32_t lc=0,lp=0;
    bool enc_was_pressed = false;
    int32_t last_enc_count = 0;
    while(1){
        uint32_t n=xTaskGetTickCount()*portTICK_PERIOD_MS;
        if (has_display) { lv_timer_handler(); lv_task_handler(); }
        if(leds) leds->update();

        // Audio capture: read from I2S mic while recording (buffer locally only)
        if (rec && audio_buf && !audio->isRecordingBufferFull()) {
            esp_err_t r = bsp_get_feed_data(false, audio_buf, chunk_samples * sizeof(int16_t));
            if (r == ESP_OK) {
                audio->addRecordingData(audio_buf, chunk_samples);
            }
        }

        // EC11 encoder: rotation → UI list navigation
        if (enc && has_display) {
            int32_t count = enc->encoder_count_.load();
            if (count != last_enc_count) {
                int32_t delta = count - last_enc_count;
                last_enc_count = count;
                auto scr = UIManager::instance().current_screen();
                if (scr == UIManager::Screen::LIST) {
                    // Scroll the event list
                    if (!events.empty()) {
                        lv_obj_t* list = UIManager::instance().get_event_list();
                        if (list) {
                            int total = lv_obj_get_child_cnt(list);
                            if (total > 0) {
                                int idx = (delta > 0) ? -1 : 1; // scroll direction
                                // Use lv_obj_scroll_by for smooth scrolling
                                lv_obj_scroll_by(list, 0, idx * 40, LV_ANIM_OFF);
                            }
                        }
                    }
                } else if (scr == UIManager::Screen::DETAIL && !events.empty()) {
                    // Navigate between events with encoder rotation
                    static int detail_idx = 0;
                    if (delta > 0) detail_idx = (detail_idx + 1) % events.size();
                    else detail_idx = (detail_idx - 1 + events.size()) % events.size();
                    UIManager::instance().show_detail(events[detail_idx]);
                }
            }

            // EC11 button polling
            bool now_pressed = enc->button_pressed_.load();
            if (enc_was_pressed && !now_pressed) {
                uint32_t dur = n - enc->button_press_time_.load();
                if (dur < 2000) {
                    if (rec) rec_stop_fn(); else rec_start_fn();
                }
            }
            enc_was_pressed = now_pressed;
        }

        if (has_display && n-lc>1000){UIManager::instance().update_clock();lc=n;}
        if(ws&&ws->isConnected()&&n-lp>30000){ws->sendText(ProtocolBuilder::build_ping());lp=n;}
        if(rec&&(n-rec_start>MAX_REC_MS)) rec_stop_fn();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
