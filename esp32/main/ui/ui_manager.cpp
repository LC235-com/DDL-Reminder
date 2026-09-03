#include "ui_manager.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char* T = "UI";
extern std::vector<DDLEvent> events;

std::function<void()> g_on_mic_press;
std::function<void()> g_on_mic_release;
std::function<void(const std::string&)> g_on_event_action;
std::function<void()> g_on_request_sync;
std::function<void(int)> g_on_volume_changed;

UIManager& UIManager::instance() { static UIManager m; return m; }

esp_err_t UIManager::init() {
    mk_main(); mk_list(); mk_detail(); mk_reminder_editor(); mk_text_detail();
    mk_popup(); mk_settings(); mk_tool_toast();
    // mk_keyboard() — disabled: too many LVGL objects triggers WDT
    show_screen(MAIN); ESP_LOGI(T, "UI ready"); return ESP_OK;
}

void UIManager::show_screen(Screen s) {
    if(mscr) lv_obj_add_flag(mscr, LV_OBJ_FLAG_HIDDEN);
    if(lscr) lv_obj_add_flag(lscr, LV_OBJ_FLAG_HIDDEN);
    if(dscr) lv_obj_add_flag(dscr, LV_OBJ_FLAG_HIDDEN);
    if(reminderscr) lv_obj_add_flag(reminderscr, LV_OBJ_FLAG_HIDDEN);
    if(textscr) lv_obj_add_flag(textscr, LV_OBJ_FLAG_HIDDEN);
    if(setscr) lv_obj_add_flag(setscr, LV_OBJ_FLAG_HIDDEN);
    if(kbscr) lv_obj_add_flag(kbscr, LV_OBJ_FLAG_HIDDEN);
    switch(s) {
        case MAIN: if(mscr){lv_obj_clear_flag(mscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(mscr); update_clock();} break;
        case LIST: if(lscr){lv_obj_clear_flag(lscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(lscr); highlight_list_item();} break;
        case DETAIL: if(dscr){lv_obj_clear_flag(dscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(dscr);} break;
        case REMINDER_EDITOR: if(reminderscr){lv_obj_clear_flag(reminderscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(reminderscr); refresh_reminder_editor();} break;
        case TEXT_DETAIL: if(textscr){lv_obj_clear_flag(textscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(textscr);} break;
        case SETTINGS: if(setscr){lv_obj_clear_flag(setscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(setscr); refresh_settings();} break;
        case KEYBOARD: if(kbscr){lv_obj_clear_flag(kbscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(kbscr);} break;
        default: break;
    }
    scr_=s;
    if (s == MAIN) highlight_tab(TAB_HOME);
    else if (s == LIST) highlight_tab(TAB_DDL);
    else if (s == SETTINGS) highlight_tab(TAB_SETTINGS);
}

// Font chain: common punctuation/symbols -> CJK -> LVGL's built-in Latin/icons.
LV_FONT_DECLARE(my_chinese_font);
LV_FONT_DECLARE(common_symbols_font);

// RAM copy of default font chained to CJK fallback.
// When a glyph isn't in the default font, LVGL tries the fallback automatically.
static lv_font_t font_with_cjk;
static lv_font_t cjk_with_default;
static bool font_ready = false;

static void ensure_cjk_font() {
    if (font_ready) return;
    cjk_with_default = my_chinese_font;
    cjk_with_default.fallback = LV_FONT_DEFAULT;
    font_with_cjk = common_symbols_font;
    font_with_cjk.fallback = &cjk_with_default;
    font_ready = true;
}

static lv_obj_t* mk_label(lv_obj_t* p, const char* txt) {
    lv_obj_t* l=lv_label_create(p);
    lv_label_set_text(l,txt);
    ensure_cjk_font();
    lv_obj_set_style_text_font(l, &font_with_cjk, 0);
    return l;
}
static lv_obj_t* mk_btn(lv_obj_t* p, int w, int h, const char* txt) {
    lv_obj_t* b=lv_btn_create(p); lv_obj_set_size(b,w,h);
    // The default light theme adds a software-rendered shadow. On this small
    // device that path needs a temporary LVGL buffer and can stall in
    // draw_shadow when the UI heap is fragmented during a screen switch.
    lv_obj_set_style_shadow_width(b, 0, 0);
    if(txt){
        lv_obj_t* lb=lv_label_create(b);
        lv_label_set_text(lb,txt);
        lv_obj_center(lb);
        ensure_cjk_font();
        lv_obj_set_style_text_font(lb, &font_with_cjk, 0);
    }
    return b;
}

static void replace_all(std::string& text, const char* from, const char* to) {
    size_t pos = 0;
    const size_t from_len = strlen(from);
    while (from_len && (pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from_len, to);
        pos += strlen(to);
    }
}

// The compact CJK bitmap font cannot render the Unicode emoji planes. Convert
// common assistant/status emoji to readable expressions instead of showing □.
static std::string display_safe_text(std::string text) {
    // Compact CJK fonts often omit the Unicode Number Forms block. Roman
    // numerals are visually equivalent as Latin letters and remain searchable.
    replace_all(text, "\xE2\x85\xA0", "I");    // Ⅰ
    replace_all(text, "\xE2\x85\xA1", "II");   // Ⅱ
    replace_all(text, "\xE2\x85\xA2", "III");  // Ⅲ
    replace_all(text, "\xE2\x85\xA3", "IV");   // Ⅳ
    replace_all(text, "\xE2\x85\xA4", "V");    // Ⅴ
    replace_all(text, "\xE2\x85\xA5", "VI");   // Ⅵ
    replace_all(text, "\xE2\x85\xA6", "VII");  // Ⅶ
    replace_all(text, "\xE2\x85\xA7", "VIII"); // Ⅷ
    replace_all(text, "\xE2\x85\xA8", "IX");   // Ⅸ
    replace_all(text, "\xE2\x85\xA9", "X");    // Ⅹ
    replace_all(text, "\xE2\x85\xAA", "XI");   // Ⅺ
    replace_all(text, "\xE2\x85\xAB", "XII");  // Ⅻ
    replace_all(text, "\xE2\x9C\x85", LV_SYMBOL_OK);       // ✅
    replace_all(text, "\xE2\x9A\xA0\xEF\xB8\x8F", LV_SYMBOL_WARNING); // ⚠️
    replace_all(text, "\xE2\x9A\xA0", LV_SYMBOL_WARNING);  // ⚠
    replace_all(text, "\xF0\x9F\x94\xA5", "[紧急]"); // 🔥
    replace_all(text, "\xE2\x9A\xA1", "[急]");       // ⚡
    replace_all(text, "\xF0\x9F\x93\x8C", "[近期]"); // 📌
    replace_all(text, "\xF0\x9F\x98\x8A", "^_^");   // 😊
    replace_all(text, "\xF0\x9F\x98\x84", "^_^");   // 😄
    replace_all(text, "\xF0\x9F\x98\x82", "T_T");   // 😂
    replace_all(text, "\xF0\x9F\x98\xA2", "T_T");   // 😢
    replace_all(text, "\xF0\x9F\x98\xAE", "O_O");   // 😮
    replace_all(text, "\xF0\x9F\xA4\x94", "o.O");   // 🤔
    replace_all(text, "\xF0\x9F\x94\x94", "[提醒]"); // 🔔
    replace_all(text, "\xEF\xB8\x8F", "");           // variation selector
    // Replace any other emoji-plane code point with a readable placeholder.
    // This is preferable to LVGL's missing-glyph square and the coloured avatar
    // still carries the assistant's actual emotion.
    std::string safe;
    for (size_t i = 0; i < text.size();) {
        const uint8_t lead = (uint8_t)text[i];
        size_t length = 1;
        uint32_t codepoint = lead;
        if ((lead & 0xE0) == 0xC0 && i + 1 < text.size()) {
            length = 2; codepoint = ((lead & 0x1F) << 6) | ((uint8_t)text[i + 1] & 0x3F);
        } else if ((lead & 0xF0) == 0xE0 && i + 2 < text.size()) {
            length = 3; codepoint = ((lead & 0x0F) << 12) | (((uint8_t)text[i + 1] & 0x3F) << 6) | ((uint8_t)text[i + 2] & 0x3F);
        } else if ((lead & 0xF8) == 0xF0 && i + 3 < text.size()) {
            length = 4; codepoint = ((lead & 0x07) << 18) | (((uint8_t)text[i + 1] & 0x3F) << 12) |
                                  (((uint8_t)text[i + 2] & 0x3F) << 6) | ((uint8_t)text[i + 3] & 0x3F);
        }
        bool emoji = (codepoint >= 0x1F000 && codepoint <= 0x1FAFF) ||
                     (codepoint >= 0x2600 && codepoint <= 0x27BF);
        if (emoji) safe += "[表情]";
        else safe.append(text, i, length);
        i += length;
    }
    return safe;
}

static void add_bottom_tabs(lv_obj_t* parent, UIManager::Tab selected) {
    static const char* names[] = {"主页", "DDL", "设置"};
    for (int i = 0; i < UIManager::TAB_COUNT; ++i) {
        lv_obj_t* b = lv_btn_create(parent);
        lv_obj_set_size(b, 70, 32);
        lv_obj_set_pos(b, 7 + i * 78, 282);
        lv_obj_set_style_radius(b, 9, 0);
        lv_obj_set_style_border_width(b, i == (int)selected ? 2 : 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(i == (int)selected ? 0xd4af37 : 0x3a2f14), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(i == (int)selected ? 0x201900 : 0x101010), 0);
        lv_obj_t* l = mk_label(b, names[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(i == (int)selected ? 0xf4cf57 : 0x8c7a4f), 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            int tab = (int)(uintptr_t)lv_event_get_user_data(e);
            UIManager::instance().set_tab((UIManager::Tab)tab);
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }
}

void UIManager::mk_main() {
    mscr=lv_obj_create(NULL); lv_obj_set_size(mscr,240,320);
    lv_obj_set_style_bg_color(mscr,lv_color_hex(0x1a1a2e),0); lv_obj_set_style_pad_all(mscr,0,0);

    // === Row 1: Clock + Date (y=0..56) ===
    clock_lbl=mk_label(mscr,"00:00");
    lv_obj_set_style_text_font(clock_lbl, &font_with_cjk, 0);
    lv_obj_set_style_text_color(clock_lbl,lv_color_hex(0xffffff),0);
    lv_obj_align(clock_lbl,LV_ALIGN_TOP_MID,0,6);

    date_lbl=mk_label(mscr,"----.--.--");
    lv_obj_set_style_text_color(date_lbl,lv_color_hex(0xaaaaaa),0);
    lv_obj_align(date_lbl,LV_ALIGN_TOP_MID,0,34);

    // === Row 2: small colour avatar (doesn't depend on monochrome emoji fonts) ===
    avatar_ = lv_obj_create(mscr);
    lv_obj_set_size(avatar_, 64, 38);
    lv_obj_set_style_radius(avatar_, 18, 0);
    lv_obj_set_style_bg_color(avatar_, lv_color_hex(0xffd5bd), 0);
    lv_obj_set_style_border_color(avatar_, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(avatar_, 1, 0);
    lv_obj_set_style_pad_all(avatar_, 0, 0);
    lv_obj_align(avatar_, LV_ALIGN_TOP_MID, 0, 53);
    avatar_hair_ = lv_obj_create(avatar_);
    lv_obj_set_size(avatar_hair_, 62, 13); lv_obj_set_pos(avatar_hair_, 0, 0);
    lv_obj_set_style_radius(avatar_hair_, 12, 0);
    lv_obj_set_style_bg_color(avatar_hair_, lv_color_hex(0x5b3a70), 0);
    lv_obj_set_style_border_width(avatar_hair_, 0, 0);
    lv_obj_set_style_pad_all(avatar_hair_, 0, 0);
    avatar_left_eye_ = mk_label(avatar_, "^"); lv_obj_set_pos(avatar_left_eye_, 15, 15);
    avatar_right_eye_ = mk_label(avatar_, "^"); lv_obj_set_pos(avatar_right_eye_, 42, 15);
    avatar_mouth_ = mk_label(avatar_, "u"); lv_obj_align(avatar_mouth_, LV_ALIGN_BOTTOM_MID, 0, -1);
    emoji_lbl = avatar_mouth_; // retained for compatibility with older call sites

    // === Row 3: ASR/TTS voice text bar (y=90..140, always visible with placeholder) ===
    asr_bar_ = lv_obj_create(mscr);
    lv_obj_set_size(asr_bar_, 226, 42);
    lv_obj_set_style_bg_color(asr_bar_, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_radius(asr_bar_, 8, 0);
    lv_obj_set_style_pad_all(asr_bar_, 5, 0);
    lv_obj_set_style_border_width(asr_bar_, 0, 0);
    lv_obj_align(asr_bar_, LV_ALIGN_TOP_MID, 0, 92);

    asr_label_ = lv_label_create(asr_bar_);
    ensure_cjk_font();
    lv_obj_set_style_text_font(asr_label_, &font_with_cjk, 0);
    lv_label_set_text(asr_label_, "按住麦克风开始说话...");
    lv_obj_set_style_text_color(asr_label_, lv_color_hex(0x888888), 0);
    lv_obj_set_width(asr_label_, 208);
    // Ordinary one-line replies marquee; structured multi-line replies become
    // a short static preview. Tapping opens the complete retained conversation.
    lv_obj_set_height(asr_label_, 18);
    lv_label_set_long_mode(asr_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(asr_label_, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_flag(asr_bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(asr_bar_, cb_open_text_detail, LV_EVENT_CLICKED, NULL);

    // === Row 4: DDL cards (y=144..236, 2 list-style items) ===
    // Card 1
    card1=lv_obj_create(mscr); lv_obj_set_size(card1,226,38);
    lv_obj_set_style_bg_color(card1,lv_color_hex(0x16213e),0);
    lv_obj_set_style_radius(card1,8,0); lv_obj_set_style_pad_all(card1,8,0);
    lv_obj_set_style_border_width(card1,0,0);
    lv_obj_align(card1,LV_ALIGN_TOP_MID,0,144);
    lv_obj_t* c1l = mk_label(card1,"暂无待办");
    lv_obj_set_style_text_color(c1l,lv_color_hex(0xcccccc),0);
    lv_obj_set_width(c1l, 205);
    lv_label_set_long_mode(c1l,LV_LABEL_LONG_DOT);
    lv_obj_add_flag(card1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card1, cb_card_click, LV_EVENT_CLICKED, NULL);

    // Card 2
    card2=lv_obj_create(mscr); lv_obj_set_size(card2,226,38);
    lv_obj_set_style_bg_color(card2,lv_color_hex(0x16213e),0);
    lv_obj_set_style_radius(card2,8,0); lv_obj_set_style_pad_all(card2,8,0);
    lv_obj_set_style_border_width(card2,0,0);
    lv_obj_align(card2,LV_ALIGN_TOP_MID,0,190);
    lv_obj_add_flag(card2,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* c2l = mk_label(card2,"");
    lv_obj_set_style_text_color(c2l,lv_color_hex(0xcccccc),0);
    lv_obj_set_width(c2l, 205);
    lv_label_set_long_mode(c2l,LV_LABEL_LONG_DOT);
    lv_obj_add_flag(card2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card2, cb_card_click, LV_EVENT_CLICKED, NULL);

    // "查看全部" link below cards
    lv_obj_t* va=mk_label(mscr,"查看全部 >");
    lv_obj_set_style_text_color(va,lv_color_hex(0x74b9ff),0);
    lv_obj_align(va,LV_ALIGN_TOP_MID,0,235);
    lv_obj_add_flag(va, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(va, cb_view_all, LV_EVENT_CLICKED, NULL);

    // === Row 5: Combined Tab bar + Mic button (y=248..310) ===
    // Tab buttons: Home, DDL, Set — each clickable, highlights on selection
    const char* tab_names[] = {"Home","DDL","Set"};
    for(int i = 0; i < 3; i++) {
        tab_btns_[i] = lv_btn_create(mscr);
        lv_obj_set_size(tab_btns_[i], 52, 36);
        lv_obj_set_style_shadow_width(tab_btns_[i], 0, 0);
        lv_obj_set_style_radius(tab_btns_[i], 10, 0);
        lv_obj_set_style_bg_color(tab_btns_[i], lv_color_hex(0x101010), 0);
        lv_obj_set_style_border_width(tab_btns_[i], 1, 0);
        lv_obj_set_style_border_color(tab_btns_[i], lv_color_hex(0x3a2f14), 0);
        lv_obj_align(tab_btns_[i], LV_ALIGN_BOTTOM_MID, -95 + i * 58, -16);

        tab_labels_[i] = lv_label_create(tab_btns_[i]);
        ensure_cjk_font();
        lv_obj_set_style_text_font(tab_labels_[i], &font_with_cjk, 0);
        lv_label_set_text(tab_labels_[i], tab_names[i]);
        lv_obj_set_style_text_color(tab_labels_[i], lv_color_hex(0x8c7a4f), 0);
        lv_obj_center(tab_labels_[i]);

        lv_obj_add_event_cb(tab_btns_[i], cb_tab_click, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }
    // Highlight current tab
    highlight_tab(TAB_HOME);

    // Large record button (round, red, 56×56)
    mic_btn_ = lv_btn_create(mscr);
    lv_obj_set_size(mic_btn_, 56, 56);
    lv_obj_set_style_shadow_width(mic_btn_, 0, 0);
    lv_obj_set_style_radius(mic_btn_, 28, 0);
    lv_obj_set_style_bg_color(mic_btn_, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_border_width(mic_btn_, 0, 0);
    lv_obj_align(mic_btn_, LV_ALIGN_BOTTOM_RIGHT, -16, -8);

    lv_obj_t* mic_lbl = lv_label_create(mic_btn_);
    ensure_cjk_font();
    lv_obj_set_style_text_font(mic_lbl, &font_with_cjk, 0);
    lv_label_set_text(mic_lbl, "MIC");
    lv_obj_set_style_text_color(mic_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(mic_lbl);

    lv_obj_add_event_cb(mic_btn_, cb_mic_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(mic_btn_, cb_mic_release, LV_EVENT_RELEASED, NULL);

    // Connection status dot (top-right)
    status_icon=mk_label(mscr,"O");
    lv_obj_set_style_text_color(status_icon,lv_color_hex(0xff4444),0);
    lv_obj_align(status_icon,LV_ALIGN_TOP_RIGHT,-8,8);
}

void UIManager::mk_list() {
    lscr=lv_obj_create(NULL); lv_obj_set_size(lscr,240,320); lv_obj_set_style_bg_color(lscr,lv_color_hex(0x1a1a2e),0);
    lv_obj_set_style_pad_all(lscr,0,0);

    // Back button
    lv_obj_t* bb=mk_btn(lscr,50,25,"<"); lv_obj_set_style_bg_color(bb,lv_color_hex(0x0f3460),0);
    lv_obj_set_style_text_color(lv_obj_get_child(bb,0),lv_color_hex(0xffffff),0);
    lv_obj_align(bb,LV_ALIGN_TOP_LEFT,5,5); lv_obj_add_event_cb(bb,cb_back,LV_EVENT_CLICKED,NULL);

    // Title
    lv_obj_t* ti=mk_label(lscr,"DDL \345\210\227\350\241\250"); lv_obj_set_style_text_color(ti,lv_color_hex(0xffffff),0);
    lv_obj_align(ti,LV_ALIGN_TOP_MID,0,8);

    // Event list. Keep a fixed pool of five rows: recreating one LVGL object tree
    // per synced DDL can occupy main long enough to trip ESP-IDF's task watchdog.
    elist=lv_obj_create(lscr); lv_obj_set_size(elist,225,232);
    lv_obj_align(elist,LV_ALIGN_TOP_MID,0,40);
    lv_obj_set_style_bg_color(elist,lv_color_hex(0x16213e),0);
    lv_obj_set_style_radius(elist,6,0);
    lv_obj_set_style_pad_all(elist,2,0);
    lv_obj_set_style_border_width(elist,0,0);
    lv_obj_clear_flag(elist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(elist, LV_SCROLLBAR_MODE_OFF);

    list_bucket_label_ = mk_label(elist, "暂无待办");
    lv_obj_set_width(list_bucket_label_, 195);
    lv_obj_set_style_text_color(list_bucket_label_, lv_color_hex(0x74b9ff), 0);
    lv_obj_set_pos(list_bucket_label_, 4, 0);

    constexpr int visible_rows = 5;
    for (int slot = 0; slot < visible_rows; ++slot) {
        lv_obj_t* button = lv_btn_create(elist);
        lv_obj_set_size(button, 198, 40);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_pos(button, 0, 20 + slot * 41);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x16213e), 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_radius(button, 4, 0);
        lv_obj_set_style_pad_all(button, 3, 0);
        lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* label = mk_label(button, "");
        lv_obj_set_width(label, 190);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(label, lv_color_hex(0xcccccc), 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 3, 0);

        lv_obj_t* due_label = mk_label(button, "");
        lv_obj_set_style_text_color(due_label, lv_color_hex(0x8f9bb3), 0);
        lv_obj_align(due_label, LV_ALIGN_BOTTOM_RIGHT, -3, 0);
        lv_obj_add_event_cb(button, cb_list_click, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
        list_items_.push_back(button);
    }

    // A touch-operable position bar replaces the native scrollbar. LVGL's
    // vertical slider grows bottom-to-top, so its value is reversed in callbacks.
    list_slider_ = lv_slider_create(elist);
    lv_obj_set_size(list_slider_, 12, 200);
    lv_obj_align(list_slider_, LV_ALIGN_RIGHT_MID, -1, 9);
    lv_slider_set_range(list_slider_, 0, 1);
    lv_obj_set_style_bg_color(list_slider_, lv_color_hex(0x34495e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(list_slider_, lv_color_hex(0x74b9ff), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(list_slider_, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_add_event_cb(list_slider_, cb_list_slider, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_flag(list_slider_, LV_OBJ_FLAG_HIDDEN);

    // Refresh button (top right)
    lv_obj_t* rb=mk_btn(lscr,60,25,"Refresh"); lv_obj_set_style_bg_color(rb,lv_color_hex(0x0f3460),0);
    lv_obj_set_style_text_color(lv_obj_get_child(rb,0),lv_color_hex(0xffffff),0);
    lv_obj_align(rb,LV_ALIGN_TOP_RIGHT,-5,5);
    lv_obj_add_event_cb(rb, [](lv_event_t*){ instance().request_ddl_refresh(); }, LV_EVENT_CLICKED, NULL);

    add_bottom_tabs(lscr, TAB_DDL);

    refresh_toast_ = lv_obj_create(lscr);
    lv_obj_set_size(refresh_toast_, 150, 46);
    lv_obj_center(refresh_toast_);
    lv_obj_set_style_pad_all(refresh_toast_, 5, 0);
    lv_obj_set_style_radius(refresh_toast_, 9, 0);
    lv_obj_set_style_border_width(refresh_toast_, 2, 0);
    lv_obj_set_style_border_color(refresh_toast_, lv_color_hex(0x74b9ff), 0);
    lv_obj_set_style_bg_color(refresh_toast_, lv_color_hex(0x16213e), 0);
    refresh_toast_label_ = mk_label(refresh_toast_, "正在刷新...");
    lv_obj_set_style_text_color(refresh_toast_label_, lv_color_hex(0xffffff), 0);
    lv_obj_center(refresh_toast_label_);
    lv_obj_add_flag(refresh_toast_, LV_OBJ_FLAG_HIDDEN);
    refresh_timer_ = lv_timer_create(cb_refresh_timer, 3000, this);
    lv_timer_pause(refresh_timer_);
}

void UIManager::mk_detail() {
    dscr=lv_obj_create(NULL); lv_obj_set_size(dscr,240,320); lv_obj_set_style_bg_color(dscr,lv_color_hex(0x1a1a2e),0);
    // Back button
    lv_obj_t* bb=mk_btn(dscr,50,25,"<"); lv_obj_set_style_bg_color(bb,lv_color_hex(0x0f3460),0); lv_obj_align(bb,LV_ALIGN_TOP_LEFT,5,5); lv_obj_add_event_cb(bb,cb_back,LV_EVENT_CLICKED,NULL);

    // Editable fields — each is a clickable label
    dtitle=mk_label(dscr,""); lv_obj_set_style_text_color(dtitle,lv_color_hex(0xffffff),0);
    lv_obj_set_width(dtitle, 165); lv_label_set_long_mode(dtitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(dtitle,LV_ALIGN_TOP_MID,20,8);
    lv_obj_add_flag(dtitle, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(dtitle, cb_detail_edit, LV_EVENT_CLICKED, (void*)"title");

    dcourse=mk_label(dscr,""); lv_obj_set_style_text_color(dcourse,lv_color_hex(0x888888),0);
    lv_obj_set_width(dcourse, 210); lv_label_set_long_mode(dcourse, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(dcourse,LV_ALIGN_TOP_MID,0,35);
    lv_obj_add_flag(dcourse, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(dcourse, cb_detail_edit, LV_EVENT_CLICKED, (void*)"course");

    ddeadline=mk_label(dscr,""); lv_obj_set_style_text_color(ddeadline,lv_color_hex(0xe94560),0); lv_obj_align(ddeadline,LV_ALIGN_TOP_MID,0,60);
    lv_obj_add_flag(ddeadline, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(ddeadline, cb_detail_edit, LV_EVENT_CLICKED, (void*)"deadline");

    dcountdown=mk_label(dscr,""); lv_obj_set_style_text_color(dcountdown,lv_color_hex(0xffd93d),0); lv_obj_align(dcountdown,LV_ALIGN_TOP_MID,0,90);

    dreminder=mk_label(dscr,""); lv_obj_set_style_text_color(dreminder,lv_color_hex(0x74b9ff),0);
    lv_obj_set_width(dreminder, 218); lv_label_set_long_mode(dreminder, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(dreminder,LV_ALIGN_TOP_MID,0,120);
    lv_obj_add_flag(dreminder, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(dreminder, cb_detail_edit, LV_EVENT_CLICKED, (void*)"advance");

    dstatus=mk_label(dscr,""); lv_obj_set_style_text_color(dstatus,lv_color_hex(0x00b894),0); lv_obj_align(dstatus,LV_ALIGN_TOP_MID,0,145);

    // Hint text (updated for touch editing)
    lv_obj_t* hint = mk_label(dscr,"Tap field to edit | Rot:prev/next");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_obj_t* db=mk_btn(dscr,90,35,"Done"); lv_obj_set_style_bg_color(db,lv_color_hex(0x00b894),0); lv_obj_set_style_radius(db,17,0); lv_obj_align(db,LV_ALIGN_BOTTOM_LEFT,15,-45); lv_obj_add_event_cb(db,cb_done,LV_EVENT_CLICKED,NULL);
    lv_obj_t* sb=mk_btn(dscr,90,35,"Later"); lv_obj_set_style_bg_color(sb,lv_color_hex(0xfdcb6e),0); lv_obj_set_style_radius(sb,17,0); lv_obj_set_style_text_color(sb,lv_color_hex(0x000000),0); lv_obj_align(sb,LV_ALIGN_BOTTOM_RIGHT,-15,-45); lv_obj_add_event_cb(sb,cb_snooze,LV_EVENT_CLICKED,NULL);
}

void UIManager::mk_reminder_editor() {
    reminderscr = lv_obj_create(NULL);
    lv_obj_set_size(reminderscr, 240, 320);
    lv_obj_set_style_bg_color(reminderscr, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_pad_all(reminderscr, 0, 0);

    lv_obj_t* back = mk_btn(reminderscr, 44, 25, "<");
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0f3460), 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_event_cb(back, cb_reminder_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = mk_label(reminderscr, "提醒时间");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* add = mk_btn(reminderscr, 62, 28, "+ 新增");
    lv_obj_set_style_bg_color(add, lv_color_hex(0x00b894), 0);
    lv_obj_align(add, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_add_event_cb(add, cb_reminder_add, LV_EVENT_CLICKED, nullptr);

    reminder_list_ = lv_obj_create(reminderscr);
    lv_obj_set_size(reminder_list_, 230, 228);
    lv_obj_align(reminder_list_, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(reminder_list_, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(reminder_list_, 0, 0);
    lv_obj_set_style_radius(reminder_list_, 7, 0);
    lv_obj_set_style_pad_all(reminder_list_, 3, 0);
    lv_obj_set_scroll_dir(reminder_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(reminder_list_, LV_SCROLLBAR_MODE_AUTO);

    reminder_hint_ = mk_label(reminderscr, "旋转选择，按下编辑 天/时/分（最多6条）");
    lv_obj_set_width(reminder_hint_, 230);
    lv_obj_set_style_text_align(reminder_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(reminder_hint_, lv_color_hex(0x8f9bb3), 0);
    lv_obj_align(reminder_hint_, LV_ALIGN_BOTTOM_MID, 0, -25);
}

void UIManager::request_ddl_refresh() {
    ESP_LOGI(T, "DDL refresh requested from UI");
    refresh_pending_ = true;
    if (scr_ == LIST && refresh_toast_ && refresh_toast_label_) {
        lv_label_set_text(refresh_toast_label_, "正在刷新...");
        lv_obj_set_style_border_color(refresh_toast_, lv_color_hex(0x74b9ff), 0);
        lv_obj_clear_flag(refresh_toast_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(refresh_toast_);
    }
    if (refresh_timer_) {
        lv_timer_set_period(refresh_timer_, 3000);
        lv_timer_reset(refresh_timer_);
        lv_timer_resume(refresh_timer_);
    }
    if (g_on_request_sync) g_on_request_sync();
    else notify_ddl_sync(false);
}

void UIManager::notify_ddl_sync(bool success) {
    if (!refresh_pending_) return;
    ESP_LOGI(T, "DDL refresh feedback: %s", success ? "success" : "failure");
    refresh_pending_ = false;
    if (scr_ == LIST && refresh_toast_ && refresh_toast_label_) {
        lv_label_set_text(refresh_toast_label_, success ? "刷新成功" : "刷新失败");
        lv_obj_set_style_border_color(refresh_toast_,
            lv_color_hex(success ? 0x00b894 : 0xe94560), 0);
        lv_obj_clear_flag(refresh_toast_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(refresh_toast_);
    }
    if (refresh_timer_) {
        lv_timer_set_period(refresh_timer_, success ? 1500 : 2200);
        lv_timer_reset(refresh_timer_);
        lv_timer_resume(refresh_timer_);
    }
}

void UIManager::cb_refresh_timer(lv_timer_t* timer) {
    auto* u = static_cast<UIManager*>(timer->user_data);
    if (!u) return;
    if (u->refresh_pending_) {
        ESP_LOGW(T, "DDL refresh timeout: no SYNC was applied within 3000ms");
        u->refresh_pending_ = false;
        if (u->scr_ == LIST && u->refresh_toast_ && u->refresh_toast_label_) {
            lv_label_set_text(u->refresh_toast_label_, "刷新失败");
            lv_obj_set_style_border_color(u->refresh_toast_, lv_color_hex(0xe94560), 0);
            lv_timer_set_period(timer, 1800);
            lv_timer_reset(timer);
            return;
        }
    }
    if (u->refresh_toast_) lv_obj_add_flag(u->refresh_toast_, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

void UIManager::mk_text_detail() {
    textscr = lv_obj_create(NULL);
    lv_obj_set_size(textscr, 240, 320);
    lv_obj_set_style_bg_color(textscr, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_pad_all(textscr, 0, 0);

    lv_obj_t* back = mk_btn(textscr, 44, 25, "<");
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0f3460), 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_event_cb(back, [](lv_event_t*) { instance().show_screen(MAIN); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* title = mk_label(textscr, "对话详情");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* body = lv_obj_create(textscr);
    lv_obj_set_size(body, 230, 270);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 8, 0);
    lv_obj_set_style_pad_all(body, 7, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    textdetail_label_ = mk_label(body, "暂无对话");
    lv_obj_set_width(textdetail_label_, 210);
    lv_label_set_long_mode(textdetail_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(textdetail_label_, lv_color_hex(0xffffff), 0);
    lv_obj_align(textdetail_label_, LV_ALIGN_TOP_LEFT, 0, 0);
}

void UIManager::mk_popup() {
    popup=lv_obj_create(lv_scr_act()); lv_obj_set_size(popup,220,220);
    lv_obj_set_style_bg_color(popup,lv_color_hex(0x2d2d44),0); lv_obj_set_style_border_color(popup,lv_color_hex(0xe94560),0); lv_obj_set_style_border_width(popup,3,0); lv_obj_set_style_radius(popup,12,0); lv_obj_center(popup); lv_obj_add_flag(popup,LV_OBJ_FLAG_HIDDEN);
    mk_label(popup,LV_SYMBOL_BELL " DDL 提醒"); lv_obj_align(lv_obj_get_child(popup,-1),LV_ALIGN_TOP_MID,0,10);
    rtitle=mk_label(popup,"DDL \346\217\220\351\206\222"); lv_obj_align(rtitle,LV_ALIGN_TOP_MID,0,50);
    rdeadline=mk_label(popup,""); lv_obj_set_style_text_color(rdeadline,lv_color_hex(0xe94560),0); lv_obj_align(rdeadline,LV_ALIGN_TOP_MID,0,75);
    lv_obj_t* cf=mk_btn(popup,100,35,"\347\237\245\351\201\223\344\272\206"); lv_obj_set_style_bg_color(cf,lv_color_hex(0x00b894),0); lv_obj_set_style_radius(cf,17,0); lv_obj_align(cf,LV_ALIGN_BOTTOM_LEFT,8,-12); lv_obj_add_event_cb(cf,[](lv_event_t*){UIManager::instance().show_screen(MAIN);},LV_EVENT_CLICKED,NULL);
    lv_obj_t* sz=mk_btn(popup,100,35,"5\345\210\206\351\222\237\345\220\216"); lv_obj_set_style_bg_color(sz,lv_color_hex(0xfdcb6e),0); lv_obj_set_style_radius(sz,17,0); lv_obj_align(sz,LV_ALIGN_BOTTOM_RIGHT,-8,-12); lv_obj_add_event_cb(sz,[](lv_event_t*){if(g_on_event_action)g_on_event_action("snooze");UIManager::instance().show_screen(MAIN);},LV_EVENT_CLICKED,NULL);
}

// ── Touch keyboard (simplified number pad) ──────────────────

static const char* KB_KEYS[12] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "-", "0", "Del"
};

void UIManager::mk_keyboard() {
    kbscr = lv_obj_create(NULL);
    lv_obj_set_size(kbscr, 240, 320);
    lv_obj_set_style_bg_color(kbscr, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_pad_all(kbscr, 0, 0);
    lv_obj_add_flag(kbscr, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* t = mk_label(kbscr, "Edit");
    lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 2);

    kb_field_lbl = mk_label(kbscr, "");
    lv_obj_set_style_text_color(kb_field_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(kb_field_lbl, LV_ALIGN_TOP_MID, 0, 22);

    kb_text = lv_textarea_create(kbscr);
    lv_obj_set_size(kb_text, 220, 40);
    lv_obj_set_style_bg_color(kb_text, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_text_color(kb_text, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(kb_text, 0, 0);
    lv_obj_set_style_radius(kb_text, 6, 0);
    lv_obj_align(kb_text, LV_ALIGN_TOP_MID, 0, 46);
    lv_textarea_set_cursor_click_pos(kb_text, true);
    lv_obj_set_style_text_font(kb_text, LV_FONT_DEFAULT, 0);

    // Key grid: 3x4, compact
    int KW = 68, KH = 38, G = 4;
    int X0 = 18, Y0 = 98;
    for (int i = 0; i < 12; i++) {
        lv_obj_t* btn = lv_btn_create(kbscr);
        lv_obj_set_size(btn, KW, KH);
        lv_obj_set_style_radius(btn, 5, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        int col = i % 3, row = i / 3;
        lv_obj_set_pos(btn, X0 + col * (KW + G), Y0 + row * (KH + G));
        lv_obj_set_style_bg_color(btn, i == 11 ? lv_color_hex(0x4a1a1a) : lv_color_hex(0x16213e), 0);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, KB_KEYS[i]);
        lv_obj_set_style_text_color(lbl, i == 11 ? lv_color_hex(0xff6b6b) : lv_color_hex(0xffffff), 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, cb_kb_key, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }

    // OK + Cancel below keys (Y0 + 4*(38+4) = 98+168 = 266)
    int by = 270;
    lv_obj_t* save = lv_btn_create(kbscr);
    lv_obj_set_size(save, 100, 34);
    lv_obj_set_style_radius(save, 8, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x00b894), 0);
    lv_obj_set_pos(save, X0, by);
    lv_obj_t* sl = lv_label_create(save);
    lv_label_set_text(sl, "OK"); lv_obj_set_style_text_color(sl, lv_color_hex(0xffffff), 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(save, cb_kb_save, LV_EVENT_CLICKED, NULL);

    lv_obj_t* cancel = lv_btn_create(kbscr);
    lv_obj_set_size(cancel, 100, 34);
    lv_obj_set_style_radius(cancel, 8, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x555555), 0);
    lv_obj_set_pos(cancel, X0 + 108, by);
    lv_obj_t* cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel"); lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, [](lv_event_t*){ instance().hide_keyboard(); }, LV_EVENT_CLICKED, NULL);
}

struct KbOpenData {
    std::string field;
    std::string value;
};

static void ui_show_keyboard_cb(void* p) {
    auto* d = (KbOpenData*)p;
    delete d;  // keyboard disabled — clean up and return
    // When keyboard is re-enabled, uncomment below:
    // auto& u = UIManager::instance();
    // if (!u.kb_field_lbl || !u.kb_text) return;
    // u.kb_field_ = d->field;
    // ...
}

void UIManager::show_keyboard(const std::string& field_name, const std::string& current_value) {
    auto* d = new KbOpenData{field_name, current_value};
    if (lvgl_port_lock(2000)) {  // LVGL is not thread-safe: lock before async enqueue
        lv_res_t result = lv_async_call(ui_show_keyboard_cb, d);
        lvgl_port_unlock();
        if (result != LV_RES_OK) delete d;
    } else {
        delete d;
    }
}

void UIManager::hide_keyboard() { show_screen(DETAIL); }

std::string UIManager::get_keyboard_text() const {
    if (!kb_text) return "";
    return lv_textarea_get_text(kb_text);
}

void UIManager::cb_kb_key(lv_event_t* e) {
    auto& u = instance();
    int key = (int)(uintptr_t)lv_event_get_user_data(e);
    const char* cur = lv_textarea_get_text(u.kb_text);
    std::string text = cur ? cur : "";
    // KB_KEYS indices: 0-8=1..9, 9='-', 10='0', 11=Del
    if (key == 11) { if (!text.empty()) text.pop_back(); }
    else if (key == 9) { text += '-'; }
    else if (key == 10) { text += '0'; }
    else if (key <= 8) { text += '1' + key; }
    lv_textarea_set_text(u.kb_text, text.c_str());
    lv_textarea_set_cursor_pos(u.kb_text, LV_TEXTAREA_CURSOR_LAST);
}

void UIManager::cb_kb_toggle(lv_event_t*) {}

void UIManager::cb_kb_save(lv_event_t*) {
    auto& u = instance();
    std::string text = u.get_keyboard_text();
    if (text.empty()) return;
    extern std::function<void(const std::string&)> g_on_event_action;
    if (g_on_event_action) g_on_event_action("edit:" + u.kb_field_ + "=" + text);
    u.hide_keyboard();
}

void UIManager::cb_detail_edit(lv_event_t* e) {
    auto& u = instance();
    const char* field = (const char*)lv_event_get_user_data(e);
    if (!field || u.detail_id_.empty()) return;

    if (strcmp(field, "deadline") == 0) {
        if (u.digit_edit_mode_ == EDIT_DEADLINE) u.commit_digit_edit();
        else {
            std::string value;
            for (const auto& event : events) if (event.id == u.detail_id_) { value = event.deadline.substr(0, 16); break; }
            if (!value.empty()) u.begin_digit_edit(EDIT_DEADLINE, value);
        }
        return;
    }
    if (strcmp(field, "advance") == 0) {
        u.open_reminder_editor();
        return;
    }

    static char adv_buf[16];
    const char* cur = "";
    if (strcmp(field, "title") == 0) cur = lv_label_get_text(u.dtitle);
    else if (strcmp(field, "course") == 0) cur = lv_label_get_text(u.dcourse);
    else if (strcmp(field, "deadline") == 0) {
        const char* due = lv_label_get_text(u.ddeadline);
        if (due && strlen(due) > 5) cur = due + 5;
        else cur = due;
    }
    else if (strcmp(field, "advance") == 0) {
        snprintf(adv_buf, sizeof(adv_buf), "%d", u.d_advance_mins);
        cur = adv_buf;
    }

    u.show_keyboard(field, cur);
}

void UIManager::mk_tool_toast() {
    tool_toast_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(tool_toast_, 218, 108);
    lv_obj_center(tool_toast_);
    lv_obj_set_style_bg_color(tool_toast_, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(tool_toast_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tool_toast_, 2, 0);
    lv_obj_set_style_border_color(tool_toast_, lv_color_hex(0xd4af37), 0);
    lv_obj_set_style_radius(tool_toast_, 10, 0);
    lv_obj_set_style_shadow_width(tool_toast_, 0, 0);
    lv_obj_set_style_pad_all(tool_toast_, 8, 0);

    tool_toast_title_ = mk_label(tool_toast_, "DDL操作");
    lv_obj_set_width(tool_toast_title_, 198);
    lv_obj_set_style_text_color(tool_toast_title_, lv_color_hex(0xf4cf57), 0);
    lv_obj_align(tool_toast_title_, LV_ALIGN_TOP_LEFT, 0, 0);

    tool_toast_message_ = mk_label(tool_toast_, "");
    lv_obj_set_width(tool_toast_message_, 198);
    lv_obj_set_height(tool_toast_message_, 64);
    lv_label_set_long_mode(tool_toast_message_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(tool_toast_message_, lv_color_hex(0xffffff), 0);
    lv_obj_align(tool_toast_message_, LV_ALIGN_TOP_LEFT, 0, 26);

    lv_obj_add_flag(tool_toast_, LV_OBJ_FLAG_HIDDEN);
    tool_toast_timer_ = lv_timer_create(cb_tool_toast_timer, 3500, this);
    lv_timer_pause(tool_toast_timer_);
}

void UIManager::show_tool_result(const std::string& tool, bool success, const std::string& message) {
    if (!tool_toast_ || !tool_toast_title_ || !tool_toast_message_) return;
    const char* action = "DDL操作";
    if (tool == "add_reminder") action = "新增DDL";
    else if (tool == "modify_reminder") action = "修改DDL";
    else if (tool == "delete_reminder") action = "删除DDL";
    else if (tool == "mark_done") action = "完成DDL";

    std::string title = std::string(action) + (success ? "成功" : "失败");
    std::string body = display_safe_text(message);
    constexpr size_t MAX_TOOL_MESSAGE_BYTES = 300;
    if (body.size() > MAX_TOOL_MESSAGE_BYTES) {
        body.resize(MAX_TOOL_MESSAGE_BYTES);
        while (!body.empty() && ((uint8_t)body.back() & 0xC0) == 0x80) body.pop_back();
        body += "……";
    }
    lv_label_set_text(tool_toast_title_, title.c_str());
    lv_label_set_text(tool_toast_message_, body.c_str());
    lv_obj_set_style_border_color(tool_toast_,
        lv_color_hex(success ? 0xd4af37 : 0xe94560), 0);
    lv_obj_set_style_text_color(tool_toast_title_,
        lv_color_hex(success ? 0xf4cf57 : 0xff6b81), 0);
    lv_obj_clear_flag(tool_toast_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(tool_toast_);
    if (tool_toast_timer_) {
        lv_timer_set_period(tool_toast_timer_, 3500);
        lv_timer_reset(tool_toast_timer_);
        lv_timer_resume(tool_toast_timer_);
    }
}

void UIManager::cb_tool_toast_timer(lv_timer_t* timer) {
    auto* manager = static_cast<UIManager*>(timer->user_data);
    if (manager && manager->tool_toast_) {
        lv_obj_add_flag(manager->tool_toast_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_pause(timer);
}

void UIManager::open_reminder_editor() {
    reminder_values_.clear();
    for (const auto& event : events) {
        if (event.id != detail_id_) continue;
        reminder_values_ = event.reminder_minutes;
        break;
    }
    for (int& value : reminder_values_) value = std::min(44639, std::max(0, value));
    std::sort(reminder_values_.begin(), reminder_values_.end(), std::greater<int>());
    reminder_values_.erase(std::unique(reminder_values_.begin(), reminder_values_.end()), reminder_values_.end());
    if (reminder_values_.size() > 6) reminder_values_.resize(6);
    reminder_idx_ = 0;
    reminder_field_ = 0;
    reminder_editing_ = false;
    show_screen(REMINDER_EDITOR);
}

static std::string format_reminder_offset(int total) {
    total = std::min(44639, std::max(0, total));
    int days = total / 1440;
    int hours = (total % 1440) / 60;
    int minutes = total % 60;
    std::string value;
    if (days) value += std::to_string(days) + "天";
    if (hours) value += std::to_string(hours) + "小时";
    if (minutes || value.empty()) value += std::to_string(minutes) + "分钟";
    return value;
}

void UIManager::refresh_reminder_editor() {
    if (!reminder_list_) return;
    lv_obj_clean(reminder_list_);
    if (reminder_values_.empty()) {
        lv_obj_t* empty = mk_label(reminder_list_, "暂无提醒，点击右上角新增");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8f9bb3), 0);
        lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 15);
    }

    for (size_t i = 0; i < reminder_values_.size(); ++i) {
        lv_obj_t* row = lv_obj_create(reminder_list_);
        lv_obj_set_size(row, 218, 42);
        lv_obj_set_pos(row, 1, 1 + (int)i * 44);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(i == (size_t)reminder_idx_ ? 0x284f7a : 0x1a1a2e), 0);
        lv_obj_set_style_border_width(row, i == (size_t)reminder_idx_ ? 2 : 0, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(reminder_editing_ && i == (size_t)reminder_idx_ ? 0xffd93d : 0x74b9ff), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        int total = reminder_values_[i];
        int days = total / 1440;
        int hours = (total % 1440) / 60;
        int minutes = total % 60;
        char value[64];
        if (reminder_editing_ && i == (size_t)reminder_idx_) {
            if (reminder_field_ == 0) snprintf(value, sizeof(value), "[%02d]天 %02d时 %02d分", days, hours, minutes);
            else if (reminder_field_ == 1) snprintf(value, sizeof(value), "%02d天 [%02d]时 %02d分", days, hours, minutes);
            else snprintf(value, sizeof(value), "%02d天 %02d时 [%02d]分", days, hours, minutes);
        } else {
            snprintf(value, sizeof(value), "%s", format_reminder_offset(total).c_str());
        }
        lv_obj_t* time_button = mk_btn(row, 140, 34, value);
        lv_obj_set_style_bg_color(time_button, lv_color_hex(i == (size_t)reminder_idx_ ? 0x245f91 : 0x23304d), 0);
        lv_obj_set_style_radius(time_button, 6, 0);
        lv_obj_align(time_button, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t* label = lv_obj_get_child(time_button, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(reminder_editing_ && i == (size_t)reminder_idx_ ? 0xffd93d : 0xffffff), 0);
        lv_obj_add_event_cb(time_button, cb_reminder_edit, LV_EVENT_CLICKED,
                            (void*)(uintptr_t)(i + 1));

        lv_obj_t* del = mk_btn(row, 68, 34, "- 删除");
        lv_obj_set_style_bg_color(del, lv_color_hex(0xe94560), 0);
        lv_obj_set_style_radius(del, 6, 0);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(del, cb_reminder_delete, LV_EVENT_CLICKED,
                            (void*)(uintptr_t)(i + 1));
    }

    if (reminder_hint_) {
        lv_label_set_text(reminder_hint_, reminder_editing_ ?
            "旋转修改，按下切换 天→时→分→保存" :
            "旋转选择，按下编辑 天/时/分（最多6条）");
    }
}

void UIManager::adjust_reminder_field(int direction) {
    if (!reminder_editing_ || reminder_idx_ < 0 || reminder_idx_ >= (int)reminder_values_.size()) return;
    int total = reminder_values_[reminder_idx_];
    int days = total / 1440;
    int hours = (total % 1440) / 60;
    int minutes = total % 60;
    int step = direction >= 0 ? 1 : -1;
    if (reminder_field_ == 0) days = (days + step + 31) % 31;
    else if (reminder_field_ == 1) hours = (hours + step + 24) % 24;
    else minutes = (minutes + step + 60) % 60;
    reminder_values_[reminder_idx_] = days * 1440 + hours * 60 + minutes;
    refresh_reminder_editor();
}

void UIManager::commit_reminders(bool rebuild) {
    std::sort(reminder_values_.begin(), reminder_values_.end(), std::greater<int>());
    reminder_values_.erase(std::unique(reminder_values_.begin(), reminder_values_.end()), reminder_values_.end());
    if (reminder_values_.size() > 6) reminder_values_.resize(6);
    if (reminder_idx_ >= (int)reminder_values_.size()) reminder_idx_ = std::max(0, (int)reminder_values_.size() - 1);
    reminder_editing_ = false;
    reminder_field_ = 0;

    std::string csv;
    for (size_t i = 0; i < reminder_values_.size(); ++i) {
        if (i) csv += ',';
        csv += std::to_string(reminder_values_[i]);
    }
    if (g_on_event_action) g_on_event_action("edit:reminders=" + csv);

    std::string summary = "提醒: ";
    if (reminder_values_.empty()) summary += "关闭";
    for (size_t i = 0; i < reminder_values_.size(); ++i) {
        if (i) summary += '/';
        summary += format_reminder_offset(reminder_values_[i]);
    }
    if (dreminder) lv_label_set_text(dreminder, summary.c_str());
    if (rebuild) refresh_reminder_editor();
}

void UIManager::cb_reminder_add(lv_event_t*) {
    auto& u = instance();
    if (u.reminder_values_.size() >= 6) {
        if (u.reminder_hint_) lv_label_set_text(u.reminder_hint_, "最多只能设置6条提醒");
        return;
    }
    static const int defaults[] = {10, 30, 60, 180, 1440, 10080};
    int value = 0;
    for (int candidate : defaults) {
        if (std::find(u.reminder_values_.begin(), u.reminder_values_.end(), candidate) == u.reminder_values_.end()) {
            value = candidate;
            break;
        }
    }
    u.reminder_values_.push_back(value);
    u.reminder_idx_ = (int)u.reminder_values_.size() - 1;
    u.reminder_field_ = 0;
    u.reminder_editing_ = true;
    u.refresh_reminder_editor();
}

void UIManager::cb_reminder_edit(lv_event_t* e) {
    auto& u = instance();
    int idx = (int)(uintptr_t)lv_event_get_user_data(e) - 1;
    if (idx < 0 || idx >= (int)u.reminder_values_.size()) return;
    u.reminder_idx_ = idx;
    u.reminder_editing_ = true;
    u.reminder_field_ = 0;
    lv_async_call(async_refresh_reminder_editor, nullptr);
}

void UIManager::cb_reminder_delete(lv_event_t* e) {
    auto& u = instance();
    int idx = (int)(uintptr_t)lv_event_get_user_data(e) - 1;
    if (idx < 0 || idx >= (int)u.reminder_values_.size()) return;
    u.reminder_values_.erase(u.reminder_values_.begin() + idx);
    u.reminder_idx_ = std::min(idx, std::max(0, (int)u.reminder_values_.size() - 1));
    u.commit_reminders(false);
    // The tapped delete button belongs to reminder_list_. Rebuild only after
    // LVGL finishes dispatching this event so we never delete the active target.
    lv_async_call(async_refresh_reminder_editor, nullptr);
}

void UIManager::async_refresh_reminder_editor(void*) {
    instance().refresh_reminder_editor();
}

void UIManager::cb_reminder_back(lv_event_t*) {
    auto& u = instance();
    if (u.reminder_editing_) u.commit_reminders();
    u.show_screen(DETAIL);
}

void UIManager::cb_open_text_detail(lv_event_t*) {
    auto& u = instance();
    if (u.textdetail_label_) {
        lv_label_set_text(u.textdetail_label_, u.asr_full_text_.empty() ? "暂无对话" : u.asr_full_text_.c_str());
    }
    u.show_screen(TEXT_DETAIL);
    if (u.textdetail_label_) {
        lv_obj_t* body = lv_obj_get_parent(u.textdetail_label_);
        lv_obj_update_layout(body);
        lv_obj_scroll_to_y(body,
            lv_obj_get_scroll_y(body) + lv_obj_get_scroll_bottom(body), LV_ANIM_OFF);
    }
}

bool UIManager::is_editing_detail() const { return digit_edit_mode_ != EDIT_NONE; }

static bool valid_deadline_digits(const std::string& value) {
    int year, month, day, hour, minute;
    if (sscanf(value.c_str(), "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) != 5) return false;
    if (year < 2020 || year > 2099 || month < 1 || month > 12 || hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int max_day = month_days[month - 1];
    if (month == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0))) max_day = 29;
    return day >= 1 && day <= max_day;
}

void UIManager::begin_digit_edit(DigitEditMode mode, const std::string& value) {
    digit_edit_mode_ = mode;
    digit_edit_value_ = value;
    digit_positions_.clear();
    for (size_t i = 0; i < digit_edit_value_.size(); ++i) {
        if (digit_edit_value_[i] >= '0' && digit_edit_value_[i] <= '9') digit_positions_.push_back(i);
    }
    digit_cursor_ = 0;
    render_digit_edit();
}

void UIManager::render_digit_edit() {
    if (digit_edit_mode_ == EDIT_NONE || digit_positions_.empty()) return;
    size_t selected = digit_positions_[digit_cursor_];
    std::string shown = digit_edit_value_.substr(0, selected) + "[" + digit_edit_value_[selected] + "]" +
                        digit_edit_value_.substr(selected + 1);
    if (digit_edit_mode_ == EDIT_DEADLINE) {
        lv_label_set_text(ddeadline, ("截止: " + shown).c_str());
        lv_obj_set_style_text_color(ddeadline, lv_color_hex(0xffd93d), 0);
    } else {
        lv_label_set_text(dreminder, ("提前: " + shown + " 分钟").c_str());
        lv_obj_set_style_text_color(dreminder, lv_color_hex(0xffd93d), 0);
    }
}

void UIManager::rotate_digit(int direction) {
    if (digit_edit_mode_ == EDIT_NONE || digit_positions_.empty()) return;
    size_t position = digit_positions_[digit_cursor_];
    std::string original = digit_edit_value_;
    for (int attempt = 0; attempt < 10; ++attempt) {
        int digit = digit_edit_value_[position] - '0';
        digit = (digit + (direction >= 0 ? 1 : 9)) % 10;
        digit_edit_value_[position] = static_cast<char>('0' + digit);
        if (digit_edit_mode_ != EDIT_DEADLINE || valid_deadline_digits(digit_edit_value_)) break;
    }
    if (digit_edit_mode_ == EDIT_DEADLINE && !valid_deadline_digits(digit_edit_value_)) digit_edit_value_ = original;
    render_digit_edit();
}

void UIManager::advance_or_commit_digit_edit() {
    if (digit_edit_mode_ == EDIT_NONE) return;
    if (++digit_cursor_ >= digit_positions_.size()) commit_digit_edit();
    else render_digit_edit();
}

void UIManager::commit_digit_edit() {
    if (digit_edit_mode_ == EDIT_NONE) return;
    if (g_on_event_action) {
        if (digit_edit_mode_ == EDIT_DEADLINE && valid_deadline_digits(digit_edit_value_)) {
            g_on_event_action("edit:deadline=" + digit_edit_value_);
            lv_label_set_text(ddeadline, ("Due: " + digit_edit_value_).c_str());
        } else if (digit_edit_mode_ == EDIT_REMINDER) {
            int minutes = atoi(digit_edit_value_.c_str());
            d_advance_mins = minutes;
            g_on_event_action("edit:advance=" + std::to_string(minutes));
            lv_label_set_text(dreminder, ("Remind: " + std::to_string(minutes) + "m before").c_str());
        }
    }
    digit_edit_mode_ = EDIT_NONE;
    digit_edit_value_.clear();
    digit_positions_.clear();
}

void UIManager::mk_settings() {
    setscr=lv_obj_create(NULL); lv_obj_set_size(setscr,240,320); lv_obj_set_style_bg_color(setscr,lv_color_hex(0x1a1a2e),0);
    lv_obj_set_style_pad_all(setscr,0,0);

    lv_obj_t* bb=mk_btn(setscr,50,25,"<"); lv_obj_set_style_bg_color(bb,lv_color_hex(0x0f3460),0);
    lv_obj_set_style_text_color(lv_obj_get_child(bb,0),lv_color_hex(0xffffff),0);
    lv_obj_align(bb,LV_ALIGN_TOP_LEFT,5,5); lv_obj_add_event_cb(bb,cb_back,LV_EVENT_CLICKED,NULL);

    lv_obj_t* ti=mk_label(setscr,"\350\256\276\347\275\256"); lv_obj_set_style_text_color(ti,lv_color_hex(0xffffff),0);
    lv_obj_align(ti,LV_ALIGN_TOP_MID,0,8);

    // Settings list (scrollable container)
    setlist=lv_obj_create(setscr); lv_obj_set_size(setlist,225,232);
    lv_obj_set_flex_flow(setlist, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(setlist,LV_ALIGN_TOP_MID,0,40);
    lv_obj_set_style_bg_color(setlist,lv_color_hex(0x16213e),0);
    lv_obj_set_style_radius(setlist,6,0);
    lv_obj_set_style_pad_all(setlist,4,0);
    lv_obj_set_style_border_width(setlist,0,0);

    // Populate settings items (manual buttons)
    auto add_set_btn = [](lv_obj_t* parent, const char* txt, int idx){
        lv_obj_t* b = lv_btn_create(parent);
        lv_obj_set_size(b, 210, 36);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x1a1a2e), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_t* l = lv_label_create(b);
        ensure_cjk_font(); lv_obj_set_style_text_font(l, &font_with_cjk, 0);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(0xcccccc), 0);
        lv_obj_center(l);
        if(idx >= 0) lv_obj_add_event_cb(b, cb_set_click, LV_EVENT_CLICKED, (void*)(uintptr_t)idx);
        return b;
    };
    add_set_btn(setlist, "Volume: 8/10", 0);
    add_set_btn(setlist, "WiFi: LC235", -1);
    add_set_btn(setlist, "Server: ws://IP:8888", -1);
    add_set_btn(setlist, "About: DDL v1.0.0", -1);

    set_idx = 0; set_editing = false;
    add_bottom_tabs(setscr, TAB_SETTINGS);
}

// Settings item click: start editing or toggle value
void UIManager::cb_set_click(lv_event_t* e) {
    auto& u = instance();
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    u.set_idx = idx;

    if (idx == 0) { // Volume: enter edit mode
        u.set_editing = !u.set_editing;
        u.refresh_settings();
    }
}
void UIManager::refresh_settings() {
    if (!setlist) return;
    int count = lv_obj_get_child_cnt(setlist);
    for (int i = 0; i < count; ++i) {
        lv_obj_t* btn = lv_obj_get_child(setlist, i);
        lv_obj_t* lbl = btn ? lv_obj_get_child(btn, 0) : nullptr;
        if (!btn || !lbl) continue;
        bool selected = i == set_idx;
        lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? 0x284f7a : 0x1a1a2e), 0);
        lv_obj_set_style_border_width(btn, selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(set_editing && i == 0 ? 0xffd93d : 0x74b9ff), 0);
        if (i == 0) {
            char text[40];
            snprintf(text, sizeof(text), set_editing ? "音量: [%d/10] 旋转调节" : "音量: %d/10", volume_);
            lv_label_set_text(lbl, text);
            lv_obj_set_style_text_color(lbl, lv_color_hex(set_editing ? 0xffd93d : 0xffffff), 0);
        }
    }
}

// public methods
void UIManager::show_reminder(const DDLEvent& e) { lv_label_set_text(rtitle,display_safe_text(UIManager::tag_text(e.tag)+e.title).c_str()); lv_label_set_text(rdeadline,("\346\210\252\346\255\242: "+e.deadline).c_str()); lv_obj_clear_flag(popup,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(popup); scr_=REMINDER_POPUP; }
// Map emoji tags to ASCII text (emoji glyphs not in our font)
std::string UIManager::tag_text(const std::string& emoji_tag) {
    if(emoji_tag.find("\360\237\224\245")!=std::string::npos) return "[紧急] ";
    if(emoji_tag.find("\342\232\241")!=std::string::npos) return "[急] ";
    if(emoji_tag.find("\360\237\223\214")!=std::string::npos) return "[近期] ";
    if(emoji_tag.find("\342\234\205")!=std::string::npos) return std::string(LV_SYMBOL_OK) + " ";
    if(emoji_tag.find("\342\232\240")!=std::string::npos) return std::string(LV_SYMBOL_WARNING) + " ";
    return "";  // strip unrecognized emoji
}

void UIManager::update_ddl_list(const std::vector<DDLEvent>& ev) {
    const TickType_t update_started = xTaskGetTickCount();
    // Show every unfinished future DDL, sorted by deadline. The server performs
    // the same filtering, but keeping it here prevents stale/expired sync data
    // from reappearing on the device.
    std::vector<size_t> active_idx;
    time_t now = time(nullptr);
    for (size_t i = 0; i < ev.size(); ++i) {
        if (ev[i].status != "pending" && ev[i].status != "snoozed") continue;
        int y, month, day, hour, minute;
        struct tm deadline_tm = {};
        if (sscanf(ev[i].deadline.c_str(), "%d-%d-%dT%d:%d", &y, &month, &day, &hour, &minute) == 5) {
            deadline_tm.tm_year = y - 1900; deadline_tm.tm_mon = month - 1; deadline_tm.tm_mday = day;
            deadline_tm.tm_hour = hour; deadline_tm.tm_min = minute; deadline_tm.tm_isdst = -1;
            if (mktime(&deadline_tm) < now) continue;
        }
        active_idx.push_back(i);
    }
    std::sort(active_idx.begin(), active_idx.end(), [&](size_t a, size_t b) {
        return ev[a].deadline < ev[b].deadline;
    });
    // Main screen cards: show top 2 active DDLs
    // Use cb_card_click (smart: checks user data for idx, falls back to view_all)
    if(active_idx.size()>0){
        auto& e=ev[active_idx[0]];
        std::string t=display_safe_text(UIManager::tag_text(e.tag)+e.course+" "+e.title);
        lv_label_set_text((lv_obj_t*)lv_obj_get_child(card1,0),t.c_str());
        lv_obj_clear_flag(card1,LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(card1, (void*)(uintptr_t)active_idx[0]);
    } else {
        lv_label_set_text((lv_obj_t*)lv_obj_get_child(card1,0),"暂无待办");
        lv_obj_clear_flag(card1,LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(card1, (void*)(uintptr_t)-1);
    }
    if(active_idx.size()>1){
        auto& e=ev[active_idx[1]];
        std::string t=display_safe_text(UIManager::tag_text(e.tag)+e.course+" "+e.title);
        lv_label_set_text((lv_obj_t*)lv_obj_get_child(card2,0),t.c_str());
        lv_obj_clear_flag(card2,LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(card2, (void*)(uintptr_t)active_idx[1]);
    } else {
        lv_obj_add_flag(card2,LV_OBJ_FLAG_HIDDEN);
    }
    // Cache the sorted data and repaint the fixed five-row pool. Never delete and
    // recreate list widgets during sync: rapid consecutive sync messages used to
    // leave main in lv_label_create long enough to trigger task_wdt.
    list_events_.clear();
    list_buckets_.clear();
    list_event_indices_ = active_idx;
    list_events_.reserve(active_idx.size());
    list_buckets_.reserve(active_idx.size());
    for (size_t event_index : active_idx) {
        const auto& event = ev[event_index];
        int minutes_left = 0;
        int yy, mm, dd, hh, mi;
        struct tm deadline_tm = {};
        if (sscanf(event.deadline.c_str(), "%d-%d-%dT%d:%d", &yy, &mm, &dd, &hh, &mi) == 5) {
            deadline_tm.tm_year = yy - 1900; deadline_tm.tm_mon = mm - 1; deadline_tm.tm_mday = dd;
            deadline_tm.tm_hour = hh; deadline_tm.tm_min = mi; deadline_tm.tm_isdst = -1;
            minutes_left = std::max(0, (int)(difftime(mktime(&deadline_tm), now) / 60));
        }
        list_events_.push_back(event);
        list_buckets_.push_back(minutes_left <= 1440 ? 0 : minutes_left <= 10080 ? 1 : minutes_left <= 43200 ? 2 : 3);
    }
    list_count_ = (int)active_idx.size();
    if (list_count_ == 0) list_idx_ = 0;
    else if (list_idx_ >= list_count_) list_idx_ = list_count_ - 1;
    highlight_list_item();
    ESP_LOGI(T, "DDL list updated: %d active, %lu ms", list_count_,
             (unsigned long)(pdTICKS_TO_MS(xTaskGetTickCount() - update_started)));
}

void UIManager::highlight_list_item() {
    if (!elist) return;
    static const char* bucket_names[] = {"1天内", "1-7天", "7天-1个月", "1个月后"};
    const int visible = (int)list_items_.size();
    if (list_count_ <= 0 || visible <= 0) {
        if (list_bucket_label_) lv_label_set_text(list_bucket_label_, "暂无待办");
        for (lv_obj_t* item : list_items_) lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
        if (list_slider_) lv_obj_add_flag(list_slider_, LV_OBJ_FLAG_HIDDEN);
        list_window_start_ = 0;
        return;
    }

    list_idx_ = std::max(0, std::min(list_idx_, list_count_ - 1));
    if (list_idx_ < list_window_start_) list_window_start_ = list_idx_;
    if (list_idx_ >= list_window_start_ + visible) list_window_start_ = list_idx_ - visible + 1;
    list_window_start_ = std::max(0, std::min(list_window_start_, std::max(0, list_count_ - visible)));
    if (list_bucket_label_ && list_idx_ < (int)list_buckets_.size()) {
        std::string heading = std::string("时间段: ") + bucket_names[list_buckets_[list_idx_]];
        lv_label_set_text(list_bucket_label_, heading.c_str());
    }

    for (int slot = 0; slot < visible; ++slot) {
        lv_obj_t* item = list_items_[slot];
        int logical = list_window_start_ + slot;
        if (logical >= list_count_ || logical >= (int)list_events_.size()) {
            lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(item, LV_OBJ_FLAG_HIDDEN);
        const auto& event = list_events_[logical];
        lv_obj_t* label = lv_obj_get_child(item, 0);
        lv_obj_t* due_label = lv_obj_get_child(item, 1);
        std::string title = display_safe_text(UIManager::tag_text(event.tag) + event.course + " - " + event.title);
        std::string due = bucket_names[list_buckets_[logical]];
        due += " · ";
        due += event.deadline.size() >= 16 ? event.deadline.substr(5, 11) : event.deadline;
        if (label) {
            // Stop the old marquee before replacing its text.
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
            lv_label_set_text(label, title.c_str());
        }
        if (due_label) lv_label_set_text(due_label, due.c_str());
        lv_obj_set_user_data(item, (void*)(uintptr_t)logical);
        bool selected = logical == list_idx_;
        lv_obj_set_style_bg_color(item, lv_color_hex(selected ? 0x245f91 : 0x16213e), 0);
        lv_obj_set_style_border_width(item, selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(item, lv_color_hex(0x74b9ff), 0);
        if (label) {
            lv_obj_set_style_text_color(label, lv_color_hex(selected ? 0xffffff : 0xcccccc), 0);
            lv_label_set_long_mode(label, selected ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP);
        }
    }
    if (list_slider_) {
        if (list_count_ > 1) {
            lv_slider_set_range(list_slider_, 0, list_count_ - 1);
            lv_slider_set_value(list_slider_, list_count_ - 1 - list_idx_, LV_ANIM_OFF);
            lv_obj_clear_flag(list_slider_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(list_slider_);
        } else {
            lv_obj_add_flag(list_slider_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Need access to global events vector
extern std::vector<DDLEvent> events;

// Smart card click: goes to detail if card has a valid event, else opens list
void UIManager::cb_card_click(lv_event_t* e) {
    lv_obj_t* target = lv_event_get_target(e);
    intptr_t idx = (intptr_t)lv_obj_get_user_data(target);
    if (idx >= 0 && idx < (intptr_t)events.size()) {
        instance().show_detail(events[(size_t)idx]);
    } else {
        instance().show_screen(LIST);
    }
}
void UIManager::set_emotion(const std::string& e) {
    if(!avatar_left_eye_ || !avatar_right_eye_ || !avatar_mouth_) return;
    const char *left = "-", *right = "-", *mouth = "_";
    lv_color_t hair = lv_color_hex(0x5b3a70);
    if(e=="happy") { left="^"; right="^"; mouth="u"; hair=lv_color_hex(0xe56b9f); }
    else if(e=="thinking") { left="o"; right="O"; mouth="~"; hair=lv_color_hex(0x526d9b); }
    else if(e=="surprised") { left="O"; right="O"; mouth="o"; hair=lv_color_hex(0xe17055); }
    else if(e=="sad") { left="T"; right="T"; mouth="n"; hair=lv_color_hex(0x466b92); }
    else if(e=="speaking") { left=">"; right="<"; mouth="D"; hair=lv_color_hex(0x2a9d8f); }
    lv_label_set_text(avatar_left_eye_, left);
    lv_label_set_text(avatar_right_eye_, right);
    lv_label_set_text(avatar_mouth_, mouth);
    if (avatar_hair_) lv_obj_set_style_bg_color(avatar_hair_, hair, 0);
}
void UIManager::show_asr_text(const std::string& text, bool is_final) {
    if (!asr_label_ || !asr_bar_) return;
    std::string safe = display_safe_text(text);
    bool is_user_line = safe.rfind("我: ", 0) == 0;
    bool is_assistant_line = safe.rfind("助手: ", 0) == 0;
    if (is_final && (is_user_line || is_assistant_line)) {
        conversation_entries_.push_back(safe);
        size_t retained_bytes = 0;
        for (const auto& entry : conversation_entries_) retained_bytes += entry.size() + 2;
        // Truncate whole oldest messages, never arbitrary UTF-8 bytes or half a
        // dialogue turn. About 6 KiB is ample for the small screen and LVGL heap.
        constexpr size_t MAX_HISTORY_BYTES = 6144;
        while (retained_bytes > MAX_HISTORY_BYTES && conversation_entries_.size() > 1) {
            retained_bytes -= conversation_entries_.front().size() + 2;
            conversation_entries_.erase(conversation_entries_.begin());
        }
        conversation_text_.clear();
        for (const auto& entry : conversation_entries_) {
            if (!conversation_text_.empty()) conversation_text_ += "\n\n";
            conversation_text_ += entry;
        }
        asr_full_text_ = conversation_text_;
    } else if (!safe.empty() && conversation_text_.empty()) {
        asr_full_text_ = safe;
    }

    // A list/paragraph reply is represented by its first line plus "……".
    // A normal single-line reply keeps the requested marquee effect.
    const size_t line_break = safe.find_first_of("\r\n");
    const bool multiline = line_break != std::string::npos;
    std::string bounded = multiline ? safe.substr(0, line_break) : safe;
    while (!bounded.empty() && (bounded.back() == ' ' || bounded.back() == '\t')) bounded.pop_back();
    if (multiline) bounded += "……";
    constexpr size_t MAX_ASR_BYTES = 384;
    if (bounded.size() > MAX_ASR_BYTES) {
        bounded.resize(MAX_ASR_BYTES);
        while (!bounded.empty() && ((uint8_t)bounded.back() & 0xC0) == 0x80) bounded.pop_back();
        bounded += "……";
    }
    lv_label_set_long_mode(asr_label_, multiline ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(asr_label_, bounded.c_str());
    if (is_final) {
        lv_obj_set_style_text_color(asr_label_, lv_color_hex(0x00ff88), 0);  // green for final recognized text
        lv_obj_set_style_bg_color(asr_bar_, lv_color_hex(0x0c2d4a), 0);
    } else if (text.find("听着") != std::string::npos) {
        lv_obj_set_style_text_color(asr_label_, lv_color_hex(0xffd93d), 0);  // yellow for listening
        lv_obj_set_style_bg_color(asr_bar_, lv_color_hex(0x0f3460), 0);
    } else {
        lv_obj_set_style_text_color(asr_label_, lv_color_hex(0xffffff), 0);  // white for TTS/speech
        lv_obj_set_style_bg_color(asr_bar_, lv_color_hex(0x0f3460), 0);
    }
}

void UIManager::set_connected(bool c) {
    if(status_icon) {
        lv_label_set_text(status_icon, c ? "*" : "O");
        lv_obj_set_style_text_color(status_icon, c ? lv_color_hex(0x00ff88) : lv_color_hex(0xff4444), 0);
    }
}
void UIManager::update_clock() {
    if(!clock_lbl||!date_lbl) return;
    time_t n=time(nullptr); struct tm* t=localtime(&n);
    char cb[16],db[32]; strftime(cb,sizeof(cb),"%H:%M",t); strftime(db,sizeof(db),"%Y-%m-%d %A",t);
    lv_label_set_text(clock_lbl,cb); lv_label_set_text(date_lbl,db);
}

void UIManager::highlight_tab(Tab t) {
    for (int i = 0; i < 3; i++) {
        if (!tab_btns_[i] || !tab_labels_[i]) continue;
        if (i == (int)t) {
            lv_obj_set_style_bg_color(tab_btns_[i], lv_color_hex(0x201900), 0);
            lv_obj_set_style_text_color(tab_labels_[i], lv_color_hex(0xf4cf57), 0);
            lv_obj_set_style_border_width(tab_btns_[i], 2, 0);
            lv_obj_set_style_border_color(tab_btns_[i], lv_color_hex(0xd4af37), 0);
        } else {
            lv_obj_set_style_bg_color(tab_btns_[i], lv_color_hex(0x101010), 0);
            lv_obj_set_style_text_color(tab_labels_[i], lv_color_hex(0x8c7a4f), 0);
            lv_obj_set_style_border_width(tab_btns_[i], 1, 0);
            lv_obj_set_style_border_color(tab_btns_[i], lv_color_hex(0x3a2f14), 0);
        }
    }
    tab_ = t;
}

void UIManager::select_tab(int dir) {
    int t = (int)tab_ + dir;
    if(t < 0) t = TAB_COUNT-1;
    if(t >= TAB_COUNT) t = 0;
    highlight_tab((Tab)t);
}

void UIManager::set_tab(Tab t) {
    highlight_tab(t);
    enter_tab();
}

void UIManager::enter_tab() {
    switch(tab_){
        case TAB_HOME: show_screen(MAIN); break;
        case TAB_DDL: show_screen(LIST); break;
        case TAB_SETTINGS: show_screen(SETTINGS); break;
        default: break;
    }
}

void UIManager::encoder_rotate(int direction) {
    direction = direction >= 0 ? 1 : -1;
    switch (scr_) {
        case MAIN:
            select_tab(direction);
            break;
        case LIST:
            if (list_count_ > 0) {
                list_idx_ = (list_idx_ + direction + list_count_) % list_count_;
                highlight_list_item();
            }
            break;
        case SETTINGS:
            if (set_editing && set_idx == 0) {
                volume_ += direction;
                if (volume_ < 0) volume_ = 0;
                if (volume_ > 10) volume_ = 10;
                if (g_on_volume_changed) g_on_volume_changed(volume_);
            } else {
                int count = setlist ? lv_obj_get_child_cnt(setlist) : 1;
                set_idx = (set_idx + direction + count) % count;
            }
            refresh_settings();
            break;
        case DETAIL:
            if (digit_edit_mode_ != EDIT_NONE) rotate_digit(direction);
            break;
        case REMINDER_EDITOR:
            if (reminder_editing_) adjust_reminder_field(direction);
            else if (!reminder_values_.empty()) {
                reminder_idx_ = (reminder_idx_ + direction + (int)reminder_values_.size()) % (int)reminder_values_.size();
                refresh_reminder_editor();
                if (reminder_idx_ < lv_obj_get_child_cnt(reminder_list_))
                    lv_obj_scroll_to_view(lv_obj_get_child(reminder_list_, reminder_idx_), LV_ANIM_OFF);
            }
            break;
        case TEXT_DETAIL:
            if (textdetail_label_) lv_obj_scroll_by(lv_obj_get_parent(textdetail_label_), 0, -direction * 32, LV_ANIM_OFF);
            break;
        default:
            break;
    }
}

void UIManager::encoder_press() {
    switch (scr_) {
        case MAIN:
            enter_tab();
            break;
        case LIST:
            if (list_idx_ >= 0 && list_idx_ < (int)list_events_.size())
                show_detail(list_events_[list_idx_]);
            break;
        case SETTINGS:
            if (set_idx == 0) {
                set_editing = !set_editing;
                refresh_settings();
            }
            break;
        case DETAIL:
            if (digit_edit_mode_ != EDIT_NONE) advance_or_commit_digit_edit();
            break;
        case REMINDER_EDITOR:
            if (reminder_values_.empty()) break;
            if (!reminder_editing_) {
                reminder_editing_ = true;
                reminder_field_ = 0;
                refresh_reminder_editor();
            } else if (++reminder_field_ > 2) {
                commit_reminders();
            } else {
                refresh_reminder_editor();
            }
            break;
        case TEXT_DETAIL:
            show_screen(MAIN);
            break;
        case REMINDER_POPUP:
            show_screen(MAIN);
            break;
        default:
            break;
    }
}

void UIManager::cb_tab_click(lv_event_t* e) {
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    instance().set_tab((Tab)idx);
}

void UIManager::show_detail(const DDLEvent& ev) {
    lv_mem_monitor_t mem_before = {};
    lv_mem_monitor(&mem_before);
    ESP_LOGI(T, "Opening detail: LVGL memory %u%% used, largest free %lu bytes, frag %u%%",
             mem_before.used_pct, (unsigned long)mem_before.free_biggest_size, mem_before.frag_pct);
    if (detail_id_ != ev.id) digit_edit_mode_ = EDIT_NONE;
    detail_id_=ev.id;
    d_advance_mins = ev.reminder_minutes.empty() ? ev.advance_minutes : ev.reminder_minutes.front();
    lv_label_set_text(dtitle,display_safe_text(ev.title).c_str());
    lv_label_set_text(dcourse,display_safe_text(ev.course).c_str());
    char buf[64];
    snprintf(buf,sizeof(buf),"Due: %s",ev.deadline.c_str());
    lv_label_set_text(ddeadline,buf);

    update_detail_countdown_label(ev);

    std::string reminder_summary = "提醒: ";
    for (size_t i = 0; i < ev.reminder_minutes.size(); ++i) {
        int minutes = ev.reminder_minutes[i];
        if (i) reminder_summary += "/";
        reminder_summary += format_reminder_offset(minutes);
    }
    if (ev.reminder_minutes.empty()) reminder_summary += "关闭";
    lv_label_set_text(dreminder, reminder_summary.c_str());

    snprintf(buf,sizeof(buf),"Status: %s",ev.status.c_str());
    lv_label_set_text(dstatus,buf);

    show_screen(DETAIL);
}

void UIManager::update_detail_countdown_label(const DDLEvent& ev) {
    if (!dcountdown) return;
    char buf[64];
    // Calculate real-time countdown locally (not server's static value).
    time_t now = time(nullptr);
    struct tm dl_tm = {};
    int y,M,d,h,m;
    if(sscanf(ev.deadline.c_str(),"%d-%d-%dT%d:%d",&y,&M,&d,&h,&m)>=5){
        dl_tm.tm_year=y-1900; dl_tm.tm_mon=M-1; dl_tm.tm_mday=d;
        dl_tm.tm_hour=h; dl_tm.tm_min=m; dl_tm.tm_sec=0;
        time_t dl_time = mktime(&dl_tm);
        int mins = (int)(difftime(dl_time, now)/60);
        // Human-readable: "3d 5h 12m left" — omit zero units
        if(mins == 0) {
            snprintf(buf,sizeof(buf),"due now");
        } else if(mins < 0) {
            int m = -mins;
            int d = m / 1440, h = (m % 1440) / 60, rm = m % 60;
            if(d > 0) snprintf(buf,sizeof(buf),"OVERDUE by %dd %dh %dm",d,h,rm);
            else if(h > 0) snprintf(buf,sizeof(buf),"OVERDUE by %dh %dm",h,rm);
            else snprintf(buf,sizeof(buf),"OVERDUE by %dm",rm);
        } else {
            int d = mins / 1440, h = (mins % 1440) / 60, m_rem = mins % 60;
            if(d > 0) snprintf(buf,sizeof(buf),"%dd %dh %dm left",d,h,m_rem);
            else if(h > 0) snprintf(buf,sizeof(buf),"%dh %dm left",h,m_rem);
            else snprintf(buf,sizeof(buf),"%dm left",m_rem);
        }
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    const char* current = lv_label_get_text(dcountdown);
    if (!current || strcmp(current, buf) != 0) lv_label_set_text(dcountdown, buf);
}

void UIManager::refresh_detail_countdown(const DDLEvent& ev) {
    if (scr_ != DETAIL || ev.id != detail_id_ || digit_edit_mode_ != EDIT_NONE) return;
    update_detail_countdown_label(ev);
}

// callbacks
void UIManager::cb_mic_press(lv_event_t*) { instance().set_emotion("neutral"); if(g_on_mic_press)g_on_mic_press(); }
void UIManager::cb_mic_release(lv_event_t*) { instance().set_emotion("thinking"); if(g_on_mic_release)g_on_mic_release(); }
void UIManager::cb_view_all(lv_event_t*) { instance().show_screen(LIST); }
// Need access to events vector — use extern
extern std::vector<DDLEvent> events;

void UIManager::cb_list_click(lv_event_t* e) {
    lv_obj_t* target = lv_event_get_target(e);
    size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(target);
    auto& u = instance();
    if(idx < u.list_events_.size()) u.show_detail(u.list_events_[idx]);
}
void UIManager::cb_list_slider(lv_event_t* e) {
    auto& u = instance();
    if (u.list_count_ <= 1) return;
    lv_obj_t* slider = lv_event_get_target(e);
    int value = lv_slider_get_value(slider);
    u.list_idx_ = u.list_count_ - 1 - value;
    u.highlight_list_item();
}
void UIManager::cb_back(lv_event_t*) { instance().show_screen(MAIN); }
void UIManager::cb_done(lv_event_t*) { if(g_on_event_action&&!instance().detail_id_.empty())g_on_event_action("done"); instance().show_screen(MAIN); }
void UIManager::cb_snooze(lv_event_t*) { if(g_on_event_action&&!instance().detail_id_.empty())g_on_event_action("snooze"); instance().show_screen(MAIN); }
void UIManager::cb_settings(lv_event_t*) { instance().show_screen(SETTINGS); }
