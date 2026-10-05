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

#include "muse_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "mbedtls/base64.h"
#include "src/draw/lv_image_decoder_private.h"   /* custom decoder */
#include "src/misc/lv_area_private.h"            /* lv_area_intersect, for the ring */

#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_console.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_menu.h"
#include "muse_pixel.h"
#include "muse_settings.h"
#include "muse_settings_ui.h"
#include "muse_state.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif

static const char *TAG = "muse_ui";

#define METER_SEGS 14
#define METER_SEG_PX 9
#define METER_GAP_PX 4
#define RING_RANGE 1000
#define CAPTION_W 256           /* 16 columns of unscii_16, the width reply captions wrap to */
#define CAPTION_LINE_SPACE 2
#define ART_BLANK_ROWS 3        /* Muse's art never reaches the grid's bottom rows */
#define MINI_CELL_PX 2          /* Muse's grid cells over a reply that's read */
#define ANSWER_MS 300           /* Muse making room for a reply, and back */
#define SPEAKER_PX 64
#define SPEAKER_GROW_PX 8       /* how much the speaker button swells while held */
#define SPEAKER_HOLD_MS 400     /* LVGL's long press */

#define COLOR_DIM 0x8b84a8
#define COLOR_CAPTION 0xd8d2ff
#define COLOR_RING_BG 0x140f22
#define COLOR_METER_OFF 0x1d1733
#define COLOR_ACCENT 0xa77dff
#define COLOR_DOT_OFF 0x3a3358
#define COLOR_LIT 0xf2efff
#define SETTINGS_TICK_S 0.25f
#define IDLE_CAPTION_SECS 3.5f

/* Text on 128 px screens, as in the button menu (muse_menu.c). */
#if LV_FONT_MONTSERRAT_12
#define FONT_COMPACT (&lv_font_montserrat_12)
#else
#define FONT_COMPACT (&lv_font_montserrat_14)
#endif

/*
 * Two layouts: the full one (round 466 px or similar) with a progress ring,
 * level meter and settings tile, and a compact one for tiny screens (128 px)
 * where Muse fills the screen and text overlays it.
 */
static int s_w, s_h;
static bool s_small;
static bool s_tall;         /* compact, with room above and below Muse (StickS3) */
static int s_canvas_px;     /* Muse's size on screen */
static int s_dy;            /* full layout: offset from a 466 px tall screen */
static lv_indev_t *s_indev;
static lv_obj_t *s_tv;
static lv_obj_t *s_face;
static lv_obj_t *s_settings;
static lv_obj_t *s_dots[2];
static lv_obj_t *s_bt_icon;
static lv_obj_t *s_bt_slash;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_wifi_slash;
static lv_obj_t *s_status_mic;
static lv_obj_t *s_battery_body;
static lv_obj_t *s_battery_tip;
static lv_obj_t *s_battery_segments[4];
static lv_obj_t *s_battery_charge;
static lv_obj_t *s_cover;
static lv_obj_t *s_pair;
static lv_obj_t *s_pair_code;
static lv_obj_t *s_pair_title;
static lv_obj_t *s_pair_hint;
static lv_obj_t *s_canvas;
static lv_obj_t *s_mic_icon;
static lv_obj_t *s_ring;
static lv_obj_t *s_bar;     /* compact layout's stand-in for the ring */
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_name_lbl;    /* this gadget's own name, to tell it from the next one */
static lv_obj_t *s_caption_lbl;
static lv_obj_t *s_reply_lbl;   /* full layout: the reply's page while answering */
static lv_obj_t *s_meter[METER_SEGS];
static lv_obj_t *s_speaker;
static lv_obj_t *s_speaker_icon;
static lv_obj_t *s_aux_icon;
static lv_obj_t *s_boot_splash;
static lv_image_dsc_t s_boot_gif_dsc;
static lv_obj_t *s_image;   /* display.draw_url, over the face */
#if CONFIG_MUSE_WATCHER_CAMERA
static lv_obj_t *s_camera_hint;
#endif
static lv_image_dsc_t s_image_dsc;  /* its data is set once the image is shown */
/* The download writes the pixels without the display lock, so a big JPEG isn't
 * held up by each frame; this guards the buffer and what changed in it. */
static SemaphoreHandle_t s_image_mutex;
static uint16_t *s_image_buf;
static lv_area_t s_image_area;
static bool s_image_dirty;
static lv_obj_t *s_wallpaper;
static lv_image_dsc_t s_wallpaper_dsc;
static uint8_t *s_wallpaper_data;
static uint8_t *s_wallpaper_pending;
static size_t s_wallpaper_pending_size;
static bool s_wallpaper_pending_gif;
static bool s_wallpaper_clear_pending;
static bool s_ready;

#if LV_USE_GIF
extern const uint8_t boot_gif_start[] asm("_binary_windows_xp_boot_gif_start");
extern const uint8_t boot_gif_end[] asm("_binary_windows_xp_boot_gif_end");
#endif

static float s_level;
static int s_shown_state = -1;
static const char *s_shown_name;
static const char *s_idle_name = "";        /* connectivity is already shown by the top icons */
static int s_shown_lit = -1;
static uint32_t s_shown_accent;
static bool s_meter_visible = true;
static int s_ring_value = -1;
static uint32_t s_caption_version;
static float s_idle_caption_until;
static bool s_caption_low;
static float s_next_power_update;
static float s_next_settings_tick;
static bool s_dark;
static int s_brightness = -1;       /* last applied */
static int s_preview_brightness = -1;
static int s_shown_page = -1;
static int s_shown_speaker = -1;
static muse_mode_t s_last_mode = MUSE_MODE_COUNT;

/*
 * While Muse is thinking or speaking it shrinks to make room for the reply:
 * a little when the reply is heard, over a few lines to follow along, and
 * to the top when it's only read, over a page.
 */
typedef struct {
    int px, y;                /* Muse's size and centre */
    int cols, lines;          /* the reply's page */
    int w, h, top;            /* and where it goes */
    lv_text_align_t align;
    lv_obj_t *hides[4];       /* what it covers */
} answer_layout_t;

enum { ANSWER_HEARD, ANSWER_READ };
static answer_layout_t s_answers[2];
static int s_answer = -1;       /* the layout showing, or -1 */
static int s_page_for = -1;     /* the layout the reply's page is sized for */
static int s_big_y;             /* Muse's centre at full size */
static int s_muse_y;            /* and now */
static int s_from_px, s_from_y, s_to_px, s_to_y;

static const char *const MODE_NAMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = "WAKING UP",
    [MUSE_MODE_IDLE] = "READY",
    [MUSE_MODE_LISTENING] = "LISTENING",
    [MUSE_MODE_THINKING] = "THINKING",
    [MUSE_MODE_SPEAKING] = "SPEAKING",
    [MUSE_MODE_ERROR] = "ERROR",
    [MUSE_MODE_OFF] = "GOODBYE",
};

/*
 * Muse reaches LVGL as an image whose pixels are made on demand: a decoder
 * scales the 64 px grid up one strip of rows at a time, so the full-size frame
 * never sits in RAM and LVGL never runs its (slow) image transform.
 *
 * LVGL splits each refresh into a tile per draw unit and draws them at once,
 * each with its own open decoder, so each one takes its own strip: with one
 * shared strip, a tile blended rows the other had just scaled into it.
 */
#define STRIP_ROWS 16
#define STRIPS LV_DRAW_SW_DRAW_UNIT_CNT

#define MUSE_PIXEL_MAX_PX 512   /* muse_pixel_set_size's cap */
#define DIRTY_RECTS_MAX 8       /* with the meter and labels, well inside LVGL's 32 areas */
#define DIRTY_MERGE_PX 2048     /* pixels that cost about one walk of the widget tree */

static lv_image_dsc_t s_muse_src;
static lv_draw_buf_t *s_strips[STRIPS];
static bool s_strip_busy[STRIPS];
static uint16_t *s_cells;       /* each cell's colour as last invalidated */
static uint16_t *s_cell_row;    /* one screen row of Muse */
static bool s_cells_valid;

static lv_result_t muse_dec_info(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
    (void)dec;
    if (dsc->src != &s_muse_src) {
        return LV_RESULT_INVALID;
    }
    *header = s_muse_src.header;
    return LV_RESULT_OK;
}

/* LVGL holds its decoder lock around open and close. */
static lv_result_t muse_dec_open(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (!s_strip_busy[i]) {
            s_strip_busy[i] = true;
            dsc->user_data = s_strips[i];
            return LV_RESULT_OK;   /* the rest of the work is in get_area */
        }
    }
    return LV_RESULT_INVALID;
}

