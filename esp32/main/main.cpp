#include <stdio.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <atomic>
#include <utility>
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

#if __has_include("device_config.h")
#include "device_config.h"
#else
#define WIFI_SSID  "YOUR_WIFI_SSID"
#define WIFI_PASS  "YOUR_WIFI_PASSWORD"
#define WS_URI     "ws://192.168.1.100:8888"
#endif
#define LCD_H_RES  240
#define LCD_V_RES  320
#define LCD_ROTATE_180 1
// Remote display (UDP mirror to PC) — define to enable
#define DDL_REMOTE_DISPLAY 0  // disabled: UDP send stalls LVGL flush, triggers WDT
#if DDL_REMOTE_DISPLAY
#include "remote_display.h"
static RemoteDisplay* remote_disp = nullptr;
#endif

static bool has_display = false;
static std::atomic<bool> rec_start_requested{false};
static std::atomic<bool> rec_stop_requested{false};
static std::atomic<bool> ddl_sync_requested{false};
static std::atomic<bool> pending_sync_completion{false};
static std::atomic<uint32_t> ddl_sync_request_serial{0};
static std::atomic<uint32_t> ddl_sync_inflight_id{0};
static std::atomic<uint32_t> ddl_sync_sent_at{0};
static std::atomic<bool> ddl_sync_waiting{false};

enum State { IDLE, RECORDING, PROCESSING, SPEAKING, REMINDER };
static State st = IDLE;
static WiFiManager* wifi = nullptr;
static WebSocketClient* ws = nullptr;
static AudioManager* audio = nullptr;
static LEDController* leds = nullptr;
static bool rec = false;
static uint32_t rec_start = 0;
static bool tts = false;
static std::string tts_stream_id;
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
            if (has_display) { if(lvgl_port_lock(2000)){ UIManager::instance().set_connected(true); lvgl_port_unlock(); } }
            ws->sendText(ProtocolBuilder::build_hello());
            ws->sendText(ProtocolBuilder::build_request_sync()); break;
        case WebSocketClient::EventType::DISCONNECTED:
            ESP_LOGI(TAG,"WS disconnected"); if(has_display){ if(lvgl_port_lock(2000)){ UIManager::instance().set_connected(false); lvgl_port_unlock(); } }
            if(tts&&audio) audio->finishStreamingPlayback();
            tts=false; tts_stream_id.clear();
            if(rec) rec_stop_requested.store(true);
            st=IDLE; ws->connect(); break;
        case WebSocketClient::EventType::DATA_TEXT:
            proc_msg(std::string((const char*)e.data,e.data_len)); break;
        case WebSocketClient::EventType::DATA_BINARY:
            if(audio&&tts&&e.data_len>0){
                if (!audio->addStreamingAudioChunk(e.data,e.data_len)) {
                    ESP_LOGW(TAG, "Dropped TTS audio chunk: stream=%s bytes=%u",
                             tts_stream_id.c_str(), static_cast<unsigned>(e.data_len));
                }
            }
            break;
        case WebSocketClient::EventType::PING:
            // WebSocket PING is connection keepalive, never a TTS boundary.
            break;
        default: break;
    }
}

