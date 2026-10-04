/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_menu.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_text.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"

static const char *TAG = "muse_menu";

#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_ACCENT 0xa77dff
#define COLOR_SELECTED 0x2e2552
#define COLOR_RULE 0x2a2345
#define COLOR_DANGER 0xff5c5c

/* Hosted from this repository with GitHub Pages. Web Bluetooth requires HTTPS. */
#define BLUETOOTH_SETUP_URL "https://iamhisu.github.io/muse-gadget-sdk/esp32/tools/muse/ble_setup.html"

/* 128 px screens want a smaller face than the 14 px every board has. */
#if LV_FONT_MONTSERRAT_12
#define FONT_COMPACT (&lv_font_montserrat_12)
#else
#define FONT_COMPACT (&lv_font_montserrat_14)
#endif

#define IDLE_CLOSE_S 30.0f      /* back to the face if left alone */
#define REFRESH_S 0.25f

/*
 * One list, stepped through with Up/Down and acted on with Select. Settings
 * cycle through a few values on each Select (wrapping around), so nothing
 * needs a long press or a third button.
 */
typedef enum {
    ITEM_VOLUME,
    ITEM_SPEAKER,
    ITEM_BRIGHTNESS,
    ITEM_MIC,
    ITEM_SLEEP,
    ITEM_PHONE,
    ITEM_WIFI,
    ITEM_INFO,
    ITEM_BATTERY,
    ITEM_RESET,
    ITEM_SLEEP_NOW,
    ITEM_POWER,
    ITEM_CLOSE,
    ITEM_COUNT,
} item_t;

static const char *const ITEM_NAMES[ITEM_COUNT] = {
    [ITEM_VOLUME] = "Volume",
    [ITEM_SPEAKER] = "Speaker",
    [ITEM_BRIGHTNESS] = "Brightness",
    [ITEM_MIC] = "Mic gain",
    [ITEM_SLEEP] = "Auto-sleep",
    [ITEM_PHONE] = "Bluetooth setup",
    [ITEM_WIFI] = "Wi-Fi",
    [ITEM_INFO] = "Status",
    [ITEM_BATTERY] = "Battery",
    [ITEM_RESET] = "Reset pairing",
    [ITEM_SLEEP_NOW] = "Screen off",
    [ITEM_POWER] = "Power off",
    [ITEM_CLOSE] = "Close menu",
};

/* What the talk button does on each row. */
static const char *const ITEM_ACTIONS[ITEM_COUNT] = {
    [ITEM_VOLUME] = "Change",
    [ITEM_SPEAKER] = "Toggle",
    [ITEM_BRIGHTNESS] = "Change",
    [ITEM_MIC] = "Change",
    [ITEM_SLEEP] = "Change",
    [ITEM_PHONE] = "Open",
    [ITEM_WIFI] = "Open",
    [ITEM_INFO] = "Open",
    [ITEM_BATTERY] = "Open",
    [ITEM_RESET] = "Select",
    [ITEM_SLEEP_NOW] = "Select",
    [ITEM_POWER] = "Select",
    [ITEM_CLOSE] = "Close",
};

/* Ascending; Select moves to the next one and wraps. */
static const int VOLUME_STEPS[] = { 10, 25, 40, 55, 70, 85, 100 };
static const int BRIGHT_STEPS[] = { 10, 25, 50, 75, 100 };
static const int GAIN_STEPS[] = { 0, 6, 12, 18, 24, 30, 36 };
static const int SLEEP_STEPS[] = { 0, 30, 60, 120, 300, 600 };
static const char *const SLEEP_NAMES[] = { "Never", "30 s", "1 min", "2 min", "5 min", "10 min" };
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

typedef enum {
    VIEW_CLOSED,
    VIEW_LIST,
    VIEW_BLUETOOTH,
    VIEW_WIFI,
    VIEW_WIFI_PASSWORD,
    VIEW_STATUS,
    VIEW_BATTERY,
    VIEW_POWER,
    VIEW_RESET,
} view_t;

#define MENU_WIFI_MAX 12
#define WIFI_VISIBLE_ROWS 6
#define PASS_COLS 8
#define PASS_ROWS 5
#define PASS_CHAR_COUNT 36
#define PASS_KEY_COUNT (PASS_COLS * PASS_ROWS)
#define PASS_MODE_KEY 36
#define PASS_DELETE_KEY 37
#define PASS_CONNECT_KEY 38
#define PASS_CANCEL_KEY 39

