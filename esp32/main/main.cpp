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
#include "touch_ft6336.h"
#include "ui/ui_manager.h"
static const char* TAG = "DDL";

#define WIFI_SSID  "LC235的场域"
#define WIFI_PASS  "Lily1314"
#define WS_URI     "ws://192.168.49.4:8888"
#define LCD_H_RES  240
#define LCD_V_RES  320
// Remote display (UDP mirror to PC) — define to enable
#define DDL_REMOTE_DISPLAY 0  // disabled: UDP send stalls LVGL flush, triggers WDT
#ifdef DDL_REMOTE_DISPLAY
#include "remote_display.h"
static RemoteDisplay* remote_disp = nullptr;
#endif

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
std::vector<DDLEvent> events;
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

// UI update callbacks (called from LVGL task via lv_async_call)
// NOTE: update_ddl_list creates many LVGL objects and WDT if run in timer.
// Instead, we set a pending flag and process in the main loop.
static std::vector<DDLEvent>* pending_ddl_update = nullptr;
static void ui_update_ddl(void* p) {
    // Store pending update — will be processed in main loop outside lv_timer_handler
    if (pending_ddl_update) delete pending_ddl_update;
    pending_ddl_update = (std::vector<DDLEvent>*)p;
}
static void ui_show_reminder(void* p) {
    auto* e = (DDLEvent*)p;
    UIManager::instance().show_reminder(*e);
    delete e;
}
static void ui_update_emoji(void* p) {
    auto* s = (std::string*)p;
    UIManager::instance().set_emotion(*s);
    delete s;
}
static void ui_show_asr(void* p) {
    auto* pair = (std::pair<std::string,bool>*)p;
    UIManager::instance().show_asr_text(pair->first, pair->second);
    delete pair;
}

static void proc_msg(const std::string& m) {
    auto t=ProtocolParser::get_message_type(m);
    if(t==ProtocolParser::MessageType::SYNC){
        auto d=ProtocolParser::parse_sync(m); events=d.events;
        if(has_display) lv_async_call(ui_update_ddl, new std::vector<DDLEvent>(events));
    }
    else if(t==ProtocolParser::MessageType::NEW_EVENT){
        auto e=ProtocolParser::parse_new_event(m); events.push_back(e);
        if(has_display) lv_async_call(ui_update_ddl, new std::vector<DDLEvent>(events));
    }
    else if(t==ProtocolParser::MessageType::DELETE_EVENT){
        std::string id=ProtocolParser::parse_delete_event(m);
        events.erase(std::remove_if(events.begin(),events.end(),[&](auto&x){return x.id==id;}),events.end());
        if(has_display) lv_async_call(ui_update_ddl, new std::vector<DDLEvent>(events));
    }
    else if(t==ProtocolParser::MessageType::REMIND){
        auto d=ProtocolParser::parse_remind(m);
        if(has_display) lv_async_call(ui_show_reminder, new DDLEvent(d.event));
        leds->set_pattern(LEDController::REMINDER); st=REMINDER;
    }
    else if(t==ProtocolParser::MessageType::SPEAK){
        auto d=ProtocolParser::parse_speak(m); st=SPEAKING;
        leds->set_pattern(LEDController::SPEAKING);
        if(has_display){
            std::string prefix;
            if(d.emotion=="happy")prefix="^_^ "; else if(d.emotion=="thinking")prefix="o.O ";
            else if(d.emotion=="surprised")prefix="O_O "; else if(d.emotion=="sad")prefix="T_T ";
            else if(d.emotion=="speaking")prefix=">_< "; else prefix="-_- ";
            auto* emo = new std::string(d.emotion);
            lv_async_call(ui_update_emoji, emo);
            auto* asr = new std::pair<std::string,bool>(prefix+d.text, true);
            lv_async_call(ui_show_asr, asr);
        }
    }
    else if(t==ProtocolParser::MessageType::EMOTION){
        if(has_display) lv_async_call(ui_update_emoji, new std::string(ProtocolParser::parse_emotion(m)));
    }
    else if(t==ProtocolParser::MessageType::LED){
        std::string a,c; ProtocolParser::parse_led(m,a,c);
        if(a=="flash")leds->set_pattern(LEDController::REMINDER);
        else if(a=="off")leds->set_pattern(LEDController::OFF);
    }
    else if(t==ProtocolParser::MessageType::ASR_RESULT){
        bool fin=false; std::string txt=ProtocolParser::parse_asr_result(m,fin);
        if(has_display) lv_async_call(ui_show_asr, new std::pair<std::string,bool>(txt,fin));
    }
}

