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

/*
 * Zhengchen 1.54 TFT Wi-Fi: ESP32-S3 with 16 MB flash, 8 MB octal PSRAM,
 * 240x240 ST7789 LCD, three active-low buttons, a 32-bit I2S microphone and
 * a MAX98357A I2S speaker. Pins, display inversion and microphone format were
 * validated on hardware in FriendOS. The simplex audio layout also matches
 * xiaozhi-esp32's no-codec boards.
 */
#include <limits.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_HOST SPI2_HOST
#define LCD_MOSI GPIO_NUM_41
#define LCD_SCLK GPIO_NUM_42
#define LCD_CS GPIO_NUM_21
#define LCD_DC GPIO_NUM_40
#define LCD_RST GPIO_NUM_45
#define LCD_BL GPIO_NUM_20
#define DRAW_BUF_LINES 40

#define MIC_WS GPIO_NUM_4
#define MIC_SCK GPIO_NUM_5
#define MIC_DATA GPIO_NUM_6
#define SPK_DATA GPIO_NUM_7
#define SPK_BCLK GPIO_NUM_15
#define SPK_WS GPIO_NUM_16

#define TALK_GPIO GPIO_NUM_0
#define VOLUME_DOWN_GPIO GPIO_NUM_39
#define VOLUME_UP_GPIO GPIO_NUM_10

#define BATTERY_ADC ADC_CHANNEL_7     /* GPIO8 / ADC1_CH7 */
#define CHARGE_GPIO GPIO_NUM_9         /* high while external power is charging */

static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk, s_volume_down, s_volume_up;
static i2s_chan_handle_t s_mic_rx, s_spk_tx;
static adc_oneshot_unit_handle_t s_adc;
static bool s_mic_on, s_spk_on;

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_volume_down, VOLUME_DOWN_GPIO), TAG, "volume down button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_volume_up, VOLUME_UP_GPIO), TAG, "volume up button");

    const gpio_config_t charge_cfg = {
        .pin_bit_mask = BIT64(CHARGE_GPIO),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&charge_cfg), TAG, "charge status");

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "battery adc");
    const adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    return adc_oneshot_config_channel(s_adc, BATTERY_ADC, &channel_cfg);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_channel = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_channel) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }

    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_invert_color(s_panel, true) != ESP_OK || esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        return NULL;
    }

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t display_cfg = {
        .panel = s_panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_WIDTH,
            .ver_res = LCD_HEIGHT,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *display = esp_lv_adapter_register_display(&display_cfg);
    if (!display || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return display;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (pct == 0) {
        /* ledc_stop() disables the PWM signal, which can leave this pad floating
         * on the S3's shared USB-D+ GPIO. Detach it from LEDC and drive it as a
         * plain output so the active-high backlight is physically held off. */
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        gpio_reset_pin(LCD_BL);
        gpio_set_level(LCD_BL, 0);
        gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
        gpio_set_level(LCD_BL, 0);
        gpio_hold_en(LCD_BL);
        return;
    }

    gpio_hold_dis(LCD_BL);
    /* set_brightness(0) deliberately detached the pin from LEDC. Route the
     * channel back to GPIO20 before restoring a non-zero duty. */
    ledc_set_pin(LCD_BL, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    /* The ST7789 sleep command stops panel scanning but does not control the
     * separate backlight pin. Force GPIO20's PWM low as part of board sleep. */
    if (sleep) {
        set_brightness(0);
    }
    esp_lcd_panel_disp_sleep(s_panel, sleep);
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

static int mic_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    if (on == s_mic_on) {
        return ESP_CODEC_DEV_OK;
    }
    esp_err_t err = on ? i2s_channel_enable(s_mic_rx) : i2s_channel_disable(s_mic_rx);
    if (err != ESP_OK) {
        return ESP_CODEC_DEV_DRV_ERR;
    }
    s_mic_on = on;
    return ESP_CODEC_DEV_OK;
}

static int mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    int frames = size / (2 * sizeof(int16_t));
    if (frames > MUSE_AUDIO_CHUNK) {
        return ESP_CODEC_DEV_READ_FAIL;
    }

    int32_t raw[MUSE_AUDIO_CHUNK];
    size_t got = 0;
    if (i2s_channel_read(s_mic_rx, raw, frames * sizeof(raw[0]), &got, pdMS_TO_TICKS(1000)) != ESP_OK ||
        got != (size_t)frames * sizeof(raw[0])) {
        return ESP_CODEC_DEV_READ_FAIL;
    }

    int16_t *out = (int16_t *)data;
    for (int i = 0; i < frames; i++) {
        int32_t sample = raw[i] >> 16;
        sample = sample > INT16_MAX ? INT16_MAX : sample < INT16_MIN ? INT16_MIN : sample;
        out[2 * i] = out[2 * i + 1] = (int16_t)sample;
    }
    return ESP_CODEC_DEV_OK;
}

static int spk_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    if (on == s_spk_on) {
        return ESP_CODEC_DEV_OK;
    }
    esp_err_t err = on ? i2s_channel_enable(s_spk_tx) : i2s_channel_disable(s_spk_tx);
    if (err != ESP_OK) {
        return ESP_CODEC_DEV_DRV_ERR;
    }
    s_spk_on = on;
    return ESP_CODEC_DEV_OK;
}