static const char *const PASS_KEYSETS[] = {
    "abcdefghijklmnopqrstuvwxyz0123456789",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
    "!@#$%^&*()-_=+[]{};:,.?/\\|0123456789",
};
static const char *const PASS_MODE_KEYS[] = { "Aa", "#+", "abc" };

static QueueHandle_t s_keys;
static volatile bool s_open;
static view_t s_view;
static int s_sel;
static int s_shown_sel = -1;
static int s_first;             /* top visible row */
static int s_visible_rows;
static int s_row_h;
static float s_last_key;
static float s_next_refresh;
static int64_t s_batt_shown_us;   /* the battery page reads the PM stats, so once a second */
static muse_wifi_ap_t s_wifi_aps[MENU_WIFI_MAX];
static int s_wifi_count;
static int s_wifi_sel;
static int s_wifi_first;
static uint32_t s_wifi_gen;
static char s_join_ssid[MUSE_SSID_MAX + 1];
static char s_password[MUSE_PASS_MAX + 1];
static int s_pass_len;
static int s_pass_mode;
static int s_pass_choice;

static lv_obj_t *s_root;
static lv_obj_t *s_title;
static lv_obj_t *s_list;
static lv_obj_t *s_rows[ITEM_COUNT];
static lv_obj_t *s_values[ITEM_COUNT];
static lv_obj_t *s_page_view;
static lv_obj_t *s_page;        /* status and power-off confirmation text */
static lv_obj_t *s_keyboard;
static lv_obj_t *s_key_cells[PASS_KEY_COUNT];
static lv_obj_t *s_key_labels[PASS_KEY_COUNT];
static lv_obj_t *s_qr;
static lv_obj_t *s_qr_caption;
static lv_obj_t *s_hint_down;
static lv_obj_t *s_hint_select;
static lv_obj_t *s_hint_up;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static int next_step(const int *steps, int n, int cur)
{
    int i = -1;
    while (i + 1 < n && steps[i + 1] <= cur) {
        i++;
    }
    return steps[(i + 1) % n];
}

static const char *sleep_name(int secs)
{
    for (int i = 0; i < COUNT(SLEEP_STEPS); i++) {
        if (SLEEP_STEPS[i] == secs) {
            return SLEEP_NAMES[i];
        }
    }
    return "Custom";
}