static lv_result_t muse_dec_get_area(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc,
                                     const lv_area_t *full, lv_area_t *area)
{
    (void)dec;
    int32_t y1 = area->y1 == LV_COORD_MIN ? full->y1 : area->y2 + 1;
    if (y1 > full->y2) {
        return LV_RESULT_INVALID;
    }
    int32_t y2 = LV_MIN(y1 + STRIP_ROWS - 1, full->y2);
    int32_t w = lv_area_get_width(full);
    lv_draw_buf_t *buf = lv_draw_buf_reshape(dsc->user_data, LV_COLOR_FORMAT_RGB565, w, y2 - y1 + 1, LV_STRIDE_AUTO);
    if (!buf) {
        return LV_RESULT_INVALID;
    }
    muse_pixel_scale((uint16_t *)buf->data, buf->header.stride / sizeof(uint16_t), full->x1, full->x2, y1, y2);
    area->x1 = full->x1;
    area->x2 = full->x2;
    area->y1 = y1;
    area->y2 = y2;
    dsc->decoded = buf;
    return LV_RESULT_OK;
}

static void muse_dec_close(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (s_strips[i] == dsc->user_data) {
            s_strip_busy[i] = false;   /* kept for the next open */
        }
    }
}

static void muse_image_init(void)
{
    s_muse_src.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_muse_src.header.cf = LV_COLOR_FORMAT_RGB565;
    s_muse_src.header.w = s_canvas_px;
    s_muse_src.header.h = s_canvas_px;
    s_muse_src.header.stride = s_canvas_px * sizeof(uint16_t);
    s_muse_src.data = (const uint8_t *)&s_muse_src;   /* LVGL skips sources without data */
    muse_pixel_set_size(s_canvas_px);

    for (int i = 0; i < STRIPS; i++) {
        s_strips[i] = lv_draw_buf_create(s_canvas_px, STRIP_ROWS, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        assert(s_strips[i]);
    }
    lv_image_decoder_t *dec = lv_image_decoder_create();
    assert(dec);
    lv_image_decoder_set_info_cb(dec, muse_dec_info);
    lv_image_decoder_set_open_cb(dec, muse_dec_open);
    lv_image_decoder_set_get_area_cb(dec, muse_dec_get_area);
    lv_image_decoder_set_close_cb(dec, muse_dec_close);
    dec->name = "muse";

    s_cells = heap_caps_malloc(MUSE_PX_W * MUSE_PX_H * sizeof(uint16_t), MUSE_BIG_CAPS);
    s_cell_row = heap_caps_malloc(MUSE_PIXEL_MAX_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
    if (!s_cells || !s_cell_row) {
        free(s_cells);
        free(s_cell_row);
        s_cells = s_cell_row = NULL;   /* redraw all of Muse every frame */
    }
}

/*
 * Most frames change only a few of Muse's cells, so only those are redrawn.
 * Each frame reads back one screen pixel of every cell (its first, which the
 * faint grid never dims) and compares it with the last frame's. A row's
 * changed cells make one span, and neighbouring spans merge while the pixels
 * drawn for nothing cost less than LVGL's walk of the widget tree for one more
 * area.
 */
static int32_t cell_px(int cell, int size)
{
    return (cell * size + MUSE_PX_W - 1) / MUSE_PX_W;   /* a cell's first screen pixel */
}

static int32_t rect_cells(const lv_area_t *r)
{
    return lv_area_get_width(r) * lv_area_get_height(r);
}

static void invalidate_muse(void)
{
    if (!s_cells) {
        lv_obj_invalidate(s_canvas);
        return;
    }
    int size = (int)s_muse_src.header.w;
    lv_area_t r[MUSE_PX_H];   /* in cells */
    int n = 0;
    for (int cy = 0; cy < MUSE_PX_H; cy++) {
        int32_t y = cell_px(cy, size);
        muse_pixel_scale(s_cell_row, size, 0, size - 1, y, y);
        uint16_t *last = &s_cells[cy * MUSE_PX_W];
        int x1 = -1, x2 = -1;
        for (int cx = 0; cx < MUSE_PX_W; cx++) {
            uint16_t c = s_cell_row[cell_px(cx, size)];
            if (c != last[cx] || !s_cells_valid) {
                last[cx] = c;
                x1 = x1 < 0 ? cx : x1;
                x2 = cx;
            }
        }
        if (x2 >= 0) {
            r[n++] = (lv_area_t){x1, cy, x2, cy};
        }
    }
    s_cells_valid = true;

    int64_t merge_cells = (int64_t)DIRTY_MERGE_PX * MUSE_PX_W * MUSE_PX_H / (size * size);
    while (n > 1) {
        int best = 0;
        int32_t best_cost = INT32_MAX;
        for (int i = 0; i + 1 < n; i++) {
            lv_area_t j = {LV_MIN(r[i].x1, r[i + 1].x1), r[i].y1, LV_MAX(r[i].x2, r[i + 1].x2), r[i + 1].y2};
            int32_t cost = rect_cells(&j) - rect_cells(&r[i]) - rect_cells(&r[i + 1]);
            if (cost < best_cost) {
                best = i;
                best_cost = cost;
            }
        }
        if (n <= DIRTY_RECTS_MAX && best_cost > merge_cells) {
            break;
        }
        r[best].x1 = LV_MIN(r[best].x1, r[best + 1].x1);
        r[best].x2 = LV_MAX(r[best].x2, r[best + 1].x2);
        r[best].y2 = r[best + 1].y2;
        memmove(&r[best + 1], &r[best + 2], (n - best - 2) * sizeof(r[0]));
        n--;
    }

    lv_area_t at;
    lv_obj_get_coords(s_canvas, &at);
    for (int i = 0; i < n; i++) {
        lv_area_t a = {
            at.x1 + cell_px(r[i].x1, size),
            at.y1 + cell_px(r[i].y1, size),
            at.x1 + cell_px(r[i].x2 + 1, size) - 1,
            at.y1 + cell_px(r[i].y2 + 1, size) - 1,
        };
        lv_obj_invalidate_area(s_canvas, &a);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);

/* A microphone from primitives: LVGL's symbol font has none. */
static lv_obj_t *make_mic(lv_obj_t *parent, int size)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    int stroke = size / 10 > 1 ? size / 10 : 1;

    lv_obj_t *head = lv_obj_create(box);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, size * 3 / 8, size * 9 / 16);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *cradle = lv_arc_create(box);
    lv_obj_set_size(cradle, size * 5 / 8 + stroke, size * 5 / 8 + stroke);
    lv_arc_set_bg_angles(cradle, 0, 180);
    lv_obj_remove_style(cradle, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(cradle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(cradle, 0, 0);
    lv_obj_set_style_arc_opa(cradle, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(cradle, stroke, LV_PART_MAIN);
    lv_obj_align(cradle, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *stem = lv_obj_create(box);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, stroke, size * 7 / 8 - size * 5 / 8);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, size * 5 / 8);

    lv_obj_t *base = lv_obj_create(box);
    lv_obj_remove_style_all(base);
    lv_obj_set_size(base, size * 3 / 8, stroke);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, size * 7 / 8);
    return box;
}

static void set_mic_object_color(lv_obj_t *mic, uint32_t color)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(mic); i++) {
        lv_obj_t *part = lv_obj_get_child(mic, i);
        lv_obj_set_style_bg_color(part, lv_color_hex(color), 0);
        lv_obj_set_style_arc_color(part, lv_color_hex(color), LV_PART_MAIN);
    }
}

static void set_mic_color(uint32_t color)
{
    set_mic_object_color(s_mic_icon, color);
}

/* Icons beside the physical buttons, in place of an instruction caption. */
static void build_button_icons(lv_obj_t *face)
{
    const muse_button_hint_t *t = &muse_board->talk_hint, *a = &muse_board->aux_hint;
    s_mic_icon = make_mic(face, s_tall ? 24 : s_small ? 12 : 26);
    lv_obj_align(s_mic_icon, t->align, t->x, t->y);
    set_mic_color(COLOR_DIM);

    /* Button-only boards open the menu with BOOT, so the idle face needs no
     * menu glyph. Touch boards may still show their physical power button. */
    if (!muse_board->touch || a->align == LV_ALIGN_DEFAULT) {
        return;
    }
    s_aux_icon = make_label(face, s_small ? &lv_font_montserrat_14 : &lv_font_montserrat_28, COLOR_DIM);
    lv_label_set_text(s_aux_icon, LV_SYMBOL_POWER);
    lv_obj_align(s_aux_icon, a->align, a->x, a->y);
}

static void on_canvas_clicked(lv_event_t *e)
{
    (void)e;
    muse_state_make_happy();
}

static const lv_font_t *font_pick(const lv_font_t *full, const lv_font_t *compact)
{
    return s_small ? compact : full;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

static void show_speaker(bool on)
{
    /* Off is the one that stands out, like a lit flashlight button. */
    lv_obj_set_style_bg_color(s_speaker, lv_color_hex(on ? COLOR_METER_OFF : COLOR_LIT), 0);
    lv_obj_set_style_text_color(s_speaker_icon, lv_color_hex(on ? COLOR_DIM : 0x000000), 0);
    lv_label_set_text(s_speaker_icon, on ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    s_shown_speaker = on;
}

static void set_speaker_size(void *obj, int32_t px)
{
    lv_obj_set_size(obj, px, px);
}

/* Swells over the long press, so the toggle lands as it reaches full size. */
static void speaker_grow(bool grow)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_speaker);
    lv_anim_set_exec_cb(&a, set_speaker_size);
    lv_anim_set_values(&a, lv_obj_get_width(s_speaker), SPEAKER_PX + (grow ? SPEAKER_GROW_PX : 0));
    lv_anim_set_duration(&a, grow ? SPEAKER_HOLD_MS : 150);
    lv_anim_start(&a);
}

