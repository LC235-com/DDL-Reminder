#pragma once
#include "lvgl.h"
#include "esp_err.h"
#include <string>
#include <vector>
#include <functional>
#include "protocol.h"

class UIManager {
public:
    enum Screen { MAIN, LIST, DETAIL, REMINDER_EDITOR, TEXT_DETAIL,
                  REMINDER_POPUP, SETTINGS, KEYBOARD };
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
    void encoder_rotate(int direction); // +1 clockwise, -1 counter-clockwise
    void encoder_press();
    std::string current_detail_id() const { return detail_id_; }
    bool is_editing_detail() const;
    lv_obj_t* get_event_list() const { return elist; }
    void show_detail(const DDLEvent& e);
    void refresh_detail_countdown(const DDLEvent& e);
    void request_ddl_refresh();
    void notify_ddl_sync(bool success);
    void show_tool_result(const std::string& tool, bool success, const std::string& message);
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
    lv_obj_t *mscr=nullptr, *lscr=nullptr, *dscr=nullptr, *reminderscr=nullptr;
    lv_obj_t *textscr=nullptr, *popup=nullptr, *setscr=nullptr;
    lv_obj_t *tab_labels_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t *tab_btns_[3] = {nullptr, nullptr, nullptr};  // clickable tab buttons
    lv_obj_t *mic_btn_ = nullptr;  // large recording button
    lv_obj_t *clock_lbl=nullptr, *date_lbl=nullptr, *emoji_lbl=nullptr, *status_icon=nullptr;
    lv_obj_t *avatar_=nullptr, *avatar_hair_=nullptr, *avatar_left_eye_=nullptr;
    lv_obj_t *avatar_right_eye_=nullptr, *avatar_mouth_=nullptr;
    lv_obj_t *card1=nullptr, *card2=nullptr, *elist=nullptr;
    lv_obj_t *list_bucket_label_=nullptr, *list_slider_=nullptr;
    std::vector<lv_obj_t*> list_items_;
    std::vector<size_t> list_event_indices_;
    std::vector<DDLEvent> list_events_;
    std::vector<int> list_buckets_;
    lv_obj_t *dtitle=nullptr, *dcourse=nullptr, *ddeadline=nullptr, *dcountdown=nullptr;
    lv_obj_t *dreminder=nullptr, *dstatus=nullptr;
    int d_advance_mins = 1440;  // current detail event's advance time
    lv_obj_t *rtitle=nullptr, *rdeadline=nullptr;
    lv_obj_t *asr_bar_=nullptr, *asr_label_=nullptr;
    lv_obj_t *textdetail_label_=nullptr;
    std::string asr_full_text_;
    std::string conversation_text_;
    std::vector<std::string> conversation_entries_;
    lv_obj_t *reminder_list_=nullptr, *reminder_hint_=nullptr;
    lv_obj_t *refresh_toast_=nullptr, *refresh_toast_label_=nullptr;
    lv_timer_t *refresh_timer_=nullptr;
    bool refresh_pending_=false;
    lv_obj_t *tool_toast_=nullptr, *tool_toast_title_=nullptr, *tool_toast_message_=nullptr;
    lv_timer_t *tool_toast_timer_=nullptr;
    std::vector<int> reminder_values_;
    int reminder_idx_ = 0;
    int reminder_field_ = 0;  // 0=days, 1=hours, 2=minutes
    bool reminder_editing_ = false;
    lv_obj_t *setlist=nullptr;  // settings option list
    int set_idx = 0;  // selected settings item
    bool set_editing = false;  // true = editing a value
    int volume_ = 8;
    int list_idx_ = 0;
    int list_count_ = 0;
    int list_window_start_ = 0;
    enum DigitEditMode { EDIT_NONE, EDIT_DEADLINE, EDIT_REMINDER } digit_edit_mode_ = EDIT_NONE;
    std::string digit_edit_value_;
    std::vector<size_t> digit_positions_;
    size_t digit_cursor_ = 0;

    // Touch keyboard screen (public for async callback access)
public:
    lv_obj_t *kbscr=nullptr, *kb_text=nullptr, *kb_field_lbl=nullptr;
    std::string kb_field_;     // which field is being edited
    bool kb_num_mode = false;  // true=number input, false=letter input
    int kb_last_key = -1;      // last key pressed (for multi-tap)
    uint32_t kb_last_tap = 0;  // last tap time (ms)
private:

    void mk_main(); void mk_list(); void mk_detail(); void mk_reminder_editor();
    void mk_text_detail(); void mk_popup(); void mk_settings(); void mk_tool_toast();
    void mk_keyboard();  // touch keyboard for editing
    void highlight_tab(Tab t);
    void highlight_list_item();
    void open_reminder_editor();
    void refresh_reminder_editor();
    void adjust_reminder_field(int direction);
    void commit_reminders(bool rebuild=true);
    void refresh_settings();
    void begin_digit_edit(DigitEditMode mode, const std::string& value);
    void rotate_digit(int direction);
    void advance_or_commit_digit_edit();
    void commit_digit_edit();
    void render_digit_edit();
    void update_detail_countdown_label(const DDLEvent& e);
    static void cb_mic_press(lv_event_t*e);
    static void cb_mic_release(lv_event_t*e);
    static void cb_view_all(lv_event_t*e);
    static void cb_list_click(lv_event_t*e);
    static void cb_list_slider(lv_event_t*e);
    static void cb_card_click(lv_event_t*e);  // smart card: detail if valid, else list
    static void cb_back(lv_event_t*e);
    static void cb_done(lv_event_t*e);
    static void cb_snooze(lv_event_t*e);
    static void cb_tab_click(lv_event_t*e);  // touch tab selection
    static void cb_kb_key(lv_event_t*e);     // keyboard key press
    static void cb_kb_save(lv_event_t*e);    // keyboard save
    static void cb_kb_toggle(lv_event_t*e);  // keyboard num/abc toggle
    static void cb_detail_edit(lv_event_t*e); // tap detail field to edit
    static void cb_open_text_detail(lv_event_t*e);
    static void cb_reminder_add(lv_event_t*e);
    static void cb_reminder_edit(lv_event_t*e);
    static void cb_reminder_delete(lv_event_t*e);
    static void cb_reminder_back(lv_event_t*e);
    static void cb_refresh_timer(lv_timer_t* timer);
    static void cb_tool_toast_timer(lv_timer_t* timer);
    static void async_refresh_reminder_editor(void* data);
};

extern std::function<void()> g_on_mic_press;
extern std::function<void()> g_on_mic_release;
extern std::function<void(const std::string&)> g_on_event_action;
extern std::function<void()> g_on_request_sync;
extern std::function<void(int)> g_on_volume_changed;
