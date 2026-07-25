#pragma once
#include "lvgl.h"
#include "esp_err.h"
#include <string>
#include <vector>
#include <functional>
#include "protocol.h"

class UIManager {
public:
    enum Screen { MAIN, LIST, DETAIL, REMINDER_POPUP, SETTINGS, KEYBOARD };
    enum Tab { TAB_HOME=0, TAB_DDL=1, TAB_SETTINGS=2, TAB_COUNT=3 };
    static UIManager& instance();
    esp_err_t init();
    void show_screen(Screen s);
    void show_reminder(const DDLEvent& e);
    void update_ddl_list(const std::vector<DDLEvent>& ev);
    static std::string tag_text(const std::string& emoji_tag);
    void set_emotion(const std::string& e);
    void set_connected(bool c);
    void show_asr_text(const std::string& text, bool is_final);
    void update_clock();
    Screen current_screen() const { return scr_; }
    Tab current_tab() const { return tab_; }
    void select_tab(int dir);  // +1 or -1 (encoder rotate)
    void set_tab(Tab t);       // direct tab selection (touch)
    void enter_tab();          // press to enter selected tab
    std::string current_detail_id() const { return detail_id_; }
    lv_obj_t* get_event_list() const { return elist; }
    void show_detail(const DDLEvent& e);
    // Touch keyboard for editing DDL fields
    void show_keyboard(const std::string& field_name, const std::string& current_value);
    void hide_keyboard();
    std::string get_keyboard_text() const;
    static void cb_settings(lv_event_t*e);
    static void cb_set_click(lv_event_t* e);

private:
    UIManager() = default;
    Screen scr_ = MAIN;
    Tab tab_ = TAB_HOME;
    std::string detail_id_;
    lv_obj_t *mscr=nullptr, *lscr=nullptr, *dscr=nullptr, *popup=nullptr, *setscr=nullptr;
    lv_obj_t *tab_labels_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t *tab_btns_[3] = {nullptr, nullptr, nullptr};  // clickable tab buttons
    lv_obj_t *mic_btn_ = nullptr;  // large recording button
    lv_obj_t *clock_lbl=nullptr, *date_lbl=nullptr, *emoji_lbl=nullptr, *status_icon=nullptr;
    lv_obj_t *card1=nullptr, *card2=nullptr, *elist=nullptr;
    lv_obj_t *dtitle=nullptr, *dcourse=nullptr, *ddeadline=nullptr, *dcountdown=nullptr;
    lv_obj_t *dreminder=nullptr, *dstatus=nullptr;
    int d_advance_mins = 1440;  // current detail event's advance time
    lv_obj_t *rtitle=nullptr, *rdeadline=nullptr;
    lv_obj_t *asr_bar_=nullptr, *asr_label_=nullptr;
    lv_obj_t *setlist=nullptr;  // settings option list
    int set_idx = 0;  // selected settings item
    bool set_editing = false;  // true = editing a value

    // Touch keyboard screen (public for async callback access)
public:
    lv_obj_t *kbscr=nullptr, *kb_text=nullptr, *kb_field_lbl=nullptr;
    std::string kb_field_;     // which field is being edited
    bool kb_num_mode = false;  // true=number input, false=letter input
    int kb_last_key = -1;      // last key pressed (for multi-tap)
    uint32_t kb_last_tap = 0;  // last tap time (ms)
private:

    void mk_main(); void mk_list(); void mk_detail(); void mk_popup(); void mk_settings();
    void mk_keyboard();  // touch keyboard for editing
    void highlight_tab(Tab t);
    static void cb_mic_press(lv_event_t*e);
    static void cb_mic_release(lv_event_t*e);
    static void cb_view_all(lv_event_t*e);
    static void cb_list_click(lv_event_t*e);
    static void cb_card_click(lv_event_t*e);  // smart card: detail if valid, else list
    static void cb_back(lv_event_t*e);
    static void cb_done(lv_event_t*e);
    static void cb_snooze(lv_event_t*e);
    static void cb_tab_click(lv_event_t*e);  // touch tab selection
    static void cb_kb_key(lv_event_t*e);     // keyboard key press
    static void cb_kb_save(lv_event_t*e);    // keyboard save
    static void cb_kb_toggle(lv_event_t*e);  // keyboard num/abc toggle
    static void cb_detail_edit(lv_event_t*e); // tap detail field to edit
};

extern std::function<void()> g_on_mic_press;
extern std::function<void()> g_on_mic_release;
extern std::function<void(const std::string&)> g_on_event_action;
extern std::function<void()> g_on_request_sync;