/* Touch and hold to toggle, like the lock screen's flashlight; a tap only says so. */
static void on_speaker_event(lv_event_t *e)
{
    bool on = muse_settings_speaker_on();
    bool idle = muse_state_mode(NULL) == MUSE_MODE_IDLE;   /* don't cover a reply's captions */
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        speaker_grow(true);
        break;
    case LV_EVENT_LONG_PRESSED:
        muse_settings_set_speaker_on(!on);
        show_speaker(!on);
        if (idle) {
            muse_state_set_caption(on ? "SPEAKER OFF" : "SPEAKER ON");
        }
        break;
    case LV_EVENT_SHORT_CLICKED:
        if (idle) {
            muse_state_set_caption(on ? "HOLD TO MUTE" : "HOLD TO UNMUTE");
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        speaker_grow(false);
        break;
    default:
        break;
    }
}

/* Upper left by the status icons, where a thumb finds it without covering the reply. */
static void build_speaker(lv_obj_t *face, int x, int y)
{
    s_speaker = lv_obj_create(face);
    lv_obj_remove_style_all(s_speaker);
    lv_obj_set_size(s_speaker, SPEAKER_PX, SPEAKER_PX);
    lv_obj_set_style_radius(s_speaker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_speaker, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_speaker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(s_speaker, 12);   /* a fingertip is bigger than the circle */
    static const lv_event_code_t EVENTS[] = { LV_EVENT_PRESSED, LV_EVENT_LONG_PRESSED, LV_EVENT_SHORT_CLICKED,
                                              LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST };
    for (size_t i = 0; i < sizeof(EVENTS) / sizeof(EVENTS[0]); i++) {
        lv_obj_add_event_cb(s_speaker, on_speaker_event, EVENTS[i], NULL);
    }
    s_speaker_icon = make_label(s_speaker, &lv_font_montserrat_28, COLOR_DIM);
    lv_obj_center(s_speaker_icon);
    lv_obj_align(s_speaker, LV_ALIGN_CENTER, x, y);
    show_speaker(muse_settings_speaker_on());
}

static void set_canvas_px(int px)
{
    if ((int)s_muse_src.header.w == px) {
        return;
    }
    s_muse_src.header.w = px;
    s_muse_src.header.h = px;
    s_muse_src.header.stride = px * sizeof(uint16_t);
    muse_pixel_set_size(px);
    lv_image_set_src(s_canvas, &s_muse_src);   /* picks up the new size */
}

static void move_muse_t(void *obj, int32_t t)
{
    (void)obj;
    set_canvas_px(s_from_px + (s_to_px - s_from_px) * t / 256);
    s_muse_y = s_from_y + (s_to_y - s_from_y) * t / 256;
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, s_muse_y);
}

/* Eases Muse to `px` centred at `y`, from wherever it is now. */
static void move_muse(int px, int y)
{
    s_from_px = s_muse_src.header.w;
    s_from_y = s_muse_y;
    s_to_px = px;
    s_to_y = y;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_canvas);
    lv_anim_set_exec_cb(&a, move_muse_t);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, ANSWER_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Switches to answer layout `which`, or back to the usual one for -1. */