// UI update callbacks (called from LVGL task via lv_async_call)
// NOTE: update_ddl_list creates many LVGL objects and WDT if run in timer.
// Instead, we set a pending flag and process in the main loop.
static std::atomic<std::vector<DDLEvent>*> pending_ddl_update{nullptr};
static void queue_ddl_update(std::vector<DDLEvent>* update) {
    // Coalesce sync bursts instead of putting large list updates on LVGL's async
    // queue. The main loop consumes only the newest snapshot under the LVGL lock.
    auto* superseded = pending_ddl_update.exchange(update);
    delete superseded;
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
static void ui_show_tool_result(void* p) {
    auto* result = static_cast<ProtocolParser::ToolResultData*>(p);
    UIManager::instance().show_tool_result(result->tool, result->success, result->message);
    delete result;
}

// Thread-safe lv_async_call: LVGL is not thread-safe, so the enqueue must hold the port lock.
// (esp_lvgl_port has no async-call helper in this version; lvgl_port_lock is recursive.)
static bool ui_async_call(lv_async_cb_t cb, void* data) {
    if (!has_display) return false;
    if (lvgl_port_lock(2000)) {
        lv_res_t result = lv_async_call(cb, data);
        lvgl_port_unlock();
        return result == LV_RES_OK;
    }
    return false;
}

static void proc_msg(const std::string& m) {
    auto t=ProtocolParser::get_message_type(m);
    if(t==ProtocolParser::MessageType::AUDIO_STREAM_START){
        const std::string stream_id = ProtocolParser::parse_audio_stream_id(m);
        if(audio){
            if(tts) audio->finishStreamingPlayback();
            audio->startStreamingPlayback();
            tts=true;
            tts_stream_id=stream_id;
            st=SPEAKING;
            if(leds) leds->set_pattern(LEDController::SPEAKING);
            ESP_LOGI(TAG, "TTS stream start: %s", tts_stream_id.c_str());
        }
        return;
    }
    if(t==ProtocolParser::MessageType::AUDIO_STREAM_END){
        const std::string stream_id = ProtocolParser::parse_audio_stream_id(m);
        if(tts&&audio&&(stream_id.empty()||stream_id==tts_stream_id)){
            audio->finishStreamingPlayback();
            ESP_LOGI(TAG, "TTS stream end: %s", tts_stream_id.c_str());
            tts=false;
            tts_stream_id.clear();
            if(st==SPEAKING){st=IDLE;if(leds)leds->set_pattern(LEDController::IDLE);}
        } else if(tts) {
            ESP_LOGW(TAG, "Ignoring mismatched TTS end: active=%s received=%s",
                     tts_stream_id.c_str(), stream_id.c_str());
        }
        return;
    }
    if(t==ProtocolParser::MessageType::SYNC){
        auto d=ProtocolParser::parse_sync(m); events=d.events;
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        bool requested = ddl_sync_waiting.exchange(false);
        uint32_t request_id = ddl_sync_inflight_id.load();
        uint32_t elapsed = requested ? now - ddl_sync_sent_at.load() : 0;
        ESP_LOGI(TAG, "DDL SYNC received: request=%s id=%u events=%u elapsed=%ums payload=%uB",
                 requested ? "manual" : "server/boot", request_id,
                 static_cast<unsigned>(events.size()), static_cast<unsigned>(elapsed),
                 static_cast<unsigned>(m.size()));
        pending_sync_completion.store(true);
        if(has_display) queue_ddl_update(new std::vector<DDLEvent>(events));
    }
    else if(t==ProtocolParser::MessageType::NEW_EVENT){
        auto e=ProtocolParser::parse_new_event(m); events.push_back(e);
        if(has_display) queue_ddl_update(new std::vector<DDLEvent>(events));
    }
    else if(t==ProtocolParser::MessageType::DELETE_EVENT){
        std::string id=ProtocolParser::parse_delete_event(m);
        events.erase(std::remove_if(events.begin(),events.end(),[&](auto&x){return x.id==id;}),events.end());
        if(has_display) queue_ddl_update(new std::vector<DDLEvent>(events));
    }
    else if(t==ProtocolParser::MessageType::REMIND){
        auto d=ProtocolParser::parse_remind(m);
        if(has_display) {
            auto* update = new DDLEvent(d.event);
            if (!ui_async_call(ui_show_reminder, update)) delete update;
        }
        leds->set_pattern(LEDController::REMINDER); st=REMINDER;
    }
    else if(t==ProtocolParser::MessageType::SPEAK){
        auto d=ProtocolParser::parse_speak(m);
        // Playback state starts with AUDIO_STREAM_START. A text-only response
        // must not leave the device permanently in SPEAKING state.
        if(!tts){st=IDLE;if(leds)leds->set_pattern(LEDController::IDLE);}
        if(has_display){
            std::string prefix;
            if(d.emotion=="happy")prefix="^_^ "; else if(d.emotion=="thinking")prefix="o.O ";
            else if(d.emotion=="surprised")prefix="O_O "; else if(d.emotion=="sad")prefix="T_T ";
            else if(d.emotion=="speaking")prefix=">_< "; else prefix="-_- ";
            auto* emo = new std::string(d.emotion);
            if (!ui_async_call(ui_update_emoji, emo)) delete emo;
            auto* asr = new std::pair<std::string,bool>("助手: "+prefix+d.text, true);
            if (!ui_async_call(ui_show_asr, asr)) delete asr;
        }
    }
    else if(t==ProtocolParser::MessageType::EMOTION){
        if(has_display) {
            auto* update = new std::string(ProtocolParser::parse_emotion(m));
            if (!ui_async_call(ui_update_emoji, update)) delete update;
        }
    }
    else if(t==ProtocolParser::MessageType::LED){
        std::string a,c; ProtocolParser::parse_led(m,a,c);
        if(a=="flash")leds->set_pattern(LEDController::REMINDER);
        else if(a=="off")leds->set_pattern(LEDController::OFF);
    }
    else if(t==ProtocolParser::MessageType::ASR_RESULT){
        bool fin=false; std::string txt=ProtocolParser::parse_asr_result(m,fin);
        if(has_display) {
            auto* update = new std::pair<std::string,bool>("我: "+txt,fin);
            if (!ui_async_call(ui_show_asr, update)) delete update;
        }
    }
    else if(t==ProtocolParser::MessageType::TOOL_RESULT){
        auto data = ProtocolParser::parse_tool_result(m);
        ESP_LOGI(TAG, "Tool result received: tool=%s success=%s message=%s",
                 data.tool.c_str(), data.success ? "true" : "false", data.message.c_str());
        if(has_display) {
            auto* update = new ProtocolParser::ToolResultData(std::move(data));
            if (!ui_async_call(ui_show_tool_result, update)) delete update;
        }
    }
    else if(t==ProtocolParser::MessageType::UNKNOWN){
        const size_t preview_len = std::min<size_t>(m.size(), 64);
        ESP_LOGW(TAG, "Unrecognized WebSocket text: %uB prefix='%.*s'",
                 static_cast<unsigned>(m.size()), static_cast<int>(preview_len), m.c_str());
    }
}

static uint32_t last_rec_stop = 0;
static bool rec_busy = false;
static void rec_start_fn() {
    if(rec || rec_busy) return;
    uint32_t n=xTaskGetTickCount()*portTICK_PERIOD_MS;
    if(n - last_rec_stop < 800) return;
    rec_busy = true; rec=true; rec_start=n;
    leds->set_pattern(LEDController::RECORDING);
    if(has_display){ if(lvgl_port_lock(2000)){ UIManager::instance().set_emotion("neutral"); UIManager::instance().show_asr_text("\345\220\254\347\235\200...",false); lvgl_port_unlock(); } } // 听着...
    audio->startRecording();
    if(ws&&ws->isConnected()) ws->sendText(ProtocolBuilder::build_audio_start());
}
static void rec_stop_fn() {
    if(!rec) return;
    rec=false; rec_busy=false; last_rec_stop=xTaskGetTickCount()*portTICK_PERIOD_MS;
    leds->set_pattern(LEDController::PROCESSING);
    if(has_display){ if(lvgl_port_lock(2000)){ UIManager::instance().set_emotion("thinking"); lvgl_port_unlock(); } }
    audio->stopRecording();
    size_t len=0; const int16_t* d=audio->getRecordingBuffer(len);
    int peak = 0;
    uint64_t sum_sq = 0;
    size_t clipped = 0;
    for (size_t i = 0; i < len; ++i) {
        int sample = d[i];
        int magnitude = sample == -32768 ? 32768 : std::abs(sample);
        peak = std::max(peak, magnitude);
        sum_sq += static_cast<int64_t>(sample) * sample;
        if (magnitude >= 32760) ++clipped;
    }
    int rms = len ? static_cast<int>(sqrt(static_cast<double>(sum_sq) / len)) : 0;
    ESP_LOGI(TAG, "ASR capture: %u samples, %.2fs, peak=%d, rms=%d, clipped=%u",
             static_cast<unsigned>(len), static_cast<double>(len) / SR,
             peak, rms, static_cast<unsigned>(clipped));
    if(ws&&ws->isConnected()&&len>SR/4){
        auto*raw=(const uint8_t*)d; size_t bytes=len*2;
        bool sent_ok = true;
        for(size_t off=0;off<bytes;off+=4096) {
            size_t chunk = std::min((size_t)4096,bytes-off);
            if (ws->sendBinary(raw+off, chunk) != static_cast<int>(chunk)) {
                sent_ok = false;
                ESP_LOGE(TAG, "ASR audio upload stopped at %u/%u bytes",
                         static_cast<unsigned>(off), static_cast<unsigned>(bytes));
                break;
            }
        }
        if (sent_ok) {
            ws->sendText(ProtocolBuilder::build_audio_end()); st=PROCESSING;
        } else {
            audio->clearRecordingBuffer(); st=IDLE; leds->set_pattern(LEDController::IDLE);
        }
    }else{audio->clearRecordingBuffer();st=IDLE;rec_busy=false;leds->set_pattern(LEDController::IDLE);}
}

static esp_lcd_panel_handle_t panel_handle = nullptr;
static lv_disp_t* lvgl_display = nullptr;

// Reconfigure only the LCD controller. LVGL, touch, networking and the current
// screen stay alive, so a white panel can recover without rebooting the ESP32.
static esp_err_t lcd_apply_controller_config(bool hard_reset) {
    if (!panel_handle) return ESP_ERR_INVALID_STATE;

    esp_err_t err;
    if (hard_reset) {
        err = esp_lcd_panel_reset(panel_handle);
        if (err != ESP_OK) return err;
    }
    err = esp_lcd_panel_init(panel_handle);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_invert_color(panel_handle, false);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_swap_xy(panel_handle, false);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_mirror(panel_handle, LCD_ROTATE_180, LCD_ROTATE_180);
    if (err != ESP_OK) return err;
    return esp_lcd_panel_disp_on_off(panel_handle, true);
}

static esp_err_t recover_lcd(bool hard_reset) {
    if (!has_display || !panel_handle || !lvgl_display) return ESP_ERR_INVALID_STATE;
    if (!lvgl_port_lock(2000)) return ESP_ERR_TIMEOUT;

    ESP_LOGW(TAG, "LCD %s recovery started", hard_reset ? "hard" : "soft");
    // tx_param inside panel_init drains pending SPI color transactions first.
    esp_err_t err = lcd_apply_controller_config(hard_reset);
    if (err == ESP_OK) {
        lv_obj_invalidate(lv_disp_get_scr_act(lvgl_display));
        // Let esp_lvgl_port's dedicated task perform the redraw. Calling
        // lv_refr_now here makes main execute the complete software renderer,
        // including transient shadow allocations, even though LVGL has its own
        // task and watchdog constraints.
        ESP_LOGI(TAG, "LCD recovery completed; redraw queued");
    } else {
        ESP_LOGE(TAG, "LCD recovery failed: %s", esp_err_to_name(err));
    }
    lvgl_port_unlock();
    return err;
}

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
    esp_err_t err = ledc_timer_config(&lt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t lc = {};
    lc.gpio_num = BL;
    lc.speed_mode = LEDC_LOW_SPEED_MODE;
    lc.channel = LEDC_CHANNEL_0;
    lc.timer_sel = LEDC_TIMER_0;
    lc.duty = 0;  // Keep the backlight dark until the controller is configured.
    err = ledc_channel_config(&lc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config (backlight) failed: %s", esp_err_to_name(err));
        return err;
    }
    // 1. Backlight - GPIO direct control (FOR TESTING)
    /*esp_err_t err = ESP_OK;
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_11);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    gpio_set_level(GPIO_NUM_11, 1);  // 高电平点亮
    ESP_LOGI(TAG, "Backlight GPIO set HIGH - screen should be lit!");
    */

    // 2. SPI bus. Deterministic idle levels reduce the chance that a briefly
    // floating Dupont contact is interpreted as an ST7789 command.
    gpio_set_pull_mode(CS, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(DC, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(RST, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(CLK, GPIO_PULLDOWN_ONLY);
    gpio_set_pull_mode(MOSI, GPIO_PULLDOWN_ONLY);
    // Restore the signal settings that were stable on this same wiring. At 4 MHz
    // every marquee refresh held the bus active five times longer than before.
    gpio_set_drive_capability(MOSI, GPIO_DRIVE_CAP_3);  // 40mA
    gpio_set_drive_capability(CLK, GPIO_DRIVE_CAP_3);   // 40mA
    gpio_set_drive_capability(DC, GPIO_DRIVE_CAP_2);    // 20mA
    gpio_set_drive_capability(CS, GPIO_DRIVE_CAP_2);    // 20mA
    gpio_set_drive_capability(RST, GPIO_DRIVE_CAP_2);   // 20mA

    spi_bus_config_t sb = {};
    sb.mosi_io_num = MOSI;
    sb.miso_io_num = -1;
    sb.sclk_io_num = CLK;
    sb.quadwp_io_num = -1;
    sb.quadhd_io_num = -1;
    sb.max_transfer_sz = LCD_H_RES * LCD_V_RES * 2;
    err = spi_bus_initialize(SPI2_HOST, &sb, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 3. Panel IO (SPI)
    // NOTE: Most 1.54" ST7789 boards use SPI mode 0. Mode 3 is for 7-pin variants.
    // If ghosting persists, toggle between 0 and 3.
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = CS;
    io.dc_gpio_num = DC;
    io.spi_mode = 0;  // SPI mode 0 (standard 4-wire ST7789)
    io.pclk_hz = 20000000; // known-good setting used before the white-screen regression
    io.trans_queue_depth = 3; // bound outstanding DMA traffic during screen changes
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    esp_lcd_panel_io_handle_t io_h = nullptr;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &io_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel IO create failed: %s", esp_err_to_name(err));
        return err;
    }

    // 4. ST7789 240x320 panel
    esp_lcd_panel_dev_config_t pd = {};
    pd.reset_gpio_num = RST;
    // With RGB565 byte order fixed below, this panel's MADCTL must use RGB.
    // BGR swaps pure red and blue (the red delete button appeared blue).
    pd.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    // LVGL's RGB565 buffer is little-endian on ESP32. Tell ST7789 RAMCTRL to
    // consume that order; BIG here byte-swaps every pixel (red->green, blue->yellow).
    pd.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    pd.bits_per_pixel = 16;
    err = esp_lcd_new_panel_st7789(io_h, &pd, &panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel create failed: %s", esp_err_to_name(err));
        return err;
    }

    // RST has been held high since before SPI initialization.

    // 5. ST7789 init
    err = lcd_apply_controller_config(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023);
    if (err == ESP_OK) err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Backlight enable failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Backlight PWM set to max brightness");

    // 6. LVGL port
    lvgl_port_cfg_t lvcfg = {};
    lvcfg.task_priority = 4;
    lvcfg.task_stack = 8192;        // larger stack for font rendering
    lvcfg.task_max_sleep_ms = 50;   // increased for CJK font rendering time
    lvcfg.timer_period_ms = 5;
    err = lvgl_port_init(&lvcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port init failed: %s", esp_err_to_name(err));
        return err;
    }

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
    lvgl_display = lvgl_port_add_disp(&dc);
    if (!lvgl_display) {
        ESP_LOGE(TAG, "LVGL display registration failed");
        return ESP_FAIL;
    }

#if DDL_REMOTE_DISPLAY
    remote_disp = new RemoteDisplay();
    remote_disp->init(lvgl_display);
#endif

    // 7. Touch I2C (FT6336U, addr 0x38)
    i2c_config_t i2c = {};
    i2c.mode = I2C_MODE_MASTER;
    i2c.sda_io_num = GPIO_NUM_21;
    i2c.scl_io_num = GPIO_NUM_45;
    i2c.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c.master.clk_speed = 100000;  // 100kHz — more tolerant without external pull-ups
    err = i2c_param_config(I2C_NUM_0, &i2c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return err;
    }

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
#if LCD_ROTATE_180
            if (p.pressed) {
                p.x = LCD_H_RES - 1 - p.x;
                p.y = LCD_V_RES - 1 - p.y;
            }
#endif
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
    // Uploading a recording can take hundreds of milliseconds. Keep that work
    // out of the LVGL callback so rendering and SPI flushes cannot be starved.
    g_on_mic_press = [](){ if(st==IDLE) rec_start_requested.store(true); };
    g_on_mic_release = [](){ rec_stop_requested.store(true); };
    g_on_event_action = [](const std::string& a){ if(ws&&ws->isConnected()) ws->sendText(ProtocolBuilder::build_event_action(UIManager::instance().current_detail_id(),a)); };
    g_on_request_sync = [](){ ddl_sync_requested.store(true); };
    g_on_volume_changed = [](int level){ if(audio) audio->setVolume(level); };
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG,"=== DDL Reminder ESP32-S3 ===");
    esp_err_t r=nvs_flash_init();
    if(r==ESP_ERR_NVS_NO_FREE_PAGES||r==ESP_ERR_NVS_NEW_VERSION_FOUND){nvs_flash_erase();nvs_flash_init();}

    esp_err_t lr = init_lcd();
    if (lr != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed: %s — display disabled", esp_err_to_name(lr));
    }
    leds=new LEDController(GPIO_NUM_41,4); leds->init(); leds->set_pattern(LEDController::IDLE);
    auto* enc = new EncoderManager(GPIO_NUM_38,GPIO_NUM_39,GPIO_NUM_40);
    esp_err_t er = enc->init();
    if (er != ESP_OK) ESP_LOGE(TAG, "EC11 init failed: %s", esp_err_to_name(er));

    esp_err_t br = bsp_board_init(16000,1,16);
    if (br != ESP_OK) ESP_LOGE(TAG, "Mic I2S init failed: %s", esp_err_to_name(br));
    esp_err_t ar = bsp_audio_init(16000,1,16);
    if (ar != ESP_OK) ESP_LOGE(TAG, "Amp I2S init failed: %s", esp_err_to_name(ar));
    audio=new AudioManager(16000,10,32);
    if (audio->init() != ESP_OK) ESP_LOGE(TAG, "AudioManager init failed");

    // 🔊 Boot test tone (1kHz, 300ms) — verifies MAX98357A + speaker wiring without the server.
    // If you hear nothing, the problem is on the board (power/speaker/amp), not the server.
    if (ar == ESP_OK) {
        const int tone_sr = 16000, tone_ms = 300;
        const int tone_n = tone_sr * tone_ms / 1000;
        int16_t* tone = (int16_t*)malloc(tone_n * sizeof(int16_t));
        if (tone) {
            for (int i = 0; i < tone_n; i++) {
                tone[i] = (int16_t)(4000.0f * sinf(2.0f * 3.14159265358979f * 1000.0f * (float)i / tone_sr));
            }
            esp_err_t tr = bsp_play_audio((const uint8_t*)tone, tone_n * sizeof(int16_t));
            ESP_LOGI(TAG, "Boot test tone: %s", esp_err_to_name(tr));
            free(tone);
        }
    }

    wifi=new WiFiManager(WIFI_SSID,WIFI_PASS); wifi->connect();
    // NTP time sync (after WiFi is up)
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    setenv("TZ", "CST-8", 1); tzset();
    ESP_LOGI(TAG,"NTP sync started");

    ws=new WebSocketClient(WS_URI,false,5000); ws->setEventCallback(ws_cb); ws->connect();

    if (has_display) {
        // LVGL task is already running (lvgl_port_init in init_lcd) — build UI under the lock
        if (lvgl_port_lock(2000)) {
            UIManager::instance().init();
            lvgl_port_unlock();
        }
        init_ui_cb();
    }
    ESP_LOGI(TAG,"Ready. Display=%s", has_display?"yes":"no");

    // Audio capture buffer (320 samples = 20ms at 16kHz)
    const int chunk_samples = 320;
    int16_t* audio_buf = (int16_t*)malloc(chunk_samples * sizeof(int16_t));

    uint32_t lc=0,lp=0;
    int32_t last_enc_count = 0;
    while(1){
        uint32_t n=xTaskGetTickCount()*portTICK_PERIOD_MS;
        if (rec_start_requested.exchange(false)) rec_start_fn();
        if (rec_stop_requested.exchange(false)) rec_stop_fn();
        if (ddl_sync_requested.exchange(false)) {
            uint32_t request_id = ddl_sync_request_serial.fetch_add(1) + 1;
            bool connected = ws && ws->isConnected();
            ddl_sync_inflight_id.store(request_id);
            ddl_sync_sent_at.store(n);
            ddl_sync_waiting.store(connected);
            std::string request = ProtocolBuilder::build_request_sync();
            int sent_bytes = connected ? ws->sendText(request) : -1;
            bool sent = sent_bytes == static_cast<int>(request.size());
            ESP_LOGI(TAG, "DDL refresh #%u: ws=%s uri=%s sent=%d/%u",
                     request_id, connected ? "connected" : "disconnected", WS_URI,
                     sent_bytes, static_cast<unsigned>(request.size()));
            if (!sent) {
                ddl_sync_waiting.store(false);
                ESP_LOGE(TAG, "DDL refresh #%u failed before server response: %s",
                         request_id, connected ? "WebSocket send incomplete" : "WebSocket disconnected");
            }
            if (!sent && has_display && lvgl_port_lock(100)) {
                UIManager::instance().notify_ddl_sync(false);
                lvgl_port_unlock();
            }
        }
        // Process pending DDL list update OUTSIDE the LVGL task (which owns lv_timer_handler).
        // LVGL is not thread-safe: every UI access from this loop must take the port lock.
        if (has_display) {
            auto* ev = pending_ddl_update.exchange(nullptr);
            if (ev) {
                if (lvgl_port_lock(2000)) {
                    UIManager::instance().update_ddl_list(*ev);
                    if (pending_sync_completion.exchange(false)) {
                        ESP_LOGI(TAG, "DDL SYNC applied to UI: %u events",
                                 static_cast<unsigned>(ev->size()));
                        UIManager::instance().notify_ddl_sync(true);
                    }
                    lvgl_port_unlock();
                    delete ev;
                } else {
                    // Do not lose a server response just because LVGL was busy.
                    // Keep the newest queued snapshot and retry it next loop.
                    std::vector<DDLEvent>* expected = nullptr;
                    if (!pending_ddl_update.compare_exchange_strong(expected, ev)) {
                        delete ev;  // A newer snapshot is already waiting.
                    }
                    ESP_LOGW(TAG, "DDL SYNC UI apply delayed: LVGL lock timeout");
                }
            }
        }
        // NOTE: lv_timer_handler()/lv_task_handler() are NOT called here anymore —
        // esp_lvgl_port's own task already runs lv_timer_handler() in a loop. Calling it
        // from two tasks concurrently corrupts LVGL state (blank UI / crashes / WDT resets).
        if(leds) leds->update();

#if DDL_REMOTE_DISPLAY
        if (remote_disp) remote_disp->update();
#endif

        // Audio capture: read from I2S mic while recording (buffer locally only)
        if (rec && audio_buf && !audio->isRecordingBufferFull()) {
            esp_err_t r = bsp_get_feed_data(false, audio_buf, chunk_samples * sizeof(int16_t));
            if (r == ESP_OK) {
                audio->addRecordingData(audio_buf, chunk_samples);
            }
        }

        // EC11 encoder: one mechanical detent is four valid Gray-code edges.
        if (enc && has_display) {
            int32_t count = enc->encoder_count_.load();
            int32_t raw = count - last_enc_count;
            int32_t step = raw / 4;
            if (step != 0) {
                last_enc_count += step * 4;
                if (lvgl_port_lock(50)) {
                    int direction = step > 0 ? 1 : -1;
                    for (int s = 0; s < std::abs((int)step); ++s) {
                        UIManager::instance().encoder_rotate(direction);
                    }
                    lvgl_port_unlock();
                }
            }

            // Debounce the physical level, then act once on the stable release.
            static bool raw_pressed = false;
            static bool stable_pressed = false;
            static uint32_t raw_changed_at = 0;
            static uint32_t pressed_at = 0;
            bool sample = enc->is_pressed();
            if (sample != raw_pressed) {
                raw_pressed = sample;
                raw_changed_at = n;
            }
            if (raw_pressed != stable_pressed && n - raw_changed_at >= 35) {
                bool old = stable_pressed;
                stable_pressed = raw_pressed;
                if (!old && stable_pressed) {
                    pressed_at = n;
                } else if (old && !stable_pressed) {
                    uint32_t held_ms = n - pressed_at;
                    if (held_ms >= 3000) {
                        // Emergency display recovery remains usable even when the
                        // panel is white and touch coordinates cannot be seen.
                        recover_lcd(true);
                        if (lvgl_port_lock(100)) {
                            UIManager::instance().request_ddl_refresh();
                            lvgl_port_unlock();
                        }
                    } else if (lvgl_port_lock(50)) {
                        if (held_ms >= 1000) UIManager::instance().show_screen(UIManager::Screen::MAIN);
                        else UIManager::instance().encoder_press();
                        lvgl_port_unlock();
                    }
                }
            }
        }




        // Separate: mic recording controlled by touch button, not encoder
        // The encoder button no longer triggers recording

        if (has_display && n-lc>1000){
            if (lvgl_port_lock(2000)) {
                UIManager::instance().update_clock();lc=n;
                // Auto-refresh countdown on detail screen
                if(UIManager::instance().current_screen()==UIManager::Screen::DETAIL &&
                   !UIManager::instance().is_editing_detail() && !events.empty()){
                    for(auto& ev : events){
                        if(ev.id == UIManager::instance().current_detail_id()){
                            UIManager::instance().refresh_detail_countdown(ev); break;
                        }
                    }
                }
                lvgl_port_unlock();
            }
        }
        if(ws&&ws->isConnected()&&n-lp>30000){ws->sendText(ProtocolBuilder::build_ping());lp=n;}
        if(rec&&(n-rec_start>MAX_REC_MS)) rec_stop_fn();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