static uint32_t last_rec_stop = 0;
static bool rec_busy = false;
static void rec_start_fn() {
    if(rec || rec_busy) return;
    uint32_t n=xTaskGetTickCount()*portTICK_PERIOD_MS;
    if(n - last_rec_stop < 800) return;
    rec_busy = true; rec=true; rec_start=n;
    leds->set_pattern(LEDController::RECORDING); if(has_display){UIManager::instance().set_emotion("neutral");UIManager::instance().show_asr_text("\345\220\254\347\235\200...",false);} // 听着...
    audio->startRecording();
    if(ws&&ws->isConnected()) ws->sendText(ProtocolBuilder::build_audio_start());
}
static void rec_stop_fn() {
    if(!rec) return;
    rec=false; rec_busy=false; last_rec_stop=xTaskGetTickCount()*portTICK_PERIOD_MS;
    leds->set_pattern(LEDController::PROCESSING); if(has_display)UIManager::instance().set_emotion("thinking");
    audio->stopRecording();
    size_t len=0; const int16_t* d=audio->getRecordingBuffer(len);
    if(ws&&ws->isConnected()&&len>SR/4){
        auto*raw=(const uint8_t*)d; size_t bytes=len*2;
        for(size_t off=0;off<bytes;off+=4096) ws->sendBinary(raw+off,std::min((size_t)4096,bytes-off));
        ws->sendText(ProtocolBuilder::build_audio_end()); st=PROCESSING;
    }else{audio->clearRecordingBuffer();st=IDLE;rec_busy=false;leds->set_pattern(LEDController::IDLE);}
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
    lc.duty = 0;  // GPIO_11 sinks BL_K through resistor → LOW=ON, HIGH=OFF
    ledc_channel_config(&lc);

    // 2. SPI bus — boost drive strength for clean signals on breadboard
    gpio_set_drive_capability(MOSI, GPIO_DRIVE_CAP_3);  // 40mA for MOSI
    gpio_set_drive_capability(CLK, GPIO_DRIVE_CAP_3);   // 40mA for CLK
    gpio_set_drive_capability(DC, GPIO_DRIVE_CAP_2);    // 20mA for DC

    spi_bus_config_t sb = {};
    sb.mosi_io_num = MOSI;
    sb.miso_io_num = -1;
    sb.sclk_io_num = CLK;
    sb.quadwp_io_num = -1;
    sb.quadhd_io_num = -1;
    sb.max_transfer_sz = LCD_H_RES * LCD_V_RES * 2;
    spi_bus_initialize(SPI2_HOST, &sb, SPI_DMA_CH_AUTO);

    // 3. Panel IO (SPI)
    // NOTE: Most 1.54" ST7789 boards use SPI mode 0. Mode 3 is for 7-pin variants.
    // If ghosting persists, toggle between 0 and 3.
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = CS;
    io.dc_gpio_num = DC;
    io.spi_mode = 0;  // SPI mode 0 (standard 4-wire ST7789)
    io.pclk_hz = 20000000;  // 20MHz (reduced from 40MHz for signal integrity on breadboard)
    io.trans_queue_depth = 10;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    esp_lcd_panel_io_handle_t io_h = nullptr;
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &io_h);

    // 4. ST7789 240x240 panel (1.54" IPS TFT)
    esp_lcd_panel_dev_config_t pd = {};
    pd.reset_gpio_num = RST;
    pd.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;  // 1.54" IPS ST7789 uses BGR subpixel order
    pd.data_endian = LCD_RGB_DATA_ENDIAN_BIG;       // ST7789 datasheet: RGB565 MSB-first
    pd.bits_per_pixel = 16;
    esp_lcd_new_panel_st7789(io_h, &pd, &panel_handle);

    // 5. ST7789 init (240x240 typically needs invert + swap_xy depending on panel)
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, false);
    esp_lcd_panel_swap_xy(panel_handle, false);
    esp_lcd_panel_mirror(panel_handle, false, false);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // 6. LVGL port
    lvgl_port_cfg_t lvcfg = {};
    lvcfg.task_priority = 4;
    lvcfg.task_stack = 8192;        // larger stack for font rendering
    lvcfg.task_max_sleep_ms = 50;   // increased for CJK font rendering time
    lvcfg.timer_period_ms = 5;
    lvgl_port_init(&lvcfg);

    lvgl_port_display_cfg_t dc = {};
    dc.io_handle = io_h;
    dc.panel_handle = panel_handle;
    dc.buffer_size = LCD_H_RES * 40;  // 40 lines buffer (bigger = fewer flush ops)
    dc.double_buffer = false;
    dc.hres = LCD_H_RES;
    dc.vres = LCD_V_RES;
    dc.monochrome = false;
    dc.flags.buff_dma = true;
    dc.flags.buff_spiram = false;  // DMA needs internal RAM
    lv_disp_t* disp = lvgl_port_add_disp(&dc);
    if (!disp) {
        ESP_LOGE(TAG, "LVGL display registration failed");
        return ESP_FAIL;
    }

