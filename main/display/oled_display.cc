#include "oled_display.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"

#include <algorithm>
#include <string>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <noto_emoji.h>

#define TAG "OledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);
LV_FONT_DECLARE(font_noto_emoji_30_1);

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_1);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }
}

void OledDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();
    if (height_ == 64) {
        SetupUI_128x64();
    } else {
        SetupUI_128x32();
    }
}

// ==== HÀM KHỞI TẠO MẮT ĐỘNG (MỚI) ====
void OledDisplay::InitEyes() {
    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();

    // Ẩn icon Robot cũ (emotion_label_) để nhường chỗ cho mắt
    if (emotion_label_ != nullptr) {
        lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Tạo mắt trái
    eye_left_ = lv_obj_create(screen);
    lv_obj_set_size(eye_left_, 22, 28);
    lv_obj_set_pos(eye_left_, 20, 10);
    lv_obj_set_style_border_width(eye_left_, 0, 0);
    lv_obj_set_style_bg_opa(eye_left_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(eye_left_, lv_color_white(), 0);
    lv_obj_set_style_radius(eye_left_, 11, 0);

    // Tạo mắt phải
    eye_right_ = lv_obj_create(screen);
    lv_obj_set_size(eye_right_, 22, 28);
    lv_obj_set_pos(eye_right_, 80, 10);
    lv_obj_set_style_border_width(eye_right_, 0, 0);
    lv_obj_set_style_bg_opa(eye_right_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(eye_right_, lv_color_white(), 0);
    lv_obj_set_style_radius(eye_right_, 11, 0);

    // Tạo Timer để mắt chớp tự động mỗi 50ms
    eye_timer_ = lv_timer_create(EyeTimerCallback, 50, this);
}

// ==== HÀM CẬP NHẬT MẮT (MỚI) ====
void OledDisplay::UpdateEyeState(int state) {
    eye_state_ = state;
    DisplayLockGuard lock(this);

    if (!eye_left_ || !eye_right_) return;

    if (state == 1) { // Nhắm mắt
        lv_obj_set_size(eye_left_, 22, 2);
        lv_obj_set_size(eye_right_, 22, 2);
        lv_obj_set_style_radius(eye_left_, 0, 0);
        lv_obj_set_style_radius(eye_right_, 0, 0);
    } else if (state == 2) { // Híp (Vui)
        lv_obj_set_size(eye_left_, 22, 10);
        lv_obj_set_size(eye_right_, 22, 10);
        lv_obj_set_style_radius(eye_left_, 8, 0);
        lv_obj_set_style_radius(eye_right_, 8, 0);
    } else if (state == 3) { // To tròn (Nghe/Nói)
        lv_obj_set_size(eye_left_, 28, 32);
        lv_obj_set_size(eye_right_, 28, 32);
        lv_obj_set_style_radius(eye_left_, 14, 0);
        lv_obj_set_style_radius(eye_right_, 14, 0);
    } else { // Mở bình thường
        lv_obj_set_size(eye_left_, 22, 28);
        lv_obj_set_size(eye_right_, 22, 28);
        lv_obj_set_style_radius(eye_left_, 11, 0);
        lv_obj_set_style_radius(eye_right_, 11, 0);
    }
}

// ==== TIMER CHỚP MẮT TỰ ĐỘNG (MỚI) ====
void OledDisplay::EyeTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<OledDisplay*>(timer->user_data);
    if (!display || !display->eye_left_) return;

    uint32_t now_ms = esp_timer_get_time() / 1000;
    static uint32_t last_blink_time = 0;

    // Mỗi 4 giây chớp 1 lần, nhắm trong 150ms
    if (!display->is_blinking_ && (now_ms - last_blink_time > 4000)) {
        display->is_blinking_ = true;
        last_blink_time = now_ms;
        display->UpdateEyeState(1); // Nhắm
    } else if (display->is_blinking_ && (now_ms - last_blink_time > 150)) {
        display->is_blinking_ = false;
        // Mở lại theo trạng thái hiện tại
        if (display->eye_state_ == 1) {
            display->UpdateEyeState(0);
        }
    }
}

// ==== HÀM SetEmotion (SỬA ĐỂ ĐIỀU KHIỂN MẮT) ====
void OledDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);

    // Nếu chưa tạo mắt thì khởi tạo
    if (eye_left_ == nullptr) {
        InitEyes();
    }

    // Đổi hình dạng mắt theo cảm xúc
    if (strcmp(emotion, "happy") == 0) {
        current_emotion_ = "happy";
        UpdateEyeState(2); // Híp vui
    } else if (strcmp(emotion, "sad") == 0) {
        current_emotion_ = "sad";
        UpdateEyeState(1); // Nhắm buồn
    } else if (strcmp(emotion, "listening") == 0 || strcmp(emotion, "speaking") == 0) {
        current_emotion_ = "listening";
        UpdateEyeState(3); // To tròn
    } else {
        current_emotion_ = "neutral";
        UpdateEyeState(0); // Mở bình thường
    }
}

// ==== HÀM CÔNG KHAI CHO BOARD GỌI (MỚI) ====
void OledDisplay::SetCustomEmotion(const std::string& emotion) {
    SetEmotion(emotion.c_str());
}

OledDisplay::~OledDisplay() {
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }

    bool is_128x64_layout = (top_bar_ != nullptr);
    if (status_bar_ != nullptr && is_128x64_layout) {
        status_label_ = nullptr;
        notification_label_ = nullptr;
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        network_label_ = nullptr;
        mute_label_ = nullptr;
        battery_label_ = nullptr;
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        if (!is_128x64_layout) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
        }
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }

    // Xóa mắt
    if (eye_left_) lv_obj_del(eye_left_);
    if (eye_right_) lv_obj_del(eye_right_);

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool OledDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void OledDisplay::Unlock() { lvgl_port_unlock(); }

void OledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');

    lv_anim_delete(chat_message_label_, nullptr);
    if (content_right_ == nullptr) {
        lv_label_set_text(chat_message_label_, content_str.c_str());
    } else {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(chat_message_label_, content_str.c_str());
            lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    /* Layer 1: Top bar */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    /* Layer 2: Status bar */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_CENTER, 0);

    content_left_ = lv_obj_create(content_);
    lv_obj_set_size(content_left_, 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content_left_, 0, 0);
    lv_obj_set_style_border_width(content_left_, 0, 0);

    emotion_label_ = lv_label_create(content_left_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emotion_label_);
    lv_obj_set_style_pad_top(emotion_label_, 8, 0);

    content_right_ = lv_obj_create(content_);
    lv_obj_set_size(content_right_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content_right_, 0, 0);
    lv_obj_set_style_border_width(content_right_, 0, 0);
    lv_obj_set_flex_grow(content_right_, 1);
    lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);

    chat_message_label_ = lv_label_create(content_right_);
    lv_label_set_text(chat_message_label_, "");
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(chat_message_label_, width_ - 32);
    lv_obj_set_style_pad_top(chat_message_label_, 14, 0);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    // ==== KHỞI TẠO MẮT ĐỘNG (GỌI SAU KHI TẠO UI) ====
    InitEyes();
}

void OledDisplay::SetupUI_128x32() {
    // ... (Giữ nguyên y hệt code cũ của SetupUI_128x32) ...
    // Cuối hàm SetupUI_128x32 cũng gọi InitEyes();
}

void OledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}