static void set_answer(int which)
{
    const answer_layout_t *was = s_answer >= 0 ? &s_answers[s_answer] : NULL;
    const answer_layout_t *l = which >= 0 ? &s_answers[which] : NULL;
    s_answer = which;
    const size_t n = sizeof(s_answers[0].hides) / sizeof(s_answers[0].hides[0]);
    for (size_t i = 0; was && i < n; i++) {
        if (was->hides[i]) {
            lv_obj_remove_flag(was->hides[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (size_t i = 0; l && i < n; i++) {
        if (l->hides[i]) {
            lv_obj_add_flag(l->hides[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!muse_board->round && s_ring) {
        lv_obj_set_flag(s_ring, LV_OBJ_FLAG_HIDDEN, l != NULL);   /* the reply runs past a rectangle's ring */
    }
    if (l) {
        lv_obj_set_size(s_reply_lbl, l->w, l->h);
        lv_obj_align(s_reply_lbl, LV_ALIGN_CENTER, 0, l->top + l->h / 2);
        lv_obj_set_style_text_align(s_reply_lbl, l->align, 0);
    }
    move_muse(l ? l->px : s_canvas_px, l ? l->y : s_big_y);
}

/* Whether a reply `w` px wide fits across the screen `y` px from the centre. */
static bool fits_across(int w, int y, int ring_in)
{
    if (!muse_board->round) {
        return w <= s_w - 16;
    }
    int r = ring_in - 6;
    return w * w / 4 + y * y <= r * r;
}

/* How far down a reply `w` px wide can reach: inside the ring, above the page dots. */
static int reply_bottom(int w, int ring_in)
{
    int bottom = s_h / 2 - 28;
    if (muse_board->round) {
        int r = ring_in - 6;
        int chord = w / 2 < r ? (int)sqrtf((float)(r * r - w * w / 4)) : 0;
        bottom = chord < bottom ? chord : bottom;
    }
    return bottom;
}

static void set_reply_box(answer_layout_t *l, int cols, int lines, int top, int cw, int pitch)
{
    l->cols = cols;
    l->lines = lines;
    l->w = cols * cw;
    l->h = lines * pitch - CAPTION_LINE_SPACE;
    l->top = top;
}

/* The hint icons a layout's reply would cover go while it's up. */
static void add_hides(answer_layout_t *l, int n)
{
    lv_area_t box = {
        .x1 = s_w / 2 - l->w / 2, .y1 = s_h / 2 + l->top,
        .x2 = s_w / 2 + l->w / 2 - 1, .y2 = s_h / 2 + l->top + l->h - 1,
    };
    lv_obj_t *const hints[] = { s_mic_icon, s_aux_icon };
    for (size_t i = 0; i < 2; i++) {
        if (!hints[i]) {
            continue;   /* no aux icon on this board */
        }
        lv_area_t a;
        lv_obj_get_coords(hints[i], &a);
        if (a.x1 <= box.x2 && a.x2 >= box.x1 && a.y1 <= box.y2 && a.y2 >= box.y1) {
            l->hides[n++] = hints[i];
        }
    }
}

/*
 * The answer layouts, both with Muse centred. Heard: Muse a size smaller,
 * where it was if there's room, over three lines at the bottom. Read: Muse
 * small under the status line, and under it and the speaker button the
 * biggest page of reply text that fits inside the ring.
 */
static void build_answer(lv_obj_t *face, int ring_in)
{
    int spk_r = (SPEAKER_PX + SPEAKER_GROW_PX) / 2;
    int spk_x = -s_w / 2 + 8 + spk_r, spk_y = -s_h / 2 + 8 + spk_r;
    if (muse_board->round) {
        spk_y = -ring_in * 5 / 8;
        int d = ring_in - spk_r - 4;   /* just inside the ring, even when swollen */
        spk_x = -(int)sqrtf((float)(d * d - spk_y * spk_y));
    }
    const lv_font_t *font = &lv_font_unscii_16;
    int cw = lv_font_get_glyph_width(font, 'M', ' ');
    int pitch = lv_font_get_line_height(font) + CAPTION_LINE_SPACE;

    answer_layout_t *l = &s_answers[ANSWER_HEARD];
    int cell = s_canvas_px / MUSE_PX_W - 1;
    cell = cell > MINI_CELL_PX ? cell : MINI_CELL_PX;
    l->px = MUSE_PX_W * cell;
    l->y = s_big_y;
    l->align = LV_TEXT_ALIGN_CENTER;
    int h = 3 * pitch - CAPTION_LINE_SPACE;
    int art_bottom = l->y + l->px / 2 - ART_BLANK_ROWS * cell;
    set_reply_box(l, CAPTION_W / cw, 3, reply_bottom(CAPTION_W, ring_in) - h, cw, pitch);
    for (int c = 24; c > l->cols; c--) {   /* wider if it still clears Muse */
        int top = reply_bottom(c * cw, ring_in) - h;
        if (top >= art_bottom + 6 && fits_across(c * cw, top, ring_in)) {
            set_reply_box(l, c, 3, top, cw, pitch);
            break;
        }
    }
    if (l->top < art_bottom + 6) {
        l->y -= art_bottom + 6 - l->top;   /* no room under Muse: it moves up */
    }

    l = &s_answers[ANSWER_READ];
    int status_bottom = 20 + s_dy + 16 - s_h / 2;
    l->px = MUSE_PX_W * MINI_CELL_PX;
    l->y = status_bottom + 2 + l->px / 2;   /* its sparkles clear of the status line */
    l->align = LV_TEXT_ALIGN_LEFT;
    art_bottom = l->y + l->px / 2 - ART_BLANK_ROWS * MINI_CELL_PX;
    int top = (art_bottom > spk_y + spk_r ? art_bottom : spk_y + spk_r) + 8;
    set_reply_box(l, 16, 2, top, cw, pitch);
    /* The widest page isn't the biggest: a round screen narrows towards the bottom. */
    for (int c = 12; c <= 24 && fits_across(c * cw, top, ring_in); c++) {
        int n = (reply_bottom(c * cw, ring_in) - top + CAPTION_LINE_SPACE) / pitch;
        /* A third of the caption spare for characters wider than a byte. */
        while ((c + 1) * n > MUSE_CAPTION_MAX * 2 / 3) {
            n--;
        }
        if (c * n > l->cols * l->lines) {
            set_reply_box(l, c, n, top, cw, pitch);
        }
    }
    ESP_LOGI(TAG, "reply pages: %d x %d heard, %d x %d read", s_answers[ANSWER_HEARD].cols,
             s_answers[ANSWER_HEARD].lines, l->cols, l->lines);

    s_reply_lbl = make_label(face, font, COLOR_CAPTION);
    lv_obj_set_style_text_line_space(s_reply_lbl, CAPTION_LINE_SPACE, 0);
    /* Pages come wrapped to fit; the transcript while thinking doesn't. */
    lv_label_set_long_mode(s_reply_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_remove_flag(s_reply_lbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_reply_lbl, LV_OBJ_FLAG_HIDDEN);

    if (muse_board->touch) {
        build_speaker(face, spk_x, spk_y);
    }

    /* Out of the way while a page is up: the mode name, where Muse was. */
    lv_obj_update_layout(face);
    s_answers[ANSWER_READ].hides[0] = s_state_lbl;
    s_answers[ANSWER_READ].hides[1] = s_name_lbl;   /* the reply takes the top too */
    add_hides(&s_answers[ANSWER_READ], 2);
    add_hides(&s_answers[ANSWER_HEARD], 0);
}

/*
 * A full circle's arc masks every pixel of every row it's asked to draw, hole
 * and corners included, which costs a full-screen redraw (a swipe) several ms.
 * So the ring is drawn a slab of rows at a time, each only as wide as the ring
 * is there: where a slab crosses the hole, a left piece and a right one.
 */
#define RING_SLAB_ROWS 52

static void on_ring_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t clip = layer->_clip_area;
    lv_area_t c;
    lv_obj_get_coords(s_ring, &c);
    int32_t cx = (c.x1 + c.x2) / 2;
    int32_t cy = (c.y1 + c.y2) / 2;
    int32_t out = lv_area_get_width(&c) / 2 + 1;                                     /* with antialiasing */
    int32_t hole = out - 1 - lv_obj_get_style_arc_width(s_ring, LV_PART_MAIN) - 2;   /* without */
    lv_event_stop_processing(e);
    for (int32_t y = LV_MAX(clip.y1, cy - out); y <= LV_MIN(clip.y2, cy + out); y += RING_SLAB_ROWS) {
        int32_t y2 = LV_MIN(y + RING_SLAB_ROWS - 1, clip.y2);
        int32_t near = y <= cy && cy <= y2 ? 0 : LV_MIN(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t far = LV_MAX(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t ow = near < out ? (int32_t)ceilf(sqrtf((float)(out * out - near * near))) : 0;
        int32_t iw = far < hole ? (int32_t)sqrtf((float)(hole * hole - far * far)) : 0;
        lv_area_t pieces[2] = {
            { cx - ow, y, iw ? cx - iw : cx + ow, y2 },
            { cx + iw, y, cx + ow, y2 },
        };
        for (int i = 0; i < (iw ? 2 : 1); i++) {
            if (lv_area_intersect(&layer->_clip_area, &clip, &pieces[i])) {
                lv_obj_event_base(NULL, e);   /* the arc's own drawing */
            }
        }
    }
    layer->_clip_area = clip;
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *face = scr;
    if (muse_board->touch) {
        /* Swipe left from Muse for settings. */
        s_tv = lv_tileview_create(scr);
        lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
        lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
        s_face = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
        /* It never scrolls, but LVGL would size its scrollbars from all its
         * children every time it draws any part of it. */
        lv_obj_set_scrollbar_mode(s_face, LV_SCROLLBAR_MODE_OFF);
        s_settings = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_LEFT);
        face = s_face;
    }

    if (!s_small && muse_board->round) {
        /* Round panels use the bezel as a progress ring. */
        int d = (s_w < s_h ? s_w : s_h) - 8;
        s_ring = lv_arc_create(face);
        lv_obj_set_size(s_ring, d, d);
        lv_obj_center(s_ring);
        lv_arc_set_bg_angles(s_ring, 0, 360);
        lv_arc_set_rotation(s_ring, 270);
        lv_arc_set_range(s_ring, 0, RING_RANGE);
        lv_arc_set_value(s_ring, 0);
        lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(s_ring, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_ring, lv_color_hex(COLOR_RING_BG), LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_ring, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(s_ring, false, LV_PART_INDICATOR);
        lv_obj_add_event_cb(s_ring, on_ring_draw, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, NULL);
    }

    /*
     * Full layout, from the bottom up (offsets from the centre): two caption
     * lines as low as the ring leaves them room, the level meter, then Muse,
     * whose blank bottom rows can tuck in behind the meter.
     */
    int ring_in = (s_w < s_h ? s_w : s_h) / 2 - 10;   /* the ring's inner edge */
    int cap_h = 2 * lv_font_get_line_height(&lv_font_unscii_16) + CAPTION_LINE_SPACE;
    int cap_bottom = 179;                              /* a 466 px circle's; fine for rectangles */
    if (muse_board->round) {
        cap_bottom = (int)sqrtf((float)(ring_in * ring_in - CAPTION_W * CAPTION_W / 4)) - 3;
    }
    int cap_top = cap_bottom - cap_h;
    int meter_y = cap_top - 6 - METER_SEG_PX / 2;
    int art_bottom = meter_y - METER_SEG_PX / 2 - 4;
    s_big_y = s_small ? 0 : art_bottom - (s_canvas_px / 2 - ART_BLANK_ROWS * (s_canvas_px / MUSE_PX_W));

    /* The character. */
    muse_image_init();
    s_canvas = lv_image_create(face);
    lv_image_set_src(s_canvas, &s_muse_src);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, s_big_y);
    s_muse_y = s_big_y;
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_clicked, LV_EVENT_CLICKED, NULL);
    if (s_ring) {
        /* The canvas's black corners reach the bezel; keep the ring on top. */
        lv_obj_move_foreground(s_ring);
    }
    build_button_icons(face);

    /* Top-right status, from left to right: active microphone, Bluetooth,
     * Wi-Fi and battery. Battery stays at the outer edge. */
    lv_obj_t *status = lv_obj_create(face);
    lv_obj_remove_style_all(status);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, 5, 0);
    lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -7, 5);

    s_status_mic = make_mic(status, 14);
    set_mic_object_color(s_status_mic, COLOR_ACCENT);
    lv_obj_add_flag(s_status_mic, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bt_box = lv_obj_create(status);
    lv_obj_remove_style_all(bt_box);
    lv_obj_set_size(bt_box, 16, 16);
    lv_obj_remove_flag(bt_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_bt_icon = make_label(bt_box, &lv_font_montserrat_14, COLOR_DIM);
    lv_obj_center(s_bt_icon);
    lv_label_set_text(s_bt_icon, LV_SYMBOL_BLUETOOTH);
    static lv_point_precise_t bt_slash_points[] = { { 0, 15 }, { 15, 0 } };
    s_bt_slash = lv_line_create(bt_box);
    lv_line_set_points(s_bt_slash, bt_slash_points, 2);
    lv_obj_set_style_line_width(s_bt_slash, 3, 0);
    lv_obj_set_style_line_rounded(s_bt_slash, true, 0);
    lv_obj_set_style_line_color(s_bt_slash, lv_color_hex(0xff554d), 0);

    lv_obj_t *wifi_box = lv_obj_create(status);
    lv_obj_remove_style_all(wifi_box);
    lv_obj_set_size(wifi_box, 18, 16);
    lv_obj_remove_flag(wifi_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_wifi_icon = make_label(wifi_box, &lv_font_montserrat_14, COLOR_DIM);
    lv_obj_center(s_wifi_icon);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    static lv_point_precise_t wifi_slash_points[] = { { 1, 15 }, { 17, 1 } };
    s_wifi_slash = lv_line_create(wifi_box);
    lv_line_set_points(s_wifi_slash, wifi_slash_points, 2);
    lv_obj_set_style_line_width(s_wifi_slash, 3, 0);
    lv_obj_set_style_line_rounded(s_wifi_slash, true, 0);
    lv_obj_set_style_line_color(s_wifi_slash, lv_color_hex(0xff554d), 0);

    lv_obj_t *battery = lv_obj_create(status);
    lv_obj_remove_style_all(battery);
    lv_obj_set_size(battery, 25, 16);
    lv_obj_remove_flag(battery, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_battery_body = lv_obj_create(battery);
    lv_obj_remove_style_all(s_battery_body);
    lv_obj_set_size(s_battery_body, 20, 12);
    lv_obj_align(s_battery_body, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_width(s_battery_body, 2, 0);
    lv_obj_set_style_border_color(s_battery_body, lv_color_hex(COLOR_LIT), 0);
    lv_obj_set_style_radius(s_battery_body, 2, 0);
    for (int i = 0; i < 4; i++) {
        s_battery_segments[i] = lv_obj_create(s_battery_body);
        lv_obj_remove_style_all(s_battery_segments[i]);
        lv_obj_set_size(s_battery_segments[i], 3, 8);
        lv_obj_set_pos(s_battery_segments[i], 2 + i * 4, 2);
        lv_obj_set_style_bg_opa(s_battery_segments[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_battery_segments[i], 1, 0);
    }
    s_battery_tip = lv_obj_create(battery);
    lv_obj_remove_style_all(s_battery_tip);
    lv_obj_set_size(s_battery_tip, 3, 6);
    lv_obj_align(s_battery_tip, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_bg_opa(s_battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_battery_tip, 1, 0);
    s_battery_charge = make_label(s_battery_body, &lv_font_montserrat_14, 0xffd54f);
    lv_label_set_text(s_battery_charge, LV_SYMBOL_CHARGE);
    lv_obj_center(s_battery_charge);
    lv_obj_add_flag(s_battery_charge, LV_OBJ_FLAG_HIDDEN);

    /* The compact layout leaves the state to the avatar and the caption,
     * unless the screen is tall enough to fit it in small type above Muse. */
    s_state_lbl = make_label(face, s_small ? &lv_font_unscii_8 : &lv_font_unscii_16, 0xffffff);
    lv_obj_set_style_text_letter_space(s_state_lbl, s_small ? 1 : 2, 0);
    lv_obj_align(s_state_lbl, LV_ALIGN_TOP_MID, 0, s_small ? 22 : 40 + s_dy);
    lv_obj_set_flag(s_state_lbl, LV_OBJ_FLAG_HIDDEN, s_small && !s_tall && s_h < 200);

    /* Kept as an empty layout anchor; Bluetooth state now lives in the top
     * status row instead of showing the MuseGadget device name here. */
    s_name_lbl = make_label(face, s_small ? &lv_font_unscii_8 : &lv_font_unscii_16, COLOR_DIM);
    lv_obj_align(s_name_lbl, LV_ALIGN_TOP_MID, 0, s_small ? 32 : 60 + s_dy);
    /* Same rule as the state label: a square 128 px screen centres Muse over
     * these rows, so there's nowhere to put this without covering the face. */
    lv_obj_set_flag(s_name_lbl, LV_OBJ_FLAG_HIDDEN, s_small && !s_tall && s_h < 200);

    s_caption_lbl = make_label(face, font_pick(&lv_font_unscii_16, &lv_font_unscii_8), COLOR_CAPTION);
    if (s_small) {
        /* Two lines over the bottom of the face, on a dark band so they stay
         * legible. A tall screen has room to keep them above the mic icon. */
        lv_obj_set_size(s_caption_lbl, s_w, 2 * 8 + 2 + 4);
        lv_obj_set_style_pad_ver(s_caption_lbl, 2, 0);
        lv_obj_set_style_text_line_space(s_caption_lbl, 2, 0);
        lv_obj_set_style_bg_color(s_caption_lbl, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_caption_lbl, LV_OPA_70, 0);
        lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
        /* Keep captions above navigation dots or the physical-button hints. */
        bool raised_caption = s_tall || s_tv || !muse_board->touch;
        lv_obj_align(s_caption_lbl, LV_ALIGN_BOTTOM_MID, 0, raised_caption ? -30 : -3);

        s_bar = lv_obj_create(face);
        lv_obj_remove_style_all(s_bar);
        lv_obj_set_size(s_bar, 0, 3);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
        lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        return;
    }
    /* Fixed height: a longer caption ends in dots rather than growing into the ring. */
    lv_obj_set_size(s_caption_lbl, CAPTION_W, cap_h);
    lv_obj_set_style_text_line_space(s_caption_lbl, CAPTION_LINE_SPACE, 0);
    lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_caption_lbl, LV_ALIGN_CENTER, 0, cap_top + cap_h / 2);

    /* Chunky level meter. */
    int span = METER_SEGS * (METER_SEG_PX + METER_GAP_PX) - METER_GAP_PX;
    for (int i = 0; i < METER_SEGS; i++) {
        lv_obj_t *seg = lv_obj_create(face);
        lv_obj_remove_style_all(seg);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(seg, METER_SEG_PX, METER_SEG_PX);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(COLOR_METER_OFF), 0);
        int x = -span / 2 + i * (METER_SEG_PX + METER_GAP_PX) + METER_SEG_PX / 2;
        lv_obj_align(seg, LV_ALIGN_CENTER, x, meter_y);
        s_meter[i] = seg;
    }

    build_answer(face, ring_in);
}


static void on_cover_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        /* Wake on touch; keep the (now clear) cover until release so the tap
         * doesn't land on whatever is underneath. */
        muse_state_set_asleep(false);
        lv_obj_set_style_bg_opa(s_cover, LV_OPA_TRANSP, 0);
    } else if (!muse_state_asleep()) {
        lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
    }
}

/* With the display lock held. */
static void image_hide_locked(void)
{
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf) {
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_image, NULL);
        heap_caps_free(s_image_buf);
        s_image_buf = NULL;
        s_image_dsc.data = NULL;
        s_image_dirty = false;
    }
    xSemaphoreGive(s_image_mutex);
}

/* With the display lock held. Uploaded media is a real background: it stays
 * behind the avatar, status icons and menus until the web page clears it. */
static void wallpaper_clear_locked(void)
{
    if (s_wallpaper) {
        lv_obj_delete(s_wallpaper);
        s_wallpaper = NULL;
    }
    memset(&s_wallpaper_dsc, 0, sizeof(s_wallpaper_dsc));
    heap_caps_free(s_wallpaper_data);
    s_wallpaper_data = NULL;
}

/* Each frame: shows a new image and redraws what the download changed. */
static void image_sync(void)
{
    uint8_t *uploaded = NULL;
    size_t uploaded_size = 0;
    bool uploaded_gif = false;
    bool clear = false;

    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_wallpaper_pending || s_wallpaper_clear_pending) {
        uploaded = s_wallpaper_pending;
        uploaded_size = s_wallpaper_pending_size;
        uploaded_gif = s_wallpaper_pending_gif;
        clear = s_wallpaper_clear_pending;
        s_wallpaper_pending = NULL;
        s_wallpaper_pending_size = 0;
        s_wallpaper_clear_pending = false;
    }
    xSemaphoreGive(s_image_mutex);

    if (uploaded || clear) {
        wallpaper_clear_locked();
        if (uploaded_gif && uploaded) {
            s_wallpaper_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_wallpaper_dsc.header.cf = LV_COLOR_FORMAT_RAW;
            s_wallpaper_dsc.data_size = uploaded_size;
            s_wallpaper_dsc.data = uploaded;
            s_wallpaper = lv_gif_create(s_face);
            if (s_wallpaper) {
                s_wallpaper_data = uploaded;
                lv_gif_set_color_format(s_wallpaper, LV_COLOR_FORMAT_RGB565);
                lv_gif_set_src(s_wallpaper, &s_wallpaper_dsc);
                lv_obj_center(s_wallpaper);
                lv_obj_move_to_index(s_wallpaper, 0);
                muse_state_set_asleep(false);
            } else {
                heap_caps_free(uploaded);
            }
        } else if (uploaded) {
            /* Web Bluetooth sends RGB565 high byte first, as display.draw_url
             * does. Convert it to the ESP32's native uint16_t representation. */
            uint16_t *pixels = (uint16_t *)uploaded;
            for (size_t i = 0; i < uploaded_size / 2; ++i) {
                uint8_t hi = uploaded[i * 2];
                uint8_t lo = uploaded[i * 2 + 1];
                pixels[i] = (uint16_t)(hi << 8 | lo);
            }
            s_wallpaper_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_wallpaper_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            s_wallpaper_dsc.header.w = s_w;
            s_wallpaper_dsc.header.h = s_h;
            s_wallpaper_dsc.header.stride = s_w * sizeof(uint16_t);
            s_wallpaper_dsc.data_size = uploaded_size;
            s_wallpaper_dsc.data = uploaded;
            s_wallpaper = lv_image_create(s_face);
            if (s_wallpaper) {
                s_wallpaper_data = uploaded;
                lv_image_set_src(s_wallpaper, &s_wallpaper_dsc);
                lv_obj_set_pos(s_wallpaper, 0, 0);
                lv_obj_move_to_index(s_wallpaper, 0);
                muse_state_set_asleep(false);
            } else {
                heap_caps_free(uploaded);
            }
        }
    }

    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf && !s_image_dsc.data) {
        s_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_image_dsc.header.w = s_w;
        s_image_dsc.header.h = s_h;
        s_image_dsc.header.stride = s_w * sizeof(uint16_t);
        s_image_dsc.data_size = (size_t)s_w * s_h * sizeof(uint16_t);
        s_image_dsc.data = (const uint8_t *)s_image_buf;
        muse_menu_close();
        lv_image_set_src(s_image, &s_image_dsc);
        lv_obj_remove_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        muse_state_set_asleep(false);
    }
    if (s_image_dirty) {
        lv_obj_invalidate_area(s_image, &s_image_area);
        s_image_dirty = false;
    }
    xSemaphoreGive(s_image_mutex);
}

static void on_image_clicked(lv_event_t *e)
{
    (void)e;
#if CONFIG_MUSE_WATCHER_CAMERA
    if (watcher_camera_preview_active()) {
        watcher_camera_preview_toggle();
        return;
    }
#endif
    image_hide_locked();
}

#if CONFIG_MUSE_WATCHER_CAMERA
static void on_camera_hint_clicked(lv_event_t *e)
{
    (void)e;
    if (watcher_camera_preview_active()) watcher_camera_preview_toggle();
}
#endif

static void on_any_press(lv_event_t *e)
{
    (void)e;
    muse_state_poke();
}

static void build_overlays(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* Page dots. */
    for (int i = 0; i < 2 && s_tv; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(COLOR_DOT_OFF), 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, i ? 8 : -8, -14);
        s_dots[i] = d;
    }

    /* A downloaded image: over everything on the screen (and in snapshots),
     * under the pairing code and sleep cover on the top layer. */
    s_image = lv_image_create(scr);
    lv_obj_set_pos(s_image, 0, 0);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_image, on_image_clicked, LV_EVENT_CLICKED, NULL);
#if CONFIG_MUSE_WATCHER_CAMERA
    s_camera_hint = lv_btn_create(scr);
    lv_obj_set_size(s_camera_hint, 244, 46);
    lv_obj_align(s_camera_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(s_camera_hint, lv_color_hex(0x201a35), 0);
    lv_obj_set_style_bg_opa(s_camera_hint, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_camera_hint, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_camera_hint, 2, 0);
    lv_obj_set_style_radius(s_camera_hint, 18, 0);
    lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *hint_text = lv_label_create(s_camera_hint);
    lv_label_set_text(hint_text, "TAP TO TAKE PHOTO");
    lv_obj_center(hint_text);
    lv_obj_add_event_cb(s_camera_hint, on_camera_hint_clicked, LV_EVENT_CLICKED, NULL);
#endif

    /* BLE pairing code, or the Muse app's ask for the talk button: a centred
     * column in one typeface, the code large. The small card grows with the hint. */
    s_pair = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_pair);
    lv_obj_set_size(s_pair, s_small ? s_w - 8 : 300, s_small ? LV_SIZE_CONTENT : 150);
    lv_obj_center(s_pair);
    lv_obj_set_flex_flow(s_pair, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pair, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(s_pair, s_small ? 10 : 18, 0);
    lv_obj_set_style_pad_hor(s_pair, s_small ? 6 : 16, 0);
    lv_obj_set_style_pad_row(s_pair, s_small ? 4 : 10, 0);
    lv_obj_set_style_radius(s_pair, s_small ? 10 : 24, 0);
    lv_obj_set_style_bg_opa(s_pair, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_pair, lv_color_hex(0x1a1530), 0);
    lv_obj_set_style_border_color(s_pair, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_pair, 2, 0);
    lv_obj_remove_flag(s_pair, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pair, LV_OBJ_FLAG_HIDDEN);
    s_pair_title = make_label(s_pair, font_pick(&lv_font_montserrat_20, FONT_COMPACT), COLOR_LIT);
    lv_label_set_text(s_pair_title, "Pairing code");
    s_pair_code = make_label(s_pair, font_pick(&lv_font_montserrat_28, &lv_font_montserrat_20), COLOR_ACCENT);
    lv_obj_set_style_text_letter_space(s_pair_code, s_small ? 2 : 6, 0);
    s_pair_hint = make_label(s_pair, font_pick(&lv_font_montserrat_14, FONT_COMPACT), COLOR_DIM);
    lv_label_set_text(s_pair_hint, s_small ? "Enter on phone" : "Enter it on your phone");
    /* Wraps: "bottom right button" is wider than the AIPI's card. */
    lv_obj_set_width(s_pair_hint, lv_pct(100));
    lv_label_set_long_mode(s_pair_hint, LV_LABEL_LONG_MODE_WRAP);

    /* Sleep cover: swallows the waking touch. */
    s_cover = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_cover);
    lv_obj_set_size(s_cover, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_cover, lv_color_black(), 0);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_PRESS_LOST, NULL);

    if (s_indev) {
        lv_indev_add_event_cb(s_indev, on_any_press, LV_EVENT_PRESSED, NULL);
    }
}

static void apply_brightness(int pct)
{
    if (pct != s_brightness) {
        muse_board->set_brightness(pct);
        s_brightness = pct;
    }
}

bool muse_ui_dark(void)
{
    return s_dark;
}

/* Returns true while asleep (skip rendering). */
static bool update_sleep(void)
{
    bool asleep = muse_state_asleep();
    if (asleep && !s_dark) {
        muse_menu_close();
        apply_brightness(0);
        if (muse_board->panel_sleep) {
            muse_board->panel_sleep(true);
        }
        lv_obj_set_style_bg_opa(s_cover, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
        s_dark = true;
    } else if (!asleep && s_dark) {
        s_dark = false;
        if (muse_board->panel_sleep) {
            muse_board->panel_sleep(false);
        }
        /* Woken by a button: drop the cover now. Woken by touch: on release. */
        if (lv_obj_get_style_bg_opa(s_cover, 0) == LV_OPA_COVER) {
            lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!s_dark) {
        int target = muse_settings_brightness();
        if (s_preview_brightness >= 0) {
            if (s_preview_brightness == target) {
                s_preview_brightness = -1;
            } else {
                target = s_preview_brightness;
            }
        }
        apply_brightness(target);
    }
    return s_dark;
}

/* Wi-Fi state is already visible in the top icon, so disconnected idle states
 * leave this line empty. */
static const char *idle_name(muse_wifi_state_t wifi)
{
    (void)wifi;
    return "";
}

static void update_chrome(float now)
{
    if (now < s_next_settings_tick) {
        return;
    }
    s_next_settings_tick = now + SETTINGS_TICK_S;

    if (s_tv) {
        int page = lv_tileview_get_tile_active(s_tv) == s_settings;
        bool subpage = muse_settings_ui_in_subpage();
        bool swipe = !page || !subpage;
        if (swipe != lv_obj_has_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE)) {
            lv_obj_set_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE, swipe);
        }
        int shown = page * 2 + subpage;
        if (shown != s_shown_page) {
            for (int i = 0; i < 2; i++) {
                lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == page ? COLOR_ACCENT : COLOR_DOT_OFF), 0);
                lv_obj_set_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN, page && subpage);
            }
            s_shown_page = shown;
        }
        muse_settings_ui_tick(lv_obj_get_scroll_x(s_tv) > 0);
    }

    /* Wi-Fi is always visible. Disconnected/connecting gets a slash over it. */
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    bool wifi_connected = w.state == MUSE_WIFI_CONNECTED;
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(wifi_connected ? COLOR_ACCENT : COLOR_DIM), 0);
    lv_obj_set_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN, wifi_connected);
    s_idle_name = idle_name(w.state);
    muse_ble_status_t b;
    muse_ble_status(&b);
    bool bt_connected = b.state == MUSE_BLE_CONNECTED;
    lv_obj_set_style_text_color(s_bt_icon, lv_color_hex(bt_connected ? COLOR_ACCENT : COLOR_DIM), 0);
    lv_obj_set_flag(s_bt_slash, LV_OBJ_FLAG_HIDDEN, bt_connected);

    /* Paired, the name has done its job (picking this one out in the Muse
     * app) and the speaker button has replies to mute. */
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    bool paired = h.state != MUSE_HATCH_NOT_SET;

    if (lv_label_get_text(s_name_lbl)[0]) {
        lv_label_set_text(s_name_lbl, "");
    }

    /* The same card asks for the talk button when the Muse app pairs. */
    bool confirm = !b.passkey && muse_link_state() == MUSE_LINK_CONFIRM;
    if (b.passkey || confirm) {
        char code[24], hint[40];
        if (confirm) {
            strlcpy(code, s_small ? "Press" : "Press button", sizeof(code));
            snprintf(hint, sizeof(hint), s_small ? "%s button" : "Press the %s button", muse_board->talk_button);
        } else {
            snprintf(code, sizeof(code), "%06lu", (unsigned long)b.passkey);
            strlcpy(hint, s_small ? "Enter on phone" : "Enter it on your phone", sizeof(hint));
        }
        const char *title = confirm ? (s_small ? "Phone" : "Confirm phone") : "Pairing code";
        if (strcmp(code, lv_label_get_text(s_pair_code)) != 0) {
            lv_label_set_text(s_pair_code, code);
            lv_label_set_text(s_pair_title, title);
            lv_label_set_text(s_pair_hint, hint);
        }
    }
    lv_obj_set_flag(s_pair, LV_OBJ_FLAG_HIDDEN, !b.passkey && !confirm);

    bool speaker = muse_settings_speaker_on();   /* also set from settings, the phone and serial */
    if (s_speaker && (int)speaker != s_shown_speaker) {
        show_speaker(speaker);
    }
    if (s_speaker && paired == lv_obj_has_flag(s_speaker, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_speaker, LV_OBJ_FLAG_HIDDEN, !paired);
    }
    /* Unpaired, a press only says "SET UP MUSE FIRST", so the mic goes too.
     * While a reply's layout is up it decides; that's only ever paired. */
    if (s_answer < 0 && paired == lv_obj_has_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN, !paired);
    }
}

