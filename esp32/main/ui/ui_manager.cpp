#include "ui_manager.h"
#include "esp_log.h"
#include <cstdio>
#include <ctime>
#include <functional>
static const char* T = "UI";

std::function<void()> g_on_mic_press;
std::function<void()> g_on_mic_release;
std::function<void(const std::string&)> g_on_event_action;
std::function<void()> g_on_request_sync;

UIManager& UIManager::instance() { static UIManager m; return m; }

esp_err_t UIManager::init() {
    mk_main(); mk_list(); mk_detail(); mk_popup(); mk_settings();
    show_screen(MAIN); ESP_LOGI(T, "UI ready"); return ESP_OK;
}

void UIManager::show_screen(Screen s) {
    if(mscr) lv_obj_add_flag(mscr, LV_OBJ_FLAG_HIDDEN);
    if(lscr) lv_obj_add_flag(lscr, LV_OBJ_FLAG_HIDDEN);
    if(dscr) lv_obj_add_flag(dscr, LV_OBJ_FLAG_HIDDEN);
    if(setscr) lv_obj_add_flag(setscr, LV_OBJ_FLAG_HIDDEN);
    switch(s) {
        case MAIN: if(mscr){lv_obj_clear_flag(mscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(mscr); update_clock();} break;
        case LIST: if(lscr){lv_obj_clear_flag(lscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(lscr);} break;
        case DETAIL: if(dscr){lv_obj_clear_flag(dscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(dscr);} break;
        case SETTINGS: if(setscr){lv_obj_clear_flag(setscr,LV_OBJ_FLAG_HIDDEN); lv_scr_load(setscr);} break;
        default: break;
    } scr_=s;
}

// helpers
static lv_obj_t* mk_label(lv_obj_t* p, const char* txt) { lv_obj_t* l=lv_label_create(p); lv_label_set_text(l,txt); return l; }
static lv_obj_t* mk_btn(lv_obj_t* p, int w, int h, const char* txt) { lv_obj_t* b=lv_btn_create(p); lv_obj_set_size(b,w,h); if(txt){lv_obj_t* lb=lv_label_create(b); lv_label_set_text(lb,txt); lv_obj_center(lb);} return b; }

void UIManager::mk_main() {
    mscr=lv_obj_create(NULL); lv_obj_set_size(mscr,240,240);
    lv_obj_set_style_bg_color(mscr,lv_color_hex(0x1a1a2e),0); lv_obj_set_style_pad_all(mscr,0,0);

    // Large clock display
    clock_lbl=mk_label(mscr,"00:00");
    lv_obj_set_style_text_color(clock_lbl,lv_color_hex(0xffffff),0);
    lv_obj_align(clock_lbl,LV_ALIGN_TOP_MID,0,8);

    // Date below clock
    date_lbl=mk_label(mscr,"----.--.--");
    lv_obj_set_style_text_color(date_lbl,lv_color_hex(0x888888),0);
    lv_obj_align(date_lbl,LV_ALIGN_TOP_MID,0,52);

    // Character expression (text-based, no emoji dependency)
    emoji_lbl=mk_label(mscr,"^_^");
    lv_obj_set_style_text_color(emoji_lbl,lv_color_hex(0xffd93d),0);
    lv_obj_align(emoji_lbl,LV_ALIGN_CENTER,0,-12);

    // Card 1 (top DDL)
    card1=lv_obj_create(mscr); lv_obj_set_size(card1,220,32);
    lv_obj_set_style_bg_color(card1,lv_color_hex(0x16213e),0);
    lv_obj_set_style_radius(card1,8,0); lv_obj_set_style_pad_all(card1,6,0);
    lv_obj_set_style_border_width(card1,0,0);
    lv_obj_align(card1,LV_ALIGN_BOTTOM_MID,0,-65);
    lv_obj_t* c1l = mk_label(card1,"\346\232\202\346\227\240\345\276\205\345\212\236"); // 暂无待办
    lv_obj_set_style_text_color(c1l,lv_color_hex(0xcccccc),0);
    lv_label_set_long_mode(c1l,LV_LABEL_LONG_SCROLL_CIRCULAR);

    // Card 2 (second DDL)
    card2=lv_obj_create(mscr); lv_obj_set_size(card2,220,32);
    lv_obj_set_style_bg_color(card2,lv_color_hex(0x16213e),0);
    lv_obj_set_style_radius(card2,8,0); lv_obj_set_style_pad_all(card2,6,0);
    lv_obj_set_style_border_width(card2,0,0);
    lv_obj_align(card2,LV_ALIGN_BOTTOM_MID,0,-25); lv_obj_add_flag(card2,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* c2l = mk_label(card2,"");
    lv_obj_set_style_text_color(c2l,lv_color_hex(0xcccccc),0);
    lv_label_set_long_mode(c2l,LV_LABEL_LONG_SCROLL_CIRCULAR);

    // Mic button (red circle)
    lv_obj_t* mb=mk_btn(mscr,60,60,"MIC");
    lv_obj_set_style_radius(mb,30,0);
    lv_obj_set_style_bg_color(mb,lv_color_hex(0xe94560),0);
    lv_obj_set_style_text_color(lv_obj_get_child(mb,0),lv_color_hex(0xffffff),0);
    lv_obj_align(mb,LV_ALIGN_BOTTOM_RIGHT,-15,-15);
    lv_obj_add_event_cb(mb,cb_mic_press,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(mb,cb_mic_release,LV_EVENT_RELEASED,NULL);

    // View all button
    lv_obj_t* va=mk_btn(mscr,100,28,"\346\237\245\347\234\213\345\205\250\351\203\250"); // 查看全部
    lv_obj_set_style_bg_color(va,lv_color_hex(0x0f3460),0);
    lv_obj_set_style_radius(va,14,0);
    lv_obj_set_style_text_color(lv_obj_get_child(va,0),lv_color_hex(0xffffff),0);
    lv_obj_align(va,LV_ALIGN_BOTTOM_LEFT,15,-30);
    lv_obj_add_event_cb(va,cb_view_all,LV_EVENT_CLICKED,NULL);

    // Connection status icon
    status_icon=mk_label(mscr,"O");  // O = disconnected, will show green dot when connected
    lv_obj_set_style_text_color(status_icon,lv_color_hex(0xff4444),0);
    lv_obj_align(status_icon,LV_ALIGN_TOP_RIGHT,-8,8);
}

void UIManager::mk_list() {
    lscr=lv_obj_create(NULL); lv_obj_set_size(lscr,240,240); lv_obj_set_style_bg_color(lscr,lv_color_hex(0x1a1a2e),0);
    lv_obj_set_style_pad_all(lscr,0,0);

    // Back button
    lv_obj_t* bb=mk_btn(lscr,50,25,"\342\206\220"); lv_obj_set_style_bg_color(bb,lv_color_hex(0x0f3460),0);
    lv_obj_set_style_text_color(lv_obj_get_child(bb,0),lv_color_hex(0xffffff),0);
    lv_obj_align(bb,LV_ALIGN_TOP_LEFT,5,5); lv_obj_add_event_cb(bb,cb_back,LV_EVENT_CLICKED,NULL);

    // Title
    lv_obj_t* ti=mk_label(lscr,"DDL \345\210\227\350\241\250"); lv_obj_set_style_text_color(ti,lv_color_hex(0xffffff),0);
    lv_obj_align(ti,LV_ALIGN_TOP_MID,0,8);

    // Event list (scrollable)
    elist=lv_list_create(lscr); lv_obj_set_size(elist,225,168);
    lv_obj_align(elist,LV_ALIGN_TOP_MID,0,40);
    lv_obj_set_style_bg_color(elist,lv_color_hex(0x16213e),0);
    lv_obj_set_style_radius(elist,6,0);
    lv_obj_set_style_pad_all(elist,0,0);
    lv_obj_set_style_border_width(elist,0,0);

    // Settings gear button
    lv_obj_t* sb=mk_btn(lscr,44,44,"S"); lv_obj_set_style_radius(sb,22,0);
    lv_obj_set_style_bg_color(sb,lv_color_hex(0x0f3460),0);
    lv_obj_set_style_text_color(lv_obj_get_child(sb,0),lv_color_hex(0xffffff),0);
    lv_obj_align(sb,LV_ALIGN_BOTTOM_RIGHT,-8,-8);
    lv_obj_add_event_cb(sb,cb_settings,LV_EVENT_CLICKED,NULL);
}

void UIManager::mk_detail() {
    dscr=lv_obj_create(NULL); lv_obj_set_size(dscr,240,240); lv_obj_set_style_bg_color(dscr,lv_color_hex(0x1a1a2e),0);
    lv_obj_t* bb=mk_btn(dscr,50,25,"\342\206\220"); lv_obj_set_style_bg_color(bb,lv_color_hex(0x0f3460),0); lv_obj_align(bb,LV_ALIGN_TOP_LEFT,5,5); lv_obj_add_event_cb(bb,cb_back,LV_EVENT_CLICKED,NULL);
    dtitle=mk_label(dscr,"\350\257\246\346\203\205"); lv_obj_set_style_text_color(dtitle,lv_color_hex(0xffffff),0); lv_obj_align(dtitle,LV_ALIGN_TOP_MID,0,8);
    dcourse=mk_label(dscr,""); lv_obj_set_style_text_color(dcourse,lv_color_hex(0x888888),0); lv_obj_align(dcourse,LV_ALIGN_TOP_MID,0,35);
    ddeadline=mk_label(dscr,""); lv_obj_set_style_text_color(ddeadline,lv_color_hex(0xe94560),0); lv_obj_align(ddeadline,LV_ALIGN_TOP_MID,0,60);
    dcountdown=mk_label(dscr,""); lv_obj_align(dcountdown,LV_ALIGN_CENTER,0,10);
    lv_obj_t* db=mk_btn(dscr,90,35,"\342\234\223 \345\256\214\346\210\220"); lv_obj_set_style_bg_color(db,lv_color_hex(0x00b894),0); lv_obj_set_style_radius(db,17,0); lv_obj_align(db,LV_ALIGN_BOTTOM_LEFT,15,-15); lv_obj_add_event_cb(db,cb_done,LV_EVENT_CLICKED,NULL);
    lv_obj_t* sb=mk_btn(dscr,90,35,"\342\217\260 \347\250\215\345\220\216"); lv_obj_set_style_bg_color(sb,lv_color_hex(0xfdcb6e),0); lv_obj_set_style_radius(sb,17,0); lv_obj_set_style_text_color(sb,lv_color_hex(0x000000),0); lv_obj_align(sb,LV_ALIGN_BOTTOM_RIGHT,-15,-15); lv_obj_add_event_cb(sb,cb_snooze,LV_EVENT_CLICKED,NULL);
}

void UIManager::mk_popup() {
    popup=lv_obj_create(lv_scr_act()); lv_obj_set_size(popup,220,190);
    lv_obj_set_style_bg_color(popup,lv_color_hex(0x2d2d44),0); lv_obj_set_style_border_color(popup,lv_color_hex(0xe94560),0); lv_obj_set_style_border_width(popup,3,0); lv_obj_set_style_radius(popup,12,0); lv_obj_center(popup); lv_obj_add_flag(popup,LV_OBJ_FLAG_HIDDEN);
    mk_label(popup,"\360\237\224\224"); lv_obj_align(lv_obj_get_child(popup,-1),LV_ALIGN_TOP_MID,0,10);
    rtitle=mk_label(popup,"DDL \346\217\220\351\206\222"); lv_obj_align(rtitle,LV_ALIGN_TOP_MID,0,50);
    rdeadline=mk_label(popup,""); lv_obj_set_style_text_color(rdeadline,lv_color_hex(0xe94560),0); lv_obj_align(rdeadline,LV_ALIGN_TOP_MID,0,75);
    lv_obj_t* cf=mk_btn(popup,100,35,"\347\237\245\351\201\223\344\272\206"); lv_obj_set_style_bg_color(cf,lv_color_hex(0x00b894),0); lv_obj_set_style_radius(cf,17,0); lv_obj_align(cf,LV_ALIGN_BOTTOM_LEFT,8,-12); lv_obj_add_event_cb(cf,[](lv_event_t*){UIManager::instance().show_screen(MAIN);},LV_EVENT_CLICKED,NULL);
    lv_obj_t* sz=mk_btn(popup,100,35,"5\345\210\206\351\222\237\345\220\216"); lv_obj_set_style_bg_color(sz,lv_color_hex(0xfdcb6e),0); lv_obj_set_style_radius(sz,17,0); lv_obj_align(sz,LV_ALIGN_BOTTOM_RIGHT,-8,-12); lv_obj_add_event_cb(sz,[](lv_event_t*){if(g_on_event_action)g_on_event_action("snooze");UIManager::instance().show_screen(MAIN);},LV_EVENT_CLICKED,NULL);
}

void UIManager::mk_settings() {
    setscr=lv_obj_create(NULL); lv_obj_set_size(setscr,240,240); lv_obj_set_style_bg_color(setscr,lv_color_hex(0x1a1a2e),0);
    lv_obj_t* bb=mk_btn(setscr,50,25,"\342\206\220"); lv_obj_set_style_bg_color(bb,lv_color_hex(0x0f3460),0); lv_obj_align(bb,LV_ALIGN_TOP_LEFT,5,5); lv_obj_add_event_cb(bb,cb_back,LV_EVENT_CLICKED,NULL);
    lv_obj_t* ti=mk_label(setscr,"\350\256\276\347\275\256"); lv_obj_set_style_text_color(ti,lv_color_hex(0xffffff),0); lv_obj_align(ti,LV_ALIGN_TOP_MID,0,8);
    mk_label(setscr,"WiFi: \350\257\267\345\234\250\344\273\243\347\240\201\344\270\255\351\205\215\347\275\256"); lv_obj_set_style_text_color(lv_obj_get_child(setscr,-1),lv_color_hex(0x888888),0); lv_obj_align(lv_obj_get_child(setscr,-1),LV_ALIGN_TOP_LEFT,10,45);
    mk_label(setscr,"Server: ws://IP:8888"); lv_obj_set_style_text_color(lv_obj_get_child(setscr,-1),lv_color_hex(0x888888),0); lv_obj_align(lv_obj_get_child(setscr,-1),LV_ALIGN_TOP_LEFT,10,75);
    mk_label(setscr,"DDL Reminder v1.0.0"); lv_obj_set_style_text_color(lv_obj_get_child(setscr,-1),lv_color_hex(0x555555),0); lv_obj_align(lv_obj_get_child(setscr,-1),LV_ALIGN_BOTTOM_MID,0,-15);
}

// public methods
void UIManager::show_reminder(const DDLEvent& e) { lv_label_set_text(rtitle,(e.tag+" "+e.title).c_str()); lv_label_set_text(rdeadline,("\346\210\252\346\255\242: "+e.deadline).c_str()); lv_obj_clear_flag(popup,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(popup); scr_=REMINDER_POPUP; }
void UIManager::update_ddl_list(const std::vector<DDLEvent>& ev) {
    if(ev.size()>0){ auto& e=ev[0]; std::string t=e.tag+" "+e.title+" ("+e.deadline.substr(5,11)+")"; lv_label_set_text((lv_obj_t*)lv_obj_get_child(card1,0),t.c_str()); lv_obj_clear_flag(card1,LV_OBJ_FLAG_HIDDEN); } else lv_obj_add_flag(card1,LV_OBJ_FLAG_HIDDEN);
    if(ev.size()>1){ auto& e=ev[1]; std::string t=e.tag+" "+e.title+" ("+e.deadline.substr(5,11)+")"; lv_label_set_text((lv_obj_t*)lv_obj_get_child(card2,0),t.c_str()); lv_obj_clear_flag(card2,LV_OBJ_FLAG_HIDDEN); } else lv_obj_add_flag(card2,LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(elist);
    for(auto& e:ev){ std::string t=e.tag+" "+e.course+" - "+e.title; lv_obj_t* b=lv_list_add_btn(elist,NULL,t.c_str()); lv_obj_add_event_cb(b,cb_list_click,LV_EVENT_CLICKED,(void*)&e); }
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

void UIManager::show_detail(const DDLEvent& ev) {
    detail_id_=ev.id;
    lv_label_set_text(dtitle,ev.title.c_str());
    lv_label_set_text(dcourse,ev.course.c_str());
    lv_label_set_text(ddeadline,("\346\210\252\346\255\242: "+ev.deadline).c_str());
    int m=ev.minutes_remaining; char cd[48];
    if(m<0) snprintf(cd,sizeof(cd),"\345\267\262\350\277\207\346\234\237 %d \345\210\206\351\222\237",-m);
    else if(m<60) snprintf(cd,sizeof(cd),"\345\211\251\344\275\231 %d \345\210\206\351\222\237",m);
    else snprintf(cd,sizeof(cd),"\345\211\251\344\275\231 %d \345\260\217\346\227\266 %d \345\210\206\351\222\237",m/60,m%60);
    lv_label_set_text(dcountdown,cd); show_screen(DETAIL);
}

// callbacks
void UIManager::cb_mic_press(lv_event_t*) { instance().set_emotion("neutral"); if(g_on_mic_press)g_on_mic_press(); }
void UIManager::cb_mic_release(lv_event_t*) { instance().set_emotion("thinking"); if(g_on_mic_release)g_on_mic_release(); }
void UIManager::cb_view_all(lv_event_t*) { instance().show_screen(LIST); }
void UIManager::cb_list_click(lv_event_t* e) {
    auto* ev=(const DDLEvent*)lv_event_get_user_data(e); if(!ev) return;
    instance().show_detail(*ev);
}
void UIManager::cb_back(lv_event_t*) { instance().show_screen(MAIN); }
void UIManager::cb_done(lv_event_t*) { if(g_on_event_action&&!instance().detail_id_.empty())g_on_event_action("done"); instance().show_screen(MAIN); }
void UIManager::cb_snooze(lv_event_t*) { if(g_on_event_action&&!instance().detail_id_.empty())g_on_event_action("snooze"); instance().show_screen(MAIN); }
void UIManager::cb_settings(lv_event_t*) { instance().show_screen(SETTINGS); }
