#include "ui_manager.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char* T = "UI";

std::function<void()> g_on_mic_press;
std::function<void()> g_on_mic_release;
std::function<void(const std::string&)> g_on_event_action;
std::function<void()> g_on_request_sync;

UIManager& UIManager::instance() { static UIManager m; return m; }

esp_err_t UIManager::init() {
    mk_main(); mk_list(); mk_detail(); mk_popup(); mk_settings();
    // mk_keyboard() — disabled: too many LVGL objects triggers WDT
    show_screen(MAIN); ESP_LOGI(T, "UI ready"); return ESP_OK;
}

void UIManager::show_screen(Screen s) {
    if(mscr) lv_obj_add_flag(mscr, LV_OBJ_FLAG_HIDDEN);
    if(lscr) lv_obj_add_flag(lscr, LV_OBJ_FLAG_HIDDEN);
    if(dscr) lv_obj_add_flag(dscr, LV_OBJ_FLAG_HIDDEN);
    if(setscr) lv_obj_add_flag(setscr, LV_OBJ_FLAG_HIDDEN);
    if(kbscr) lv_obj_add_flag(kbscr, LV_OBJ_FLAG_HIDDEN);
    switch(s) {
        case MAIN: if(mscr){lv_obj_clear_flag(mscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(mscr); update_clock();} break;
        case LIST: if(lscr){lv_obj_clear_flag(lscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(lscr);} break;
        case DETAIL: if(dscr){lv_obj_clear_flag(dscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(dscr);} break;
        case SETTINGS: if(setscr){lv_obj_clear_flag(setscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(setscr);} break;
        case KEYBOARD: if(kbscr){lv_obj_clear_flag(kbscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(kbscr);} break;
        default: break;
    } scr_=s;
}

// CJK font — covers U+4E00..U+9FFF (CJK Unified Ideographs)
LV_FONT_DECLARE(my_chinese_font);

// RAM copy of default font chained to CJK fallback.
// When a glyph isn't in the default font, LVGL tries the fallback automatically.
static lv_font_t font_with_cjk;
static bool font_ready = false;

static void ensure_cjk_font() {
    if (font_ready) return;
    // Use CJK font as PRIMARY (1bpp crisp), with default font as fallback for missing glyphs
    font_with_cjk = my_chinese_font;
    font_with_cjk.fallback = LV_FONT_DEFAULT;
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
    if(txt){
        lv_obj_t* lb=lv_label_create(b);
        lv_label_set_text(lb,txt);
        lv_obj_center(lb);
        ensure_cjk_font();
        lv_obj_set_style_text_font(lb, &font_with_cjk, 0);
    }
    return b;
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

    // === Row 2: Emoji expression (y=56..100) ===
    emoji_lbl=mk_label(mscr,"^_^");
    lv_obj_set_style_text_font(emoji_lbl, &font_with_cjk, 0);
    lv_obj_set_style_text_color(emoji_lbl,lv_color_hex(0xffd93d),0);
    lv_obj_align(emoji_lbl,LV_ALIGN_TOP_MID,0,56);

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
    lv_obj_set_width(asr_label_, LV_PCT(100));
    lv_label_set_long_mode(asr_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(asr_label_, LV_ALIGN_LEFT_MID, 4, 0);

    // === Row 4: DDL cards (y=144..236, 2 list-style items) ===
    // Card 1
    card1=lv_obj_create(mscr); lv_obj_set_size(card1,226,38);
    lv_obj_set_style_bg_color(card1,lv_color_hex(0x16213e),0);
    lv_obj_set_style_radius(card1,8,0); lv_obj_set_style_pad_all(card1,8,0);
    lv_obj_set_style_border_width(card1,0,0);
    lv_obj_align(card1,LV_ALIGN_TOP_MID,0,144);
    lv_obj_t* c1l = mk_label(card1,"暂无待办");
    lv_obj_set_style_text_color(c1l,lv_color_hex(0xcccccc),0);
    lv_label_set_long_mode(c1l,LV_LABEL_LONG_SCROLL_CIRCULAR);
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
    lv_label_set_long_mode(c2l,LV_LABEL_LONG_SCROLL_CIRCULAR);
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
        lv_obj_set_style_radius(tab_btns_[i], 10, 0);
        lv_obj_set_style_bg_color(tab_btns_[i], lv_color_hex(0x16213e), 0);
        lv_obj_set_style_border_width(tab_btns_[i], 0, 0);
        lv_obj_align(tab_btns_[i], LV_ALIGN_BOTTOM_MID, -95 + i * 58, -16);

        tab_labels_[i] = lv_label_create(tab_btns_[i]);
        ensure_cjk_font();
        lv_obj_set_style_text_font(tab_labels_[i], &font_with_cjk, 0);
        lv_label_set_text(tab_labels_[i], tab_names[i]);
        lv_obj_set_style_text_color(tab_labels_[i], lv_color_hex(0x888888), 0);
        lv_obj_center(tab_labels_[i]);

        lv_obj_add_event_cb(tab_btns_[i], cb_tab_click, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }
    // Highlight current tab
    highlight_tab(TAB_HOME);

    // Large record button (round, red, 56×56)
    mic_btn_ = lv_btn_create(mscr);
    lv_obj_set_size(mic_btn_, 56, 56);
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
    lv_obj_t* bb=mk_btn(lscr,50,25,"\342\206\220"); lv_obj_set_style_bg_color(bb,lv_color_hex(0x0f3460),0);
    lv_obj_set_style_text_color(lv_obj_get_child(bb,0),lv_color_hex(0xffffff),0);
    lv_obj_align(bb,LV_ALIGN_TOP_LEFT,5,5); lv_obj_add_event_cb(bb,cb_back,LV_EVENT_CLICKED,NULL);

    // Title
    lv_obj_t* ti=mk_label(lscr,"DDL \345\210\227\350\241\250"); lv_obj_set_style_text_color(ti,lv_color_hex(0xffffff),0);
    lv_obj_align(ti,LV_ALIGN_TOP_MID,0,8);

    // Event list (simple container, manual positioning — no flex to avoid re-layout WDT)
    elist=lv_obj_create(lscr); lv_obj_set_size(elist,225,240);
    lv_obj_align(elist,LV_ALIGN_TOP_MID,0,40);
    lv_obj_set_style_bg_color(elist,lv_color_hex(0x16213e),0);
    lv_obj_set_style_radius(elist,6,0);
    lv_obj_set_style_pad_all(elist,2,0);
    lv_obj_set_style_border_width(elist,0,0);
    lv_obj_set_scrollbar_mode(elist, LV_SCROLLBAR_MODE_OFF);

    // Refresh button (top right)
    lv_obj_t* rb=mk_btn(lscr,60,25,"Refresh"); lv_obj_set_style_bg_color(rb,lv_color_hex(0x0f3460),0);
    lv_obj_set_style_text_color(lv_obj_get_child(rb,0),lv_color_hex(0xffffff),0);
    lv_obj_align(rb,LV_ALIGN_TOP_RIGHT,-5,5);
    lv_obj_add_event_cb(rb, [](lv_event_t*){ if(g_on_request_sync) g_on_request_sync(); }, LV_EVENT_CLICKED, NULL);

    // Settings gear button
    lv_obj_t* sb=mk_btn(lscr,44,44,"S"); lv_obj_set_style_radius(sb,22,0);
    lv_obj_set_style_bg_color(sb,lv_color_hex(0x0f3460),0);
    lv_obj_set_style_text_color(lv_obj_get_child(sb,0),lv_color_hex(0xffffff),0);
    lv_obj_align(sb,LV_ALIGN_BOTTOM_RIGHT,-8,-8);
    lv_obj_add_event_cb(sb,cb_settings,LV_EVENT_CLICKED,NULL);
}

void UIManager::mk_detail() {
    dscr=lv_obj_create(NULL); lv_obj_set_size(dscr,240,320); lv_obj_set_style_bg_color(dscr,lv_color_hex(0x1a1a2e),0);
    // Back button
    lv_obj_t* bb=mk_btn(dscr,50,25,"\342\206\220"); lv_obj_set_style_bg_color(bb,lv_color_hex(0x0f3460),0); lv_obj_align(bb,LV_ALIGN_TOP_LEFT,5,5); lv_obj_add_event_cb(bb,cb_back,LV_EVENT_CLICKED,NULL);

    // Editable fields — each is a clickable label
    dtitle=mk_label(dscr,""); lv_obj_set_style_text_color(dtitle,lv_color_hex(0xffffff),0); lv_obj_align(dtitle,LV_ALIGN_TOP_MID,0,8);
    lv_obj_add_flag(dtitle, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(dtitle, cb_detail_edit, LV_EVENT_CLICKED, (void*)"title");

    dcourse=mk_label(dscr,""); lv_obj_set_style_text_color(dcourse,lv_color_hex(0x888888),0); lv_obj_align(dcourse,LV_ALIGN_TOP_MID,0,35);
    lv_obj_add_flag(dcourse, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(dcourse, cb_detail_edit, LV_EVENT_CLICKED, (void*)"course");

    ddeadline=mk_label(dscr,""); lv_obj_set_style_text_color(ddeadline,lv_color_hex(0xe94560),0); lv_obj_align(ddeadline,LV_ALIGN_TOP_MID,0,60);
    lv_obj_add_flag(ddeadline, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(ddeadline, cb_detail_edit, LV_EVENT_CLICKED, (void*)"deadline");

    dcountdown=mk_label(dscr,""); lv_obj_set_style_text_color(dcountdown,lv_color_hex(0xffd93d),0); lv_obj_align(dcountdown,LV_ALIGN_TOP_MID,0,90);

    dreminder=mk_label(dscr,""); lv_obj_set_style_text_color(dreminder,lv_color_hex(0x74b9ff),0); lv_obj_align(dreminder,LV_ALIGN_TOP_MID,0,120);
    lv_obj_add_flag(dreminder, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(dreminder, cb_detail_edit, LV_EVENT_CLICKED, (void*)"advance");

    dstatus=mk_label(dscr,""); lv_obj_set_style_text_color(dstatus,lv_color_hex(0x00b894),0); lv_obj_align(dstatus,LV_ALIGN_TOP_MID,0,145);

    // Hint text (updated for touch editing)
    lv_obj_t* hint = mk_label(dscr,"Tap field to edit | Rot:prev/next");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_obj_t* db=mk_btn(dscr,90,35,"Done"); lv_obj_set_style_bg_color(db,lv_color_hex(0x00b894),0); lv_obj_set_style_radius(db,17,0); lv_obj_align(db,LV_ALIGN_BOTTOM_LEFT,15,-45); lv_obj_add_event_cb(db,cb_done,LV_EVENT_CLICKED,NULL);
    lv_obj_t* sb=mk_btn(dscr,90,35,"Later"); lv_obj_set_style_bg_color(sb,lv_color_hex(0xfdcb6e),0); lv_obj_set_style_radius(sb,17,0); lv_obj_set_style_text_color(sb,lv_color_hex(0x000000),0); lv_obj_align(sb,LV_ALIGN_BOTTOM_RIGHT,-15,-45); lv_obj_add_event_cb(sb,cb_snooze,LV_EVENT_CLICKED,NULL);
}

void UIManager::mk_popup() {
    popup=lv_obj_create(lv_scr_act()); lv_obj_set_size(popup,220,220);
    lv_obj_set_style_bg_color(popup,lv_color_hex(0x2d2d44),0); lv_obj_set_style_border_color(popup,lv_color_hex(0xe94560),0); lv_obj_set_style_border_width(popup,3,0); lv_obj_set_style_radius(popup,12,0); lv_obj_center(popup); lv_obj_add_flag(popup,LV_OBJ_FLAG_HIDDEN);
    mk_label(popup,"\360\237\224\224"); lv_obj_align(lv_obj_get_child(popup,-1),LV_ALIGN_TOP_MID,0,10);
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
    lv_async_call(ui_show_keyboard_cb, d);
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

void UIManager::mk_settings() {
    setscr=lv_obj_create(NULL); lv_obj_set_size(setscr,240,320); lv_obj_set_style_bg_color(setscr,lv_color_hex(0x1a1a2e),0);
    lv_obj_set_style_pad_all(setscr,0,0);

    lv_obj_t* bb=mk_btn(setscr,50,25,"\342\206\220"); lv_obj_set_style_bg_color(bb,lv_color_hex(0x0f3460),0);
    lv_obj_set_style_text_color(lv_obj_get_child(bb,0),lv_color_hex(0xffffff),0);
    lv_obj_align(bb,LV_ALIGN_TOP_LEFT,5,5); lv_obj_add_event_cb(bb,cb_back,LV_EVENT_CLICKED,NULL);

    lv_obj_t* ti=mk_label(setscr,"\350\256\276\347\275\256"); lv_obj_set_style_text_color(ti,lv_color_hex(0xffffff),0);
    lv_obj_align(ti,LV_ALIGN_TOP_MID,0,8);

    // Settings list (scrollable container)
    setlist=lv_obj_create(setscr); lv_obj_set_size(setlist,225,240);
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
}

// Settings item click: start editing or toggle value
void UIManager::cb_set_click(lv_event_t* e) {
    auto& u = instance();
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    u.set_idx = idx;

    if (idx == 0) { // Volume: enter edit mode
        u.set_editing = !u.set_editing;
        lv_obj_t* btn = lv_obj_get_child(u.setlist, idx);
        lv_obj_t* lbl = lv_obj_get_child(btn, 0);
        if (u.set_editing) {
            lv_label_set_text(lbl, "Volume: [8/10] <->");
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xffd93d), 0);
        } else {
            lv_label_set_text(lbl, "Volume: 8/10");
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        }
    }
}
static void cb_set_click(lv_event_t* e) { UIManager::cb_set_click(e); }

// public methods
void UIManager::show_reminder(const DDLEvent& e) { lv_label_set_text(rtitle,(UIManager::UIManager::tag_text(e.tag)+e.title).c_str()); lv_label_set_text(rdeadline,("\346\210\252\346\255\242: "+e.deadline).c_str()); lv_obj_clear_flag(popup,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(popup); scr_=REMINDER_POPUP; }
// Map emoji tags to ASCII text (emoji glyphs not in our font)
std::string UIManager::tag_text(const std::string& emoji_tag) {
    if(emoji_tag.find("\360\237\224\245")!=std::string::npos) return "!";  // fire
    if(emoji_tag.find("\342\232\241")!=std::string::npos) return "!!";     // zap
    if(emoji_tag.find("\360\237\223\214")!=std::string::npos) return "~";  // pin
    if(emoji_tag.find("\342\234\205")!=std::string::npos) return "v";      // check
    if(emoji_tag.find("\342\232\240")!=std::string::npos) return "!";      // warning
    return "";  // strip unrecognized emoji
}

void UIManager::update_ddl_list(const std::vector<DDLEvent>& ev) {
    // Filter: only show pending/snoozed (hide done/deleted)
    std::vector<int> active_idx;
    for(size_t i=0;i<ev.size();i++){
        if(ev[i].status=="pending"||ev[i].status=="snoozed") active_idx.push_back(i);
    }
    // Main screen cards: show top 2 active DDLs
    // Use cb_card_click (smart: checks user data for idx, falls back to view_all)
    if(active_idx.size()>0){
        auto& e=ev[active_idx[0]];
        std::string t=UIManager::tag_text(e.tag)+e.course+" "+e.title;
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
        std::string t=UIManager::tag_text(e.tag)+e.course+" "+e.title;
        lv_label_set_text((lv_obj_t*)lv_obj_get_child(card2,0),t.c_str());
        lv_obj_clear_flag(card2,LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(card2, (void*)(uintptr_t)active_idx[1]);
    } else {
        lv_obj_add_flag(card2,LV_OBJ_FLAG_HIDDEN);
    }
    // List: reuse items, position manually (no flex), max 6 visible
    int existing = lv_obj_get_child_cnt(elist);
    int needed = (int)active_idx.size();
    if (needed > 6) needed = 6;  // limit visible items to avoid render overload

    // Hide excess items
    for (int i = needed; i < existing; i++) {
        lv_obj_add_flag(lv_obj_get_child(elist, i), LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < needed; i++) {
        auto& e = ev[active_idx[i]];
        std::string t = UIManager::tag_text(e.tag) + e.course + " - " + e.title;
        lv_obj_t* b;
        if (i < existing) {
            b = lv_obj_get_child(elist, i);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
        } else {
            b = lv_btn_create(elist);
            lv_obj_set_size(b, 215, 36);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x16213e), 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_radius(b, 4, 0);
            lv_obj_t* bl = lv_label_create(b);
            ensure_cjk_font(); lv_obj_set_style_text_font(bl, &font_with_cjk, 0);
            lv_obj_set_style_text_color(bl, lv_color_hex(0xcccccc), 0);
            lv_obj_align(bl, LV_ALIGN_LEFT_MID, 8, 0);
            lv_obj_add_event_cb(b, cb_list_click, LV_EVENT_CLICKED, nullptr);
        }
        // Manual position (no flex layout = no expensive re-layout)
        lv_obj_set_pos(b, 2, 2 + i * 38);
        // Update label text
        lv_obj_t* bl = lv_obj_get_child(b, 0);
        if (bl) lv_label_set_text(bl, t.c_str());
        // Store event index on the object
        lv_obj_set_user_data(b, (void*)(uintptr_t)active_idx[i]);
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
    if(!emoji_lbl) return;
    // Text-based expressions (LVGL default fonts don't have emoji glyphs)
    if(e=="happy") { lv_label_set_text(emoji_lbl,"^_^"); lv_obj_set_style_text_color(emoji_lbl,lv_color_hex(0xffd93d),0); }
    else if(e=="thinking") { lv_label_set_text(emoji_lbl,"o.O"); lv_obj_set_style_text_color(emoji_lbl,lv_color_hex(0x74b9ff),0); }
    else if(e=="surprised") { lv_label_set_text(emoji_lbl,"O_O"); lv_obj_set_style_text_color(emoji_lbl,lv_color_hex(0xff7675),0); }
    else if(e=="sad") { lv_label_set_text(emoji_lbl,"T_T"); lv_obj_set_style_text_color(emoji_lbl,lv_color_hex(0x74b9ff),0); }
    else if(e=="speaking") { lv_label_set_text(emoji_lbl,">_<"); lv_obj_set_style_text_color(emoji_lbl,lv_color_hex(0x00b894),0); }
    else { lv_label_set_text(emoji_lbl,"-_-"); lv_obj_set_style_text_color(emoji_lbl,lv_color_hex(0xffd93d),0); }
}
void UIManager::show_asr_text(const std::string& text, bool is_final) {
    if (!asr_label_ || !asr_bar_) return;
    lv_label_set_text(asr_label_, text.c_str());
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
            lv_obj_set_style_bg_color(tab_btns_[i], lv_color_hex(0x0f3460), 0);
            lv_obj_set_style_text_color(tab_labels_[i], lv_color_hex(0xffffff), 0);
        } else {
            lv_obj_set_style_bg_color(tab_btns_[i], lv_color_hex(0x16213e), 0);
            lv_obj_set_style_text_color(tab_labels_[i], lv_color_hex(0x888888), 0);
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

void UIManager::cb_tab_click(lv_event_t* e) {
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    instance().set_tab((Tab)idx);
}

void UIManager::show_detail(const DDLEvent& ev) {
    detail_id_=ev.id;
    d_advance_mins = ev.advance_minutes;
    lv_label_set_text(dtitle,ev.title.c_str());
    lv_label_set_text(dcourse,ev.course.c_str());
    char buf[64];
    snprintf(buf,sizeof(buf),"Due: %s",ev.deadline.c_str());
    lv_label_set_text(ddeadline,buf);

    // Calculate real-time countdown locally (not server's static value)
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
        lv_label_set_text(dcountdown,buf);
    }

    int adv = ev.advance_minutes;
    if(adv >= 1440) {
        int d = adv / 1440;
        int h = (adv % 1440) / 60;
        if(h > 0) snprintf(buf,sizeof(buf),"Remind: %dd %dh before",d,h);
        else snprintf(buf,sizeof(buf),"Remind: %dd before",d);
    } else if(adv >= 60) {
        int h = adv / 60;
        int m = adv % 60;
        if(m > 0) snprintf(buf,sizeof(buf),"Remind: %dh %dm before",h,m);
        else snprintf(buf,sizeof(buf),"Remind: %dh before",h);
    } else {
        snprintf(buf,sizeof(buf),"Remind: %dm before",adv);
    }
    lv_label_set_text(dreminder,buf);

    snprintf(buf,sizeof(buf),"Status: %s",ev.status.c_str());
    lv_label_set_text(dstatus,buf);

    show_screen(DETAIL);
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
    if(idx < events.size()) {
        instance().show_detail(events[idx]);
    }
}
void UIManager::cb_back(lv_event_t*) { instance().show_screen(MAIN); }
void UIManager::cb_done(lv_event_t*) { if(g_on_event_action&&!instance().detail_id_.empty())g_on_event_action("done"); instance().show_screen(MAIN); }
void UIManager::cb_snooze(lv_event_t*) { if(g_on_event_action&&!instance().detail_id_.empty())g_on_event_action("snooze"); instance().show_screen(MAIN); }
void UIManager::cb_settings(lv_event_t*) { instance().show_screen(SETTINGS); }