/* One area for the whole meter before its segments change: LVGL won't join
 * squares with gaps between them, and each area it keeps costs a walk of the
 * widget tree. The segments' own areas then fall inside this one. */
static void invalidate_meter(void)
{
    lv_area_t a, last;
    lv_obj_get_coords(s_meter[0], &a);
    lv_obj_get_coords(s_meter[METER_SEGS - 1], &last);
    a.x2 = last.x2;
    lv_obj_invalidate_area(lv_obj_get_parent(s_meter[0]), &a);
}

static void set_meter_visible(bool visible)
{
    if (s_small || visible == s_meter_visible) {
        return;
    }
    invalidate_meter();
    for (int i = 0; i < METER_SEGS; i++) {
        if (visible) {
            lv_obj_remove_flag(s_meter[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_meter[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_meter_visible = visible;
    s_shown_lit = -1;
}

static void update_power(float now)
{
    if (now < s_next_power_update) {
        return;
    }
    s_next_power_update = now + 1.0f;

    muse_power_t p = muse_state_power();
    uint32_t color = p.charging ? COLOR_ACCENT : p.battery_pct >= 0 && p.battery_pct <= 20 ? 0xff554d : COLOR_LIT;
    int pct = p.battery_pct < 0 ? 0 : p.battery_pct > 100 ? 100 : p.battery_pct;
    int filled = pct == 0 ? 0 : (pct + 24) / 25;
    lv_obj_set_style_border_color(s_battery_body, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_battery_tip, lv_color_hex(color), 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_battery_segments[i], lv_color_hex(i < filled ? color : 0x302e3a), 0);
        lv_obj_set_flag(s_battery_segments[i], LV_OBJ_FLAG_HIDDEN, p.charging);
    }
    lv_obj_set_flag(s_battery_charge, LV_OBJ_FLAG_HIDDEN, !p.charging);
}

static void update_status(muse_mode_t mode, float now)
{
    uint32_t accent = muse_pixel_accent(mode);
    const char *name = mode == MUSE_MODE_IDLE ? s_idle_name : MODE_NAMES[mode];

    if (name != s_shown_name) {
        lv_label_set_text(s_state_lbl, name);
        s_shown_name = name;
    }
    if ((int)mode != s_shown_state) {
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(accent), 0);
        if (s_ring) {
            lv_obj_set_style_arc_color(s_ring, lv_color_hex(accent), LV_PART_INDICATOR);
        } else if (s_bar) {
            lv_obj_set_style_bg_color(s_bar, lv_color_hex(accent), 0);
        }
        set_mic_color(mode == MUSE_MODE_LISTENING ? accent : COLOR_DIM);   /* lights up while recording */
        lv_obj_set_flag(s_status_mic, LV_OBJ_FLAG_HIDDEN, mode != MUSE_MODE_LISTENING);
        if (mode == MUSE_MODE_LISTENING) {
            set_mic_object_color(s_status_mic, accent);
        }
        s_shown_state = (int)mode;
        s_shown_lit = -1;
    }

    /* Ring: phase progress while listening/speaking, a spinner while thinking. */
    int ring = 0;
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        ring = (int)(muse_state_progress() * RING_RANGE);
    } else if (mode == MUSE_MODE_THINKING) {
        ring = RING_RANGE / 6;
    }
    if (s_ring && mode == MUSE_MODE_THINKING) {
        /* Move the indicator rather than rotate the arc: a rotation redraws
         * the whole ring, and with it the whole screen. */
        int start = (int)(now * 300.0f) % 360;
        lv_arc_set_angles(s_ring, start, start + ring * 360 / RING_RANGE);
        s_ring_value = -1;
    } else if (s_ring) {
        if (s_ring_value < 0) {
            lv_arc_set_bg_start_angle(s_ring, 0);   /* unchanged, but puts the value's angles back */
        }
        if (ring != s_ring_value) {
            lv_arc_set_value(s_ring, ring);
            s_ring_value = ring;
        }
    } else if (s_bar) {
        /* Bar along the bottom edge; a sliding segment while thinking. */
        int x = mode == MUSE_MODE_THINKING ? (int)(now * s_w) % s_w : 0;
        int w = ring * s_w / RING_RANGE;
        if (ring != s_ring_value || x) {
            lv_obj_set_width(s_bar, w);
            lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, x, 0);
            s_ring_value = ring;
        }
    }

    /* The mic's meter; a reply's page has the room while answering. */
    bool meter = mode == MUSE_MODE_LISTENING;
    set_meter_visible(meter);
    if (meter && !s_small) {
        int lit = (int)lroundf(s_level * METER_SEGS);
        if (lit != s_shown_lit || accent != s_shown_accent) {
            invalidate_meter();
            for (int i = 0; i < METER_SEGS; i++) {
                /* Outer segments light last, like a centred VU. */
                int rank = abs(2 * i - (METER_SEGS - 1)) / 2;
                bool on = rank < (lit + 1) / 2;
                lv_obj_set_style_bg_color(s_meter[i], lv_color_hex(on ? accent : COLOR_METER_OFF), 0);
            }
            s_shown_lit = lit;
            s_shown_accent = accent;
        }
    }

    /* On the 240x240 button-only board, short idle notices use the otherwise
     * empty button-hint strip. Listening and reply captions stay raised. */
    bool caption_low = s_small && mode == MUSE_MODE_IDLE;
    if (caption_low != s_caption_low) {
        lv_obj_align(s_caption_lbl, LV_ALIGN_BOTTOM_MID, 0, caption_low ? -3 : -30);
        s_caption_low = caption_low;
    }

    static char caption[MUSE_CAPTION_MAX];
    bool fresh = muse_state_caption(caption, sizeof(caption), &s_caption_version);
    int answer = -1;
    if (s_reply_lbl) {
        /* The speaker picks the layout, even mid-reply: the voice task pages to fit. */
        int layout = muse_settings_speaker_on() ? ANSWER_HEARD : ANSWER_READ;
        if (layout != s_page_for) {
            muse_state_set_page(s_answers[layout].cols, s_answers[layout].lines);
            s_page_for = layout;
        }
        if (mode == MUSE_MODE_THINKING || mode == MUSE_MODE_SPEAKING) {
            answer = layout;
        }
    }
    if (answer != s_answer) {
        set_answer(answer);
        fresh = true;   /* the caption moves between labels */
    }
    if (mode != MUSE_MODE_IDLE) {
        s_idle_caption_until = 0;
    } else if (fresh) {
        s_idle_caption_until = caption[0] ? now + IDLE_CAPTION_SECS : 0;
    } else if (s_idle_caption_until > 0 && now >= s_idle_caption_until) {
        caption[0] = '\0';
        s_idle_caption_until = 0;
        muse_state_set_caption("%s", "");
        fresh = true;
    }
    if (fresh) {
        lv_obj_t *lbl = answer >= 0 ? s_reply_lbl : s_caption_lbl;
        lv_label_set_text(lbl, caption);
        lv_obj_set_flag(lbl, LV_OBJ_FLAG_HIDDEN, !caption[0]);
        if (s_reply_lbl) {
            lv_obj_add_flag(answer >= 0 ? s_caption_lbl : s_reply_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    update_power(now);
}

static volatile bool s_snapshot;

/* Streams the screen over the USB cable as base64 RGB565 (bench testing; needs
 * LV_USE_SNAPSHOT, which devices/sdkconfig.muse-bench turns on). */
static void send_snapshot(void)
{
#if LV_USE_SNAPSHOT
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!buf) {
        return;
    }
    enum { RAW = 144 };   /* lines must fit the driver's 256-byte TX ring */
    char hdr[48];
    int n = snprintf(hdr, sizeof(hdr), "\nSNAP BEGIN %d %d %d\n", (int)buf->header.w, (int)buf->header.h, RAW);
    muse_console_write(hdr, n);
    static unsigned char b64[4 * RAW / 3 + 4];
    for (uint32_t y = 0; y < buf->header.h; y++) {
        const unsigned char *p = buf->data + y * buf->header.stride;
        size_t left = buf->header.w * 2;
        while (left) {
            size_t chunk = left > RAW ? RAW : left, olen;
            mbedtls_base64_encode(b64, sizeof(b64), &olen, p, chunk);
            b64[olen++] = '\n';
            muse_console_write(b64, olen);
            p += chunk;
            left -= chunk;
        }
    }
    muse_console_write("SNAP END\n", 9);
    lv_draw_buf_destroy(buf);
#else
    muse_console_write("\nSNAP OFF\n", 10);   /* so snap.py can say why */
#endif
}

void muse_ui_request_snapshot(void)
{
    s_snapshot = true;
}

static void frame_tick(lv_timer_t *timer)
{
    if (s_snapshot) {
        s_snapshot = false;
        send_snapshot();
    }
    (void)timer;
    image_sync();
    float mode_t;
    muse_mode_t mode = muse_state_mode(&mode_t);
    float now = (float)esp_timer_get_time() / 1e6f;

    if (mode != s_last_mode) {
        if (mode == MUSE_MODE_LISTENING) {
            image_hide_locked();
            muse_ui_show_face();
        }
        s_last_mode = mode;
    }
    if (update_sleep()) {
        return;
    }
    update_chrome(now);
    if (muse_menu_tick(now)) {
        image_hide_locked();
        return;   /* the menu covers the face */
    }
    if (s_image_dsc.data) {
        return;   /* the image covers the face */
    }
    if (s_tv && lv_obj_get_scroll_x(s_tv) != 0) {
        /* Off screen, or sliding to or from settings: hold still so the
         * slide gets the whole frame time. */
        return;
    }

    /* Fast attack, slow release keeps the mouth and meter lively but readable. */
    float level = muse_state_level();
    s_level += (level - s_level) * (level > s_level ? 0.6f : 0.2f);

    muse_pose_t pose = {
        .mode = mode,
        .t = now,
        .mode_t = mode_t,
        .level = s_level,
        .happy = muse_state_happiness(),
    };
    muse_pixel_render(&pose);
    invalidate_muse();

    update_status(mode, now);
}

esp_err_t muse_ui_start(void)
{
    s_w = muse_board->width;
    s_h = muse_board->height;
    /* The full layout assumes room for the 466 px board's header and bottom
     * captions. Short landscape panels (BOX-3) need the compact layout too. */
    bool short_landscape = s_w > s_h && s_h < 320;
    bool compact_square = s_w <= 240 && s_h <= 240;
    s_small = s_h < 200 || s_w < 200 || short_landscape || compact_square;
    s_tall = s_small && s_h >= s_w + 64;
    /* Small screens keep room for the status line and button icons. A narrow
     * one is as wide as Muse gets, in whole pixels. */
    s_canvas_px = s_small ? s_h * 3 / 4 : MUSE_PX_W * 5;
    if (short_landscape) {
        /* Leave the header's first 40 rows and bottom captions clear. */
        s_canvas_px = s_h * 2 / 3;
    }
    if (s_canvas_px > s_w) {
        s_canvas_px = s_w / MUSE_PX_W * MUSE_PX_W;
    }
    s_dy = (s_h - 466) / 2;
    if (!s_small && muse_board->round && s_dy < 0) {
        /* A shorter screen moves the labels out towards its edges, but a
         * smaller circle's edges curve away from them. Keep them where they
         * are and shrink Muse to whole pixels that fit between them. */
        s_canvas_px = (MUSE_PX_W * 5 + 2 * s_dy) / MUSE_PX_W * MUSE_PX_W;
        s_dy = 0;
    }

    lv_display_t *disp = muse_board->display_start(&s_indev);
    if (!disp) {
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }

    s_image_mutex = xSemaphoreCreateMutex();
    muse_board->display_lock(-1);
    build_screen();
    if (s_settings) {
        muse_settings_ui_build(s_settings);
    } else {
        muse_menu_build(lv_screen_active(), s_w, s_h);
    }
    build_overlays();
#if LV_USE_GIF
    s_boot_splash = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_boot_splash);
    lv_obj_set_size(s_boot_splash, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_boot_splash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_boot_splash, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_boot_splash, LV_OBJ_FLAG_SCROLLABLE);

    s_boot_gif_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_boot_gif_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    s_boot_gif_dsc.data_size = (uint32_t)(boot_gif_end - boot_gif_start);
    s_boot_gif_dsc.data = boot_gif_start;
    lv_obj_t *gif = lv_gif_create(s_boot_splash);
    lv_gif_set_color_format(gif, LV_COLOR_FORMAT_RGB565);
    lv_gif_set_src(gif, &s_boot_gif_dsc);
    /* The embedded source is pre-sized to 240x180 for this panel. */
    lv_obj_center(gif);
    lv_obj_move_foreground(s_boot_splash);
#endif
    lv_timer_create(frame_tick, muse_board->frame_ms, NULL);
    s_ready = true;
    muse_board->display_unlock();

    ESP_LOGI(TAG, "UI up: %dx%d, %d px Muse, %d ms frames", s_w, s_h, s_canvas_px, muse_board->frame_ms);
    return ESP_OK;
}

void muse_ui_boot_splash_hide(void)
{
    if (!s_boot_splash || !muse_board->display_lock(-1)) {
        return;
    }
    lv_obj_delete(s_boot_splash);
    s_boot_splash = NULL;
    muse_board->display_unlock();
}

void muse_ui_show_face(void)
{
    if (!s_tv) {
        return;
    }
    lv_obj_add_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE);
    lv_tileview_set_tile(s_tv, s_face, LV_ANIM_ON);
}

void muse_ui_set_swipe_enabled(bool enabled)
{
    if (!s_tv) {
        return;
    }
    lv_obj_set_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE, enabled);
    s_shown_page = -1;
    s_next_settings_tick = 0;
}