static void value_text(int item, char *buf, size_t n)
{
    static const char *const WIFI_VALUES[] = { "Off", "Not set", "Joining", "On", "Failed", "Not found" };
    muse_wifi_status_t w;
    muse_power_t p;

    switch (item) {
    case ITEM_VOLUME:
        snprintf(buf, n, "%d%%", muse_settings_volume());
        break;
    case ITEM_SPEAKER:
        strlcpy(buf, muse_settings_speaker_on() ? "On" : "Off", n);
        break;
    case ITEM_BRIGHTNESS:
        snprintf(buf, n, "%d%%", muse_settings_brightness());
        break;
    case ITEM_MIC:
        snprintf(buf, n, "%d dB", muse_settings_mic_gain());
        break;
    case ITEM_SLEEP:
        strlcpy(buf, sleep_name(muse_settings_sleep_s()), n);
        break;
    case ITEM_PHONE:
        strlcpy(buf, muse_settings_ble_on() ? "On" : "Off", n);
        break;
    case ITEM_WIFI:
        muse_wifi_status(&w);
        strlcpy(buf, WIFI_VALUES[w.state], n);
        break;
    case ITEM_BATTERY:
        p = muse_state_power();
        if (p.usb || p.battery_pct < 0) {
            strlcpy(buf, "USB", n);
        } else {
            snprintf(buf, n, "%d%%", p.battery_pct);
        }
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

static void status_text(char *buf, size_t n)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    muse_ble_status_t b;
    muse_ble_status(&b);
    muse_power_t p = muse_state_power();

    char batt[16] = "USB";
    if (p.battery_pct >= 0) {
        snprintf(batt, sizeof(batt), "%d%%%s", p.battery_pct, p.charging ? " +" : "");
    }
    const char *ble = b.state == MUSE_BLE_OFF ? "Off" : (b.state == MUSE_BLE_CONNECTED ? "Connected" : b.name);
    snprintf(buf, n, "#a77dff Wi-Fi# %s\n#a77dff IP#    %s\n#a77dff Link#  %s\n#a77dff Muse#  %s\n"
             "#a77dff BLE#   %s\n#a77dff Power# %s\n#a77dff Ver#   %s",
             w.state == MUSE_WIFI_CONNECTED ? w.ssid : (w.state == MUSE_WIFI_OFF ? "off" : "offline"),
             w.state == MUSE_WIFI_CONNECTED ? w.ip : "-", muse_link_state_name(muse_link_state()),
             muse_hatch_state_name(h.state), ble, batt,
             esp_app_get_description()->version);
    muse_text_to_ascii(buf, n);   /* network and phone names can have curly quotes */
}

static void pm_text(char out[24], int pm)
{
    if (pm < 0) {
        strlcpy(out, "-", 24);
    } else {
        snprintf(out, 24, "%d.%d%%", pm / 10, pm % 10);
    }
}

/* The battery meter (muse_battery.h), 15 columns wide. */
static void battery_text(char *buf, size_t n)
{
    muse_battery_t b;
    muse_battery_read(&b);
    if (!b.started) {
        strlcpy(buf, "#a77dff Battery test#\nUnplug USB to start\nthe battery runtime test.", n);
        return;
    }
    char t[24], rate[24] = "-", full[24] = "-", wakes[24] = "-", off[24], slept[24], busy[24];
    int h = (int)(b.secs / 3600), m = (int)(b.secs / 60 % 60), rate10, full_h;
    if (h) {
        snprintf(t, sizeof(t), "%dh%02dm", h, m);
    } else {
        snprintf(t, sizeof(t), "%dm", m);
    }
    if (muse_battery_drain(&b, &rate10, &full_h)) {
        snprintf(rate, sizeof(rate), "%d.%d%%/h", rate10 / 10, rate10 % 10);
        snprintf(full, sizeof(full), "~%d h", full_h);
    }
    if (b.secs && b.slept_pm >= 0) {
        int per10 = (int)(b.sleeps * 10LL / b.secs);
        snprintf(wakes, sizeof(wakes), "%d.%d/s", per10 / 10, per10 % 10);
    }
    pm_text(off, b.screen_off_pm);
    pm_text(slept, b.slept_pm);
    pm_text(busy, b.busy_pm);
    snprintf(buf, n, "#a77dff State# %s  %s\n#a77dff Level# %d%% -> %d%%\n"
             "#a77dff Drain# %s | %s\n#a77dff Screen off# %s\n#a77dff Sleeping# %s\n"
             "#a77dff Wake# %s | #a77dff Busy# %s",
             b.running ? "On battery" : "Last run", t, b.pct_start, b.pct_now,
             rate, full, off, slept, wakes, busy);
}

static const char *wifi_state_text(muse_wifi_state_t state)
{
    switch (state) {
    case MUSE_WIFI_CONNECTED: return "Connected";
    case MUSE_WIFI_CONNECTING: return "Joining...";
    case MUSE_WIFI_FAILED: return "Connection failed";
    case MUSE_WIFI_NOT_NEARBY: return "Network not found";
    case MUSE_WIFI_OFF: return "Wi-Fi off";
    default: return muse_wifi_scanning() ? "Scanning..." : "Choose a network";
    }
}

static void wifi_page_text(char *buf, size_t n)
{
    uint32_t gen = 0;
    muse_wifi_ap_t aps[MENU_WIFI_MAX];
    int count = muse_wifi_scan_results(aps, MENU_WIFI_MAX, &gen);
    if (gen != s_wifi_gen) {
        memcpy(s_wifi_aps, aps, count * sizeof(aps[0]));
        s_wifi_count = count;
        s_wifi_gen = gen;
        int total = s_wifi_count + 2; /* Scan again, Back */
        if (s_wifi_sel >= total) {
            s_wifi_sel = total - 1;
        }
    }

    int total = s_wifi_count + 2;
    if (s_wifi_sel < s_wifi_first) {
        s_wifi_first = s_wifi_sel;
    } else if (s_wifi_sel >= s_wifi_first + WIFI_VISIBLE_ROWS) {
        s_wifi_first = s_wifi_sel - WIFI_VISIBLE_ROWS + 1;
    }

    muse_wifi_status_t status;
    muse_wifi_status(&status);
    size_t used = (size_t)snprintf(buf, n, "%s\n", wifi_state_text(status.state));
    for (int row = 0, i = s_wifi_first; row < WIFI_VISIBLE_ROWS && i < total; row++, i++) {
        char line[64];
        if (i < s_wifi_count) {
            const muse_wifi_ap_t *ap = &s_wifi_aps[i];
            snprintf(line, sizeof(line), "%c %-19.19s %4d %c", i == s_wifi_sel ? '>' : ' ',
                     ap->ssid, ap->rssi, ap->secure ? '*' : ' ');
        } else if (i == s_wifi_count) {
            snprintf(line, sizeof(line), "%c Scan again", i == s_wifi_sel ? '>' : ' ');
        } else {
            snprintf(line, sizeof(line), "%c Back", i == s_wifi_sel ? '>' : ' ');
        }
        if (used < n) {
            used += (size_t)snprintf(buf + used, n - used, "%s%s", row ? "\n" : "", line);
        }
    }
    set_text(s_hint_select, s_wifi_sel < s_wifi_count ? "Select" :
             (s_wifi_sel == s_wifi_count ? "Scan" : "Back"));
}

static const char *pass_action(void)
{
    if (s_pass_choice < PASS_CHAR_COUNT) {
        return "Add";
    }
    switch (s_pass_choice) {
    case PASS_MODE_KEY: return "Mode";
    case PASS_DELETE_KEY: return "Delete";
    case PASS_CONNECT_KEY: return "Connect";
    default: return "Cancel";
    }
}

static void password_page_text(char *buf, size_t n)
{
    char masked[29];
    int shown = s_pass_len > 28 ? 28 : s_pass_len;
    memset(masked, '*', shown);
    masked[shown] = '\0';
    if (s_pass_len > shown) {
        strlcpy(masked, "...", sizeof(masked));
        int tail = shown - 3;
        memset(masked + 3, '*', tail);
        masked[3 + tail] = '\0';
    }
    snprintf(buf, n, "%.25s\nPass: %s  %d/%d", s_join_ssid, masked, s_pass_len, MUSE_PASS_MAX);

    char key[4];
    for (int i = 0; i < PASS_KEY_COUNT; i++) {
        if (i < PASS_CHAR_COUNT) {
            key[0] = PASS_KEYSETS[s_pass_mode][i];
            key[1] = '\0';
            set_text(s_key_labels[i], key);
        } else if (i == PASS_MODE_KEY) {
            set_text(s_key_labels[i], PASS_MODE_KEYS[s_pass_mode]);
        } else if (i == PASS_DELETE_KEY) {
            set_text(s_key_labels[i], LV_SYMBOL_BACKSPACE);
        } else if (i == PASS_CONNECT_KEY) {
            set_text(s_key_labels[i], "OK");
        } else {
            set_text(s_key_labels[i], "X");
        }
        lv_obj_set_style_bg_opa(s_key_cells[i], i == s_pass_choice ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    set_text(s_hint_select, pass_action());
}

static void refresh(void)
{
    char buf[512];
    if (s_view == VIEW_LIST) {
        for (int i = 0; i < ITEM_COUNT; i++) {
            value_text(i, buf, sizeof(buf));
            set_text(s_values[i], buf);
        }
        if (s_sel != s_shown_sel) {
            for (int i = 0; i < ITEM_COUNT; i++) {
                lv_obj_set_style_bg_opa(s_rows[i], i == s_sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            }
            /* Scroll by whole rows so none is ever cut in half. */
            if (s_sel < s_first) {
                s_first = s_sel;
            } else if (s_sel >= s_first + s_visible_rows) {
                s_first = s_sel - s_visible_rows + 1;
            }
            lv_obj_scroll_to_y(s_list, s_first * s_row_h, LV_ANIM_OFF);
            set_text(s_hint_select, ITEM_ACTIONS[s_sel]);
            s_shown_sel = s_sel;
        }
    } else if (s_view == VIEW_WIFI) {
        wifi_page_text(buf, sizeof(buf));
        set_text(s_page, buf);
    } else if (s_view == VIEW_WIFI_PASSWORD) {
        password_page_text(buf, sizeof(buf));
        set_text(s_page, buf);
    } else if (s_view == VIEW_STATUS) {
        status_text(buf, sizeof(buf));
        set_text(s_page, buf);
    } else if (s_view == VIEW_BATTERY) {
        int64_t now = esp_timer_get_time();
        if (!s_batt_shown_us || now - s_batt_shown_us >= 1000000) {
            s_batt_shown_us = now;
            battery_text(buf, sizeof(buf));
            set_text(s_page, buf);
        }
    }
}

static void show(view_t view)
{
    s_view = view;
    s_shown_sel = -1;
    s_batt_shown_us = 0;
    lv_obj_set_flag(s_list, LV_OBJ_FLAG_HIDDEN, view != VIEW_LIST);
    lv_obj_set_flag(s_page_view, LV_OBJ_FLAG_HIDDEN, view == VIEW_LIST || view == VIEW_BLUETOOTH);
    lv_obj_set_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN, view != VIEW_WIFI_PASSWORD);
    lv_obj_set_flag(s_qr, LV_OBJ_FLAG_HIDDEN, view != VIEW_BLUETOOTH);
    lv_obj_set_flag(s_qr_caption, LV_OBJ_FLAG_HIDDEN, view != VIEW_BLUETOOTH);
    lv_obj_scroll_to_y(s_page_view, 0, LV_ANIM_OFF);
    lv_label_set_recolor(s_page, false);
    const char *left = LV_SYMBOL_DOWN, *right = LV_SYMBOL_UP;
    if (view == VIEW_WIFI_PASSWORD) {
        left = LV_SYMBOL_RIGHT;
        right = LV_SYMBOL_DOWN;
    } else if (view == VIEW_BLUETOOTH) {
        left = "Off";
        right = "";
    } else if (view == VIEW_POWER) {
        left = "Cancel";
        right = "";
    } else if (view == VIEW_RESET) {
        left = "Back";
        right = "";
    }
    set_text(s_hint_down, left);
    set_text(s_hint_up, right);
    bool danger = view == VIEW_POWER || view == VIEW_RESET;
    lv_obj_set_style_text_color(s_hint_select, lv_color_hex(danger ? COLOR_DANGER : COLOR_TEXT), 0);

    switch (view) {
    case VIEW_BLUETOOTH:
        set_text(s_title, "BLUETOOTH SETUP");
        set_text(s_hint_select, "Back");
        break;
    case VIEW_WIFI:
        lv_obj_set_style_text_font(s_page, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_line_space(s_page, 2, 0);
        lv_label_set_long_mode(s_page, LV_LABEL_LONG_MODE_WRAP);
        set_text(s_title, "WI-FI SETUP");
        break;
    case VIEW_WIFI_PASSWORD:
        lv_obj_set_style_text_font(s_page, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_line_space(s_page, 0, 0);
        lv_label_set_long_mode(s_page, LV_LABEL_LONG_MODE_WRAP);
        set_text(s_title, "WI-FI PASSWORD");
        break;
    case VIEW_STATUS:
    case VIEW_BATTERY:
        lv_obj_set_style_text_font(s_page, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_line_space(s_page, 2, 0);
        lv_label_set_long_mode(s_page, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_recolor(s_page, true);
        set_text(s_title, view == VIEW_STATUS ? "STATUS" : "BATTERY");
        set_text(s_hint_select, "Back");
        break;
    case VIEW_POWER: {
        lv_obj_set_style_text_font(s_page, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_line_space(s_page, 5, 0);
        lv_label_set_long_mode(s_page, LV_LABEL_LONG_MODE_WRAP);
        char text[96];
        snprintf(text, sizeof(text), "Turn Muse off?\n\nPress the %s button to turn it back on.",
                 muse_board->aux_button);
        set_text(s_title, "POWER OFF");
        set_text(s_page, text);
        set_text(s_hint_select, "Power off");
        break;
    }
    case VIEW_RESET:
        lv_obj_set_style_text_font(s_page, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_line_space(s_page, 5, 0);
        lv_label_set_long_mode(s_page, LV_LABEL_LONG_MODE_WRAP);
        set_text(s_title, "RESET PAIRING");
        set_text(s_page, "Forget Wi-Fi and the Muse app pairing, then restart?");
        set_text(s_hint_select, "Reset");
        break;
    default:
        set_text(s_title, "MENU");
        break;
    }
    refresh();
}

static void open_menu(void)
{
    ESP_LOGI(TAG, "open");
    s_sel = s_first = 0;
    lv_obj_move_foreground(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    show(VIEW_LIST);
    s_open = true;
}

static void open_wifi_setup(void)
{
    muse_settings_set_wifi_on(true);
    s_wifi_count = 0;
    s_wifi_sel = 0;
    s_wifi_first = 0;
    s_wifi_gen = UINT32_MAX;
    esp_err_t err = muse_wifi_scan();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Wi-Fi scan: %s", esp_err_to_name(err));
    }
    show(VIEW_WIFI);
}

static void open_wifi_password(const char *ssid)
{
    strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
    memset(s_password, 0, sizeof(s_password));
    s_pass_len = 0;
    s_pass_mode = 0;
    s_pass_choice = 0;
    show(VIEW_WIFI_PASSWORD);
}

static void wifi_select(void)
{
    if (s_wifi_sel < s_wifi_count) {
        muse_wifi_ap_t ap = s_wifi_aps[s_wifi_sel];
        strlcpy(s_join_ssid, ap.ssid, sizeof(s_join_ssid));
        if (ap.secure) {
            open_wifi_password(ap.ssid);
        } else {
            muse_settings_set_wifi(ap.ssid, "");
            show(VIEW_WIFI);
        }
    } else if (s_wifi_sel == s_wifi_count) {
        s_wifi_gen = UINT32_MAX;
        esp_err_t err = muse_wifi_scan();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Wi-Fi rescan: %s", esp_err_to_name(err));
        }
        refresh();
    } else {
        show(VIEW_LIST);
    }
}

static void password_select(void)
{
    if (s_pass_choice < PASS_CHAR_COUNT) {
        if (s_pass_len < MUSE_PASS_MAX) {
            s_password[s_pass_len++] = PASS_KEYSETS[s_pass_mode][s_pass_choice];
            s_password[s_pass_len] = '\0';
        }
    } else {
        switch (s_pass_choice) {
        case PASS_MODE_KEY:
            s_pass_mode = (s_pass_mode + 1) % COUNT(PASS_KEYSETS);
            break;
        case PASS_DELETE_KEY:
            if (s_pass_len > 0) {
                s_password[--s_pass_len] = '\0';
            }
            break;
        case PASS_CONNECT_KEY:
            muse_settings_set_wifi_on(true);
            muse_settings_set_wifi(s_join_ssid, s_password);
            memset(s_password, 0, sizeof(s_password));
            s_pass_len = 0;
            show(VIEW_WIFI);
            return;
        default:
            memset(s_password, 0, sizeof(s_password));
            s_pass_len = 0;
            show(VIEW_WIFI);
            return;
        }
    }
    refresh();
}

static void activate(int item)
{
    ESP_LOGI(TAG, "select %s", ITEM_NAMES[item]);
    switch (item) {
    case ITEM_VOLUME:
        muse_settings_set_volume(next_step(VOLUME_STEPS, COUNT(VOLUME_STEPS), muse_settings_volume()));
        muse_voice_request_chirp();
        break;
    case ITEM_SPEAKER:
        muse_settings_set_speaker_on(!muse_settings_speaker_on());
        break;
    case ITEM_BRIGHTNESS:
        {
            int pct = next_step(BRIGHT_STEPS, COUNT(BRIGHT_STEPS), muse_settings_brightness());
            /* Apply immediately and make the UI's preview state converge on the saved value. */
            muse_ui_preview_brightness(pct);
            muse_settings_set_brightness(pct);
        }
        break;
    case ITEM_MIC:
        muse_settings_set_mic_gain(next_step(GAIN_STEPS, COUNT(GAIN_STEPS), muse_settings_mic_gain()));
        break;
    case ITEM_SLEEP:
        muse_settings_set_sleep_s(next_step(SLEEP_STEPS, COUNT(SLEEP_STEPS), muse_settings_sleep_s()));
        break;
    case ITEM_PHONE:
        muse_settings_set_ble_on(true);
        show(VIEW_BLUETOOTH);
        return;
    case ITEM_WIFI:
        open_wifi_setup();
        return;
    case ITEM_INFO:
        show(VIEW_STATUS);
        return;
    case ITEM_BATTERY:
        show(VIEW_BATTERY);
        return;
    case ITEM_SLEEP_NOW:
        muse_menu_close();
        muse_state_set_asleep(true);
        return;
    case ITEM_RESET:
        show(VIEW_RESET);
        return;
    case ITEM_POWER:
        show(VIEW_POWER);
        return;
    case ITEM_CLOSE:
    default:
        muse_menu_close();
        return;
    }
    refresh();
}

static void scroll_page(int pixels)
{
    lv_obj_update_layout(s_page_view);
    lv_obj_scroll_to_y(s_page_view, lv_obj_get_scroll_y(s_page_view) + pixels, LV_ANIM_ON);
}

static void handle(muse_menu_key_t key)
{
    switch (s_view) {
    case VIEW_CLOSED:
        if (key == MUSE_MENU_OPEN) {
            open_menu();
        }
        break;
    case VIEW_LIST:
        if (key == MUSE_MENU_DOWN) {
            s_sel = (s_sel + 1) % ITEM_COUNT;
            refresh();
        } else if (key == MUSE_MENU_UP) {
            s_sel = (s_sel + ITEM_COUNT - 1) % ITEM_COUNT;
            refresh();
        } else if (key == MUSE_MENU_SELECT) {
            activate(s_sel);
        }
        break;
    case VIEW_STATUS:
    case VIEW_BATTERY:
        if (key == MUSE_MENU_DOWN) {
            scroll_page(32);
        } else if (key == MUSE_MENU_UP) {
            scroll_page(-32);
        } else if (key == MUSE_MENU_SELECT) {
            show(VIEW_LIST);
        }
        break;
    case VIEW_BLUETOOTH:
        if (key == MUSE_MENU_DOWN) {
            muse_settings_set_ble_on(false);
            show(VIEW_LIST);
        } else if (key == MUSE_MENU_SELECT) {
            show(VIEW_LIST);
        }
        break;
    case VIEW_WIFI: {
        int total = s_wifi_count + 2;
        if (key == MUSE_MENU_DOWN) {
            s_wifi_sel = (s_wifi_sel + 1) % total;
            refresh();
        } else if (key == MUSE_MENU_UP) {
            s_wifi_sel = (s_wifi_sel + total - 1) % total;
            refresh();
        } else if (key == MUSE_MENU_SELECT) {
            wifi_select();
        }
        break;
    }
    case VIEW_WIFI_PASSWORD: {
        if (key == MUSE_MENU_DOWN) {
            int row = s_pass_choice / PASS_COLS;
            int col = (s_pass_choice % PASS_COLS + 1) % PASS_COLS;
            s_pass_choice = row * PASS_COLS + col;
            refresh();
        } else if (key == MUSE_MENU_UP) {
            int row = (s_pass_choice / PASS_COLS + 1) % PASS_ROWS;
            int col = s_pass_choice % PASS_COLS;
            s_pass_choice = row * PASS_COLS + col;
            refresh();
        } else if (key == MUSE_MENU_SELECT) {
            password_select();
        }
        break;
    }
    case VIEW_POWER:
        if (key == MUSE_MENU_DOWN) {
            show(VIEW_LIST);
        } else if (key == MUSE_MENU_SELECT) {
            muse_menu_close();
            muse_input_request_power_off();
        }
        break;
    case VIEW_RESET:
        if (key == MUSE_MENU_DOWN) {
            show(VIEW_LIST);
        } else if (key == MUSE_MENU_SELECT) {
            muse_menu_close();
            muse_state_set_caption("RESETTING...");
            muse_link_reset_setup();
        }
        break;
    }
}

void muse_menu_key(muse_menu_key_t key)
{
    if (s_keys) {
        uint8_t k = (uint8_t)key;
        xQueueSend(s_keys, &k, 0);
    }
}

bool muse_menu_is_open(void)
{
    return s_open;
}

void muse_menu_build(lv_obj_t *parent, int w, int h)
{
    bool small = h < 200 || w < 200;
    const lv_font_t *font = small ? FONT_COMPACT : &lv_font_montserrat_20;
    const lv_font_t *fine = small ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
    const lv_font_t *title_font = small ? &lv_font_montserrat_14 : &lv_font_montserrat_20;
    int pad = small ? 2 : 8;
    int title_h = small ? 13 : 40;
    int hint_h = small ? 17 : 44;
    s_row_h = small ? 16 : 36;
    s_visible_rows = (h - title_h - hint_h) / s_row_h;
    int strip = 0;

    s_keys = xQueueCreate(8, sizeof(uint8_t));

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, w, h);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    s_title = label(s_root, title_font, COLOR_ACCENT, "MENU");
    lv_obj_set_style_text_letter_space(s_title, 1, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, small ? 3 : 12);

    s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, w - strip, s_visible_rows * s_row_h);
    lv_obj_set_pos(s_list, 0, title_h);
    lv_obj_set_style_pad_hor(s_list, pad, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < ITEM_COUNT; i++) {
        lv_obj_t *r = lv_obj_create(s_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), s_row_h);
        lv_obj_set_style_pad_hor(r, small ? 3 : 10, 0);
        lv_obj_set_style_radius(r, small ? 3 : 8, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_SELECTED), 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        uint32_t color = i == ITEM_POWER ? COLOR_DANGER : COLOR_TEXT;
        lv_obj_align(label(r, font, color, ITEM_NAMES[i]), LV_ALIGN_LEFT_MID, 0, 0);
        s_values[i] = label(r, font, COLOR_ACCENT, "");
        lv_obj_align(s_values[i], LV_ALIGN_RIGHT_MID, 0, 0);
        s_rows[i] = r;
    }

    s_page_view = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_page_view);
    lv_obj_set_size(s_page_view, w - 4 * pad - strip, h - title_h - hint_h - 2 * pad);
    lv_obj_set_pos(s_page_view, 2 * pad, title_h + pad);
    lv_obj_set_scroll_dir(s_page_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_page_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_page_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_page_view, LV_OBJ_FLAG_HIDDEN);

    s_page = label(s_page_view, fine, COLOR_TEXT, "");
    lv_obj_set_width(s_page, lv_pct(100));
    lv_obj_set_height(s_page, LV_SIZE_CONTENT);
    lv_obj_set_style_text_line_space(s_page, small ? 3 : 8, 0);
    lv_label_set_long_mode(s_page, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(s_page, 0, 0);

    int key_gap = small ? 1 : 2;
    int keyboard_x = small ? 2 : 4;
    int keyboard_y = title_h + (small ? 22 : 39);
    int keyboard_w = w - 2 * keyboard_x;
    int keyboard_h = h - hint_h - keyboard_y - (small ? 1 : 3);
    int key_w = (keyboard_w - (PASS_COLS - 1) * key_gap) / PASS_COLS;
    int key_h = (keyboard_h - (PASS_ROWS - 1) * key_gap) / PASS_ROWS;
    s_keyboard = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_keyboard);
    lv_obj_set_size(s_keyboard, keyboard_w, keyboard_h);
    lv_obj_set_pos(s_keyboard, keyboard_x, keyboard_y);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < PASS_KEY_COUNT; i++) {
        lv_obj_t *key = lv_obj_create(s_keyboard);
        lv_obj_remove_style_all(key);
        lv_obj_set_size(key, key_w, key_h);
        lv_obj_set_pos(key, (i % PASS_COLS) * (key_w + key_gap),
                       (i / PASS_COLS) * (key_h + key_gap));
        lv_obj_set_style_radius(key, small ? 2 : 4, 0);
        lv_obj_set_style_bg_color(key, lv_color_hex(COLOR_SELECTED), 0);
        lv_obj_remove_flag(key, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_key_cells[i] = key;
        s_key_labels[i] = label(key, fine, COLOR_TEXT, "");
        lv_obj_center(s_key_labels[i]);
    }

    /* A phone scans this HTTPS page, then its browser opens the Web Bluetooth
     * device picker. The QR is deliberately large and high contrast for the
     * 240x240 Zhengchen panel. */
    s_qr = lv_qrcode_create(s_root);
    lv_qrcode_set_size(s_qr, small ? 104 : 128);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_qrcode_set_quiet_zone(s_qr, true);
    lv_qrcode_update(s_qr, BLUETOOTH_SETUP_URL, strlen(BLUETOOTH_SETUP_URL));
    lv_obj_set_style_border_color(s_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_qr, small ? 2 : 4, 0);
    lv_obj_align(s_qr, LV_ALIGN_TOP_MID, 0, title_h + (small ? 1 : 2));
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);

    s_qr_caption = label(s_root, fine, COLOR_TEXT, "Scan to connect");
    lv_obj_align(s_qr_caption, LV_ALIGN_TOP_MID, 0, title_h + (small ? 108 : 134));
    lv_obj_add_flag(s_qr_caption, LV_OBJ_FLAG_HIDDEN);

    /* Match the physical Zhengchen order: volume down, BOOT, volume up. */
    lv_obj_t *rule = lv_obj_create(s_root);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, w, 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COLOR_RULE), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, -hint_h);
    s_hint_down = label(s_root, font, COLOR_TEXT, LV_SYMBOL_DOWN);
    lv_obj_align(s_hint_down, LV_ALIGN_BOTTOM_LEFT, pad, -pad);
    s_hint_select = label(s_root, font, COLOR_TEXT, "");
    lv_obj_align(s_hint_select, LV_ALIGN_BOTTOM_MID, 0, -pad);
    s_hint_up = label(s_root, font, COLOR_TEXT, LV_SYMBOL_UP);
    lv_obj_align(s_hint_up, LV_ALIGN_BOTTOM_RIGHT, -pad, -pad);
}

bool muse_menu_tick(float now)
{
    if (!s_root) {
        return false;
    }
    uint8_t k;
    while (xQueueReceive(s_keys, &k, 0) == pdTRUE) {
        s_last_key = now;
        handle((muse_menu_key_t)k);
    }
    if (s_view == VIEW_CLOSED) {
        return false;
    }
    if (now - s_last_key > IDLE_CLOSE_S) {
        muse_menu_close();
        return false;
    }
    if (now >= s_next_refresh) {
        s_next_refresh = now + REFRESH_S;
        refresh();
    }
    return true;
}

void muse_menu_close(void)
{
    if (!s_root || s_view == VIEW_CLOSED) {
        return;
    }
    s_view = VIEW_CLOSED;
    s_open = false;
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