static int spk_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t wrote = 0;
    return i2s_channel_write(s_spk_tx, data, size, &wrote, portMAX_DELAY) == ESP_OK && wrote == (size_t)size
               ? ESP_CODEC_DEV_OK
               : ESP_CODEC_DEV_WRITE_FAIL;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_config_t mic_channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_channel, NULL, &s_mic_rx), TAG, "mic channel");
    i2s_std_config_t mic_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DATA,
        },
    };
    mic_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_mic_rx, &mic_cfg), TAG, "mic i2s");

    i2s_chan_config_t spk_channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    spk_channel.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_channel, &s_spk_tx, NULL), TAG, "speaker channel");
    const i2s_std_config_t spk_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK,
            .ws = SPK_WS,
            .dout = SPK_DATA,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_spk_tx, &spk_cfg), TAG, "speaker i2s");

    static const audio_codec_data_if_t spk_if = { .enable = spk_enable, .write = spk_write };
    static const audio_codec_data_if_t mic_if = { .enable = mic_enable, .read = mic_read };
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &spk_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk)
           | muse_gpio_button_poll(&s_volume_down) << 2
           | muse_gpio_button_poll(&s_volume_up) << 4;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk, &s_volume_down, &s_volume_up }, 3, timeout_ms);
}

/* Zhengchen's xiaozhi profile measures GPIO8 as ADC1_CH7 and maps the raw
 * value through these board-specific points. Keep three readings, like the
 * reference implementation, so the displayed level does not jump. */
static esp_err_t read_power(muse_power_t *out)
{
    static int samples[3];
    static int sample_count;
    static int sample_next;
    static const struct {
        int adc;
        int pct;
    } levels[] = {
        { 2030, 0 }, { 2134, 20 }, { 2252, 40 },
        { 2370, 60 }, { 2488, 80 }, { 2606, 100 },
    };

    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATTERY_ADC, &raw), TAG, "battery read");
        sum += raw;
    }
    samples[sample_next] = sum / 8 + 80;   /* offset used by the original Zhengchen firmware */
    sample_next = (sample_next + 1) % 3;
    if (sample_count < 3) {
        sample_count++;
    }

    int average = 0;
    for (int i = 0; i < sample_count; i++) {
        average += samples[i];
    }
    average /= sample_count;

    int pct = 100;
    if (average < levels[0].adc) {
        pct = 0;
    } else {
        for (int i = 0; i < 5; i++) {
            if (average < levels[i + 1].adc) {
                pct = levels[i].pct
                      + (average - levels[i].adc) * (levels[i + 1].pct - levels[i].pct)
                            / (levels[i + 1].adc - levels[i].adc);
                break;
            }
        }
    }

    bool external_power = gpio_get_level(CHARGE_GPIO) == 1;
    out->battery_pct = pct;
    out->battery_mv = 0;   /* this board profile is calibrated in raw ADC units */
    out->usb = external_power;
    out->charging = external_power && pct < 100;
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Zhengchen 1.54 TFT Wi-Fi",
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round = false,
    .touch = false,
    .diagonal_in = 1.54f,
    .talk_button = "boot",
    .aux_button = "volume down",
    .talk_hint = { LV_ALIGN_BOTTOM_LEFT, 12, -4 },
    .aux_hint = { LV_ALIGN_BOTTOM_RIGHT, -12, -4 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