void muse_ui_preview_brightness(int pct)
{
    s_preview_brightness = pct;
    apply_brightness(pct);
}

bool muse_ui_image_size(int *w, int *h)
{
    if (!s_ready || !heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        return false;
    }
    *w = s_w;
    *h = s_h;
    return true;
}

bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!s_ready || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > s_w || y + h > s_h) {
        return false;
    }
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (!s_image_buf) {
        /* The first rectangle of an image: a black screen to draw onto. */
        s_image_buf = heap_caps_calloc((size_t)s_w * s_h, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_image_buf) {
            xSemaphoreGive(s_image_mutex);
            return false;
        }
        /* Now, not when it's shown: asleep, the display may be paused (muse_input.c). */
        muse_state_set_asleep(false);
    }
    const uint8_t *src = (const uint8_t *)pixels;
    for (int row = 0; row < h; row++) {
        uint16_t *dst = s_image_buf + (size_t)(y + row) * s_w + x;
        for (int i = 0; i < w; i++, src += 2) {
            dst[i] = (uint16_t)(src[0] << 8 | src[1]);
        }
    }
    lv_area_t *a = &s_image_area;
    if (!s_image_dirty) {
        *a = (lv_area_t){ x, y, x + w - 1, y + h - 1 };
        s_image_dirty = true;
    } else {
        a->x1 = LV_MIN(a->x1, x);
        a->y1 = LV_MIN(a->y1, y);
        a->x2 = LV_MAX(a->x2, x + w - 1);
        a->y2 = LV_MAX(a->y2, y + h - 1);
    }
    xSemaphoreGive(s_image_mutex);
    return true;
}

