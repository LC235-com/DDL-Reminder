#pragma once
#include "lvgl.h"
#include "esp_err.h"
#include <string>
#include <vector>
#include <functional>
#include "protocol.h"

class UIManager {
public:
    enum Screen { MAIN, LIST, DETAIL, REMINDER_POPUP, SETTINGS };
    static UIManager& instance();
    esp_err_t init();
    void show_screen(Screen s);
    void show_reminder(const DDLEvent& e);
    void update_ddl_list(const std::vector<DDLEvent>& ev);
    void set_emotion(const std::string& e);
    void set_connected(bool c);
    void update_clock();
    Screen current_screen() const { return scr_; }
    std::string current_detail_id() const { return detail_id_; }
    lv_obj_t* get_event_list() const { return elist; }
    void show_detail(const DDLEvent& e);

private:
    UIManager() = default;
    Screen scr_ = MAIN;
    std::string detail_id_;
    lv_obj_t *mscr=nullptr, *lscr=nullptr, *dscr=nullptr, *popup=nullptr, *setscr=nullptr;
    lv_obj_t *clock_lbl=nullptr, *date_lbl=nullptr, *emoji_lbl=nullptr, *status_icon=nullptr;
    lv_obj_t *card1=nullptr, *card2=nullptr, *elist=nullptr;
    lv_obj_t *dtitle=nullptr, *dcourse=nullptr, *ddeadline=nullptr, *dcountdown=nullptr;
    lv_obj_t *rtitle=nullptr, *rdeadline=nullptr;

    void mk_main(); void mk_list(); void mk_detail(); void mk_popup(); void mk_settings();
    static void cb_mic_press(lv_event_t*e);
    static void cb_mic_release(lv_event_t*e);
    static void cb_view_all(lv_event_t*e);
    static void cb_list_click(lv_event_t*e);
    static void cb_back(lv_event_t*e);
    static void cb_done(lv_event_t*e);
    static void cb_snooze(lv_event_t*e);
    static void cb_settings(lv_event_t*e);
};

extern std::function<void()> g_on_mic_press;
extern std::function<void()> g_on_mic_release;
extern std::function<void(const std::string&)> g_on_event_action;
extern std::function<void()> g_on_request_sync;