#ifdef DDL_REMOTE_DISPLAY
    remote_disp = new RemoteDisplay();
    remote_disp->init(disp);
#endif

    // 7. Touch I2C (FT6336U, addr 0x38)
    i2c_config_t i2c = {};
    i2c.mode = I2C_MODE_MASTER;
    i2c.sda_io_num = GPIO_NUM_21;
    i2c.scl_io_num = GPIO_NUM_45;
    i2c.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c.master.clk_speed = 100000;  // 100kHz — more tolerant without external pull-ups
    i2c_param_config(I2C_NUM_0, &i2c);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

    // Quick I2C probe: scan known touch addresses only
    ESP_LOGI(TAG, "I2C probing touch addresses...");
    uint8_t probe_addrs[] = {0x38, 0x5D, 0x24, 0x70, 0x48, 0x2A, 0x15, 0x3A};
    for (int i = 0; i < 8; i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (probe_addrs[i] << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(I2C_NUM_0, cmd, 20 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (r == ESP_OK) ESP_LOGI(TAG, "  Touch found at 0x%02X", probe_addrs[i]);
    }

    // Try FT6336U at multiple known addresses
    static TouchFT6336 touch;
    esp_err_t tr = ESP_FAIL;
    uint8_t addrs[] = {0x38, 0x5D, 0x24, 0x2A, 0x48, 0x70};
    for (int i = 0; i < 6; i++) {
        tr = touch.init(I2C_NUM_0, GPIO_NUM_21, GPIO_NUM_45, GPIO_NUM_13, GPIO_NUM_14,
                        LCD_H_RES, LCD_V_RES, addrs[i]);
        if (tr == ESP_OK) break;
    }
    if (tr == ESP_OK) {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = [](lv_indev_drv_t*, lv_indev_data_t* d) {
            auto p = touch.read();
            d->state = p.pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
            d->point.x = p.x; d->point.y = p.y;
        };
        lv_indev_drv_register(&indev_drv);
        ESP_LOGI(TAG, "FT6336U touch registered as LVGL input");
    }

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
        // Process pending DDL list update OUTSIDE lv_timer_handler to avoid WDT
        if (has_display && pending_ddl_update) {
            auto* ev = pending_ddl_update;
            pending_ddl_update = nullptr;
            UIManager::instance().update_ddl_list(*ev);
            delete ev;
        }
        if (has_display) { lv_timer_handler(); lv_task_handler(); }
        if(leds) leds->update();

#ifdef DDL_REMOTE_DISPLAY
        if (remote_disp) remote_disp->update();
#endif

        // Audio capture: read from I2S mic while recording (buffer locally only)
        if (rec && audio_buf && !audio->isRecordingBufferFull()) {
            esp_err_t r = bsp_get_feed_data(false, audio_buf, chunk_samples * sizeof(int16_t));
            if (r == ESP_OK) {
                audio->addRecordingData(audio_buf, chunk_samples);
            }
        }

        // EC11 encoder: rotation + button with heavy debounce
        if (enc && has_display) {
            int32_t count = enc->encoder_count_.load();
            int32_t raw = count - last_enc_count;
            // Heavy debounce: need 6 detents for one logical step
            int32_t step = raw / 6;
            if (step != 0) {
                last_enc_count = count - (raw - step * 6);  // track remainder
                auto scr = UIManager::instance().current_screen();
                bool cw = (step > 0);
                int abs_step = (step > 0) ? step : -step;

                for (int s = 0; s < abs_step; s++) {
                switch (scr) {
                case UIManager::Screen::MAIN:
                    // Rotate = switch tabs (carousel style)
                    UIManager::instance().select_tab(cw ? 1 : -1);
                    break;
                case UIManager::Screen::LIST:
                    // Rotate = prev/next item (snap, not free scroll)
                    if (!events.empty()) {
                        lv_obj_t* list = UIManager::instance().get_event_list();
                        if (list) {
                            int cnt = lv_obj_get_child_cnt(list);
                            if (cnt > 0) {
                                static int list_idx = 0;
                                if (cw) list_idx = (list_idx + 1) % cnt;
                                else list_idx = (list_idx - 1 + cnt) % cnt;
                                lv_obj_scroll_to_view(lv_obj_get_child(list, list_idx), LV_ANIM_ON);
                            }
                        }
                    }
                    break;
                case UIManager::Screen::SETTINGS:
                    // Rotate = select settings item / adjust volume if editing
                    {
                        static int vol = 8;
                        vol = std::max(0, std::min(10, vol + (cw ? 1 : -1)));
                        char vbuf[32];
                        snprintf(vbuf, sizeof(vbuf), "Vol: %d/10", vol);
                        UIManager::instance().show_asr_text(vbuf, true);
                    }
                    break;
                case UIManager::Screen::DETAIL:
                    if (!events.empty()) {
                        static int detail_idx = 0;
                        if (cw) detail_idx = (detail_idx + 1) % events.size();
                        else detail_idx = (detail_idx - 1 + events.size()) % events.size();
                        UIManager::instance().show_detail(events[detail_idx]);
                    }
                    break;
                default: break;
                }
                }
            }

            // EC11 button with debounce (50ms)
            bool now_pressed = enc->button_pressed_.load();
            static uint32_t last_btn_change = 0;
            if (now_pressed != enc_was_pressed && (n - last_btn_change) > 50) {
                last_btn_change = n;
                enc_was_pressed = now_pressed;
            }
            if (enc_was_pressed && !now_pressed) {
                uint32_t dur = n - enc->button_press_time_.load();
                auto scr = UIManager::instance().current_screen();

                if (dur < 1000) {  // Short press
                    switch (scr) {
                    case UIManager::Screen::MAIN:
                        UIManager::instance().enter_tab();  // enter selected tab
                        break;
                    case UIManager::Screen::LIST:
                        // Select the highlighted item: find focused child
                        {
                            lv_obj_t* list = UIManager::instance().get_event_list();
                            if (list && lv_obj_get_child_cnt(list) > 0) {
                                // Trigger click on the first visible child
                                lv_obj_t* child = lv_obj_get_child(list, 0);
                                if (child) lv_event_send(child, LV_EVENT_CLICKED, nullptr);
                            }
                        }
                        break;
                    case UIManager::Screen::SETTINGS:
                        UIManager::instance().show_screen(UIManager::Screen::MAIN);
                        break;
                    case UIManager::Screen::DETAIL:
                        UIManager::instance().show_screen(UIManager::Screen::MAIN);
                        break;
                    case UIManager::Screen::REMINDER_POPUP:
                        UIManager::instance().show_screen(UIManager::Screen::MAIN);
                        break;
                    default: break;
                    }
                } else {  // Long press (>1s) = back to main
                    UIManager::instance().show_screen(UIManager::Screen::MAIN);
                }
            }
            enc_was_pressed = now_pressed;
        }

        // Separate: mic recording controlled by touch button, not encoder
        // The encoder button no longer triggers recording

        if (has_display && n-lc>1000){
            UIManager::instance().update_clock();lc=n;
            // Auto-refresh countdown on detail screen
            if(UIManager::instance().current_screen()==UIManager::Screen::DETAIL && !events.empty()){
                for(auto& ev : events){
                    if(ev.id == UIManager::instance().current_detail_id()){
                        UIManager::instance().show_detail(ev); break;
                    }
                }
            }
        }
        if(ws&&ws->isConnected()&&n-lp>30000){ws->sendText(ProtocolBuilder::build_ping());lp=n;}
        if(rec&&(n-rec_start>MAX_REC_MS)) rec_stop_fn();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