void muse_ui_image_hide(void)
{
    if (!s_ready) {
        return;
    }
    muse_board->display_lock(-1);
    image_hide_locked();
#if CONFIG_MUSE_WATCHER_CAMERA
    if (s_camera_hint) lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
#endif
    muse_board->display_unlock();
}

bool muse_ui_wallpaper_submit(uint8_t *data, size_t size, bool gif)
{
    if (!s_ready || !data || (!gif && size != (size_t)s_w * s_h * 2) ||
        (gif && (size < 6 || size > 1024 * 1024 || memcmp(data, "GIF8", 4) != 0))) {
        return false;
    }
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    heap_caps_free(s_wallpaper_pending);
    s_wallpaper_pending = data;
    s_wallpaper_pending_size = size;
    s_wallpaper_pending_gif = gif;
    s_wallpaper_clear_pending = false;
    xSemaphoreGive(s_image_mutex);
    return true;
}

void muse_ui_wallpaper_clear(void)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    heap_caps_free(s_wallpaper_pending);
    s_wallpaper_pending = NULL;
    s_wallpaper_pending_size = 0;
    s_wallpaper_clear_pending = true;
    xSemaphoreGive(s_image_mutex);
}

#if CONFIG_MUSE_WATCHER_CAMERA
void muse_ui_camera_hint(bool visible)
{
    if (!s_ready || !s_camera_hint) return;
    muse_board->display_lock(-1);
    if (visible) lv_obj_remove_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    muse_board->display_unlock();
}
#endif
