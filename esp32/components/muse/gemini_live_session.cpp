/* Gemini Live backend for the Muse voice UI.
 *
 * The public function names retain the muse_hatch_* ABI so the existing voice
 * and settings UI can use Gemini without carrying a second audio pipeline.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

extern "C" {
#include "cJSON.h"
#include "muse_settings.h"
#include "muse_wifi.h"
}
#include "muse_chat_priv.h"

static const char *TAG = "gemini_live";

#define MIC_RATE 16000
#define GEMINI_RATE 24000
#define IN_BYTES (MIC_RATE * 2 * 10)
#define OUT_BYTES (MIC_RATE * 2 * 4)
#define EVENT_TEXT 96
#define REPLY_TEXT 2048
#define WS_RX_LIMIT (96 * 1024)
#define SEND_PCM_FRAMES 320
#define GEMINI_MODEL "models/gemini-3.8-live"

enum cmd_type_t : uint8_t {
    CMD_CONNECT,
    CMD_FORGET,
    CMD_BEGIN,
    CMD_END,
    CMD_CANCEL,
    CMD_TEXT,
    CMD_TEXT_CANCEL,
};

struct cmd_t {
    cmd_type_t type;
    uint32_t generation;
    char *text;
};

struct event_t {
    muse_hatch_ev_t type;
    uint32_t generation;
    char text[EVENT_TEXT];
};

struct packet_t {
    char *data;
    size_t len;
};

static QueueHandle_t s_commands;
static QueueHandle_t s_events;
static QueueHandle_t s_packets;
static StreamBufferHandle_t s_input;
static StreamBufferHandle_t s_output;
static esp_websocket_client_handle_t s_ws;
static std::atomic<uint32_t> s_generation{0};
static std::atomic<bool> s_connected{false};
static std::atomic<bool> s_setup_complete{false};
static std::atomic<bool> s_resting{false};
static bool s_turn_active;
static bool s_end_requested;
static bool s_end_sent;
static bool s_reply_started;
static bool s_text_turn;
static char s_reply[REPLY_TEXT];
static size_t s_reply_len;
static portMUX_TYPE s_reply_lock = portMUX_INITIALIZER_UNLOCKED;

/* WebSocket event fragments are delivered serially by the client task. */
static char *s_rx_message;
static size_t s_rx_size;

static void emit(muse_hatch_ev_t type, const char *text)
{
    if (!s_events) {
        return;
    }
    event_t ev = {};
    ev.type = type;
    ev.generation = s_generation.load();
    if (text) {
        strlcpy(ev.text, text, sizeof(ev.text));
    }
    if (xQueueSend(s_events, &ev, 0) != pdTRUE) {
        event_t old;
        xQueueReceive(s_events, &old, 0);
        xQueueSend(s_events, &ev, 0);
    }
}

static void reply_clear(void)
{
    portENTER_CRITICAL(&s_reply_lock);
    s_reply[0] = '\0';
    s_reply_len = 0;
    portEXIT_CRITICAL(&s_reply_lock);
}

static void reply_append(const char *text)
{
    if (!text || !*text) {
        return;
    }
    portENTER_CRITICAL(&s_reply_lock);
    size_t room = sizeof(s_reply) - 1 - s_reply_len;
    size_t n = strlen(text);
    if (n > room) {
        n = room;
    }
    memcpy(s_reply + s_reply_len, text, n);
    s_reply_len += n;
    s_reply[s_reply_len] = '\0';
    portEXIT_CRITICAL(&s_reply_lock);
}

static cJSON *member_any(cJSON *object, const char *camel, const char *snake)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, camel);
    return item ? item : cJSON_GetObjectItemCaseSensitive(object, snake);
}

static bool send_json(const char *json)
{
    if (!s_ws || !s_connected.load()) {
        return false;
    }
    int len = (int)strlen(json);
    return esp_websocket_client_send_text(s_ws, json, len, pdMS_TO_TICKS(5000)) == len;
}

static void ws_event(void *, esp_event_base_t, int32_t id, void *data)
{
    auto *ev = static_cast<esp_websocket_event_data_t *>(data);
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        s_connected.store(true);
        ESP_LOGI(TAG, "WebSocket connected");
        return;
    }
    if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED || id == WEBSOCKET_EVENT_ERROR) {
        s_connected.store(false);
        s_setup_complete.store(false);
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA || !ev || ev->op_code != 1 || ev->payload_len <= 0 ||
        ev->payload_len > WS_RX_LIMIT) {
        return;
    }
    if (ev->payload_offset == 0) {
        free(s_rx_message);
        s_rx_message = static_cast<char *>(heap_caps_malloc((size_t)ev->payload_len + 1, MALLOC_CAP_SPIRAM));
        s_rx_size = s_rx_message ? (size_t)ev->payload_len : 0;
    }
    if (!s_rx_message || ev->payload_offset < 0 || ev->data_len < 0 ||
        (size_t)ev->payload_offset + (size_t)ev->data_len > s_rx_size) {
        free(s_rx_message);
        s_rx_message = nullptr;
        s_rx_size = 0;
        return;
    }
    memcpy(s_rx_message + ev->payload_offset, ev->data_ptr, (size_t)ev->data_len);
    if ((size_t)ev->payload_offset + (size_t)ev->data_len == s_rx_size) {
        s_rx_message[s_rx_size] = '\0';
        packet_t packet = {s_rx_message, s_rx_size};
        if (xQueueSend(s_packets, &packet, 0) != pdTRUE) {
            free(packet.data);
        }
        s_rx_message = nullptr;
        s_rx_size = 0;
    }
}

static void disconnect(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
    }
    s_connected.store(false);
    s_setup_complete.store(false);
    free(s_rx_message);
    s_rx_message = nullptr;
    s_rx_size = 0;
}

static bool connect_gemini(void)
{
    if (s_ws && s_connected.load()) {
        return true;
    }
    disconnect();
    if (!muse_hatch_configured() || !muse_wifi_connected()) {
        return false;
    }

    char *key = static_cast<char *>(heap_caps_malloc(MUSE_TOKEN_MAX + 1, MALLOC_CAP_SPIRAM));
    char *uri = static_cast<char *>(heap_caps_malloc(MUSE_TOKEN_MAX + 256, MALLOC_CAP_SPIRAM));
    if (!key || !uri) {
        free(key);
        free(uri);
        muse_hatch_report(MUSE_HATCH_UNREACHABLE, "Not enough memory");
        return false;
    }
    muse_settings_hatch_token(key);
    snprintf(uri, MUSE_TOKEN_MAX + 256,
             "wss://generativelanguage.googleapis.com/ws/"
             "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=%s",
             key);
    memset(key, 0, MUSE_TOKEN_MAX + 1);
    free(key);

    esp_websocket_client_config_t config = {};
    config.uri = uri;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 8192;
    config.task_stack = 6144;
    config.task_prio = 5;
    config.disable_auto_reconnect = true;
    config.network_timeout_ms = 15000;
    config.ping_interval_sec = 20;
    s_ws = esp_websocket_client_init(&config);
    memset(uri, 0, MUSE_TOKEN_MAX + 256);
    free(uri);
    if (!s_ws) {
        muse_hatch_report(MUSE_HATCH_UNREACHABLE, "WebSocket init failed");
        return false;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, nullptr);
    muse_hatch_report(MUSE_HATCH_TESTING, "Connecting to Gemini...");
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        disconnect();
        muse_hatch_report(MUSE_HATCH_UNREACHABLE, "Connection failed");
        return false;
    }
    for (int i = 0; i < 150 && !s_connected.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_connected.load()) {
        disconnect();
        muse_hatch_report(MUSE_HATCH_UNREACHABLE, "Gemini timed out");
        return false;
    }

    static const char setup[] =
        "{\"setup\":{"
        "\"model\":\"" GEMINI_MODEL "\","
        "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]},"
        "\"systemInstruction\":{\"parts\":[{\"text\":\"Bạn là trợ lý giọng nói cá nhân tên Hisu. "
        "Luôn trả lời bằng tiếng Việt tự nhiên, ngắn gọn và chính xác, không dùng đại từ nhân xưng.\"}]},"
        "\"inputAudioTranscription\":{},\"outputAudioTranscription\":{},"
        "\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":true}},"
        "\"tools\":[{\"googleSearch\":{}}]}}";
    if (!send_json(setup)) {
        disconnect();
        muse_hatch_report(MUSE_HATCH_UNREACHABLE, "Setup send failed");
        return false;
    }
    return true;
}

static void send_audio(const int16_t *pcm, size_t frames)
{
    size_t raw = frames * sizeof(int16_t);
    if (frames > SEND_PCM_FRAMES) {
        return;
    }
    constexpr size_t b64_cap = ((SEND_PCM_FRAMES * 2 + 2) / 3) * 4 + 1;
    char b64[b64_cap];
    char json[b64_cap + 128];
    size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(b64), b64_cap, &written,
                              reinterpret_cast<const unsigned char *>(pcm), raw) == 0) {
        b64[written] = '\0';
        snprintf(json, b64_cap + 128,
                 "{\"realtimeInput\":{\"audio\":{\"data\":\"%s\","
                 "\"mimeType\":\"audio/pcm;rate=16000\"}}}", b64);
        send_json(json);
    }
}

/* 24 kHz PCM16 -> 16 kHz PCM16. Every three input samples become two. */
static void queue_audio_16k(const uint8_t *data, size_t bytes)
{
    static int16_t carry[2];
    static int carry_n;
    size_t samples = bytes / 2;
    const int16_t *in = reinterpret_cast<const int16_t *>(data);
    size_t total = samples + (size_t)carry_n;
    int16_t *joined = static_cast<int16_t *>(heap_caps_malloc(total * 2, MALLOC_CAP_SPIRAM));
    if (!joined) {
        return;
    }
    memcpy(joined, carry, (size_t)carry_n * 2);
    memcpy(joined + carry_n, in, samples * 2);
    size_t groups = total / 3;
    int16_t *out = static_cast<int16_t *>(heap_caps_malloc(groups * 2 * 2, MALLOC_CAP_SPIRAM));
    if (out) {
        for (size_t i = 0; i < groups; ++i) {
            int16_t a = joined[i * 3];
            int16_t b = joined[i * 3 + 1];
            int16_t c = joined[i * 3 + 2];
            out[i * 2] = a;
            out[i * 2 + 1] = (int16_t)(((int32_t)b + c) / 2);
        }
        xStreamBufferSend(s_output, out, groups * 4, pdMS_TO_TICKS(100));
        free(out);
    }
    carry_n = (int)(total % 3);
    memcpy(carry, joined + groups * 3, (size_t)carry_n * 2);
    free(joined);
}

static void handle_inline_audio(cJSON *part)
{
    cJSON *inline_data = member_any(part, "inlineData", "inline_data");
    if (!cJSON_IsObject(inline_data)) {
        return;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(inline_data, "data");
    if (!cJSON_IsString(data) || !data->valuestring) {
        return;
    }
    size_t encoded = strlen(data->valuestring);
    size_t cap = encoded * 3 / 4 + 4;
    uint8_t *pcm = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    size_t decoded = 0;
    if (pcm && mbedtls_base64_decode(pcm, cap, &decoded,
                                     reinterpret_cast<const unsigned char *>(data->valuestring), encoded) == 0) {
        if (!s_reply_started) {
            s_reply_started = true;
            emit(MUSE_HATCH_EV_REPLY, "Gemini is replying");
        }
        queue_audio_16k(pcm, decoded);
    }
    free(pcm);
}

static void handle_packet(packet_t &packet)
{
    cJSON *root = cJSON_ParseWithLength(packet.data, packet.len);
    if (!root) {
        ESP_LOGW(TAG, "invalid JSON from Gemini (%u bytes)", (unsigned)packet.len);
        return;
    }
    if (member_any(root, "setupComplete", "setup_complete")) {
        s_setup_complete.store(true);
        muse_hatch_report(MUSE_HATCH_REACHABLE, "Gemini ready");
        ESP_LOGI(TAG, "Gemini Live session ready");
    }
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error)) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
        const char *why = cJSON_IsString(message) ? message->valuestring : "Gemini error";
        ESP_LOGW(TAG, "Gemini error: %.120s", why);
        muse_hatch_report(MUSE_HATCH_UNREACHABLE, "Gemini rejected request");
        if (s_turn_active) {
            emit(MUSE_HATCH_EV_ERROR, "GEMINI ERROR");
            s_turn_active = false;
        }
    }
    cJSON *server = member_any(root, "serverContent", "server_content");
    if (cJSON_IsObject(server)) {
        cJSON *input_tx = member_any(server, "inputTranscription", "input_transcription");
        cJSON *input_text = cJSON_IsObject(input_tx) ? cJSON_GetObjectItemCaseSensitive(input_tx, "text") : nullptr;
        if (cJSON_IsString(input_text) && input_text->valuestring[0]) {
            emit(MUSE_HATCH_EV_HEARD, input_text->valuestring);
        }
        cJSON *output_tx = member_any(server, "outputTranscription", "output_transcription");
        cJSON *output_text = cJSON_IsObject(output_tx) ? cJSON_GetObjectItemCaseSensitive(output_tx, "text") : nullptr;
        if (cJSON_IsString(output_text) && output_text->valuestring[0]) {
            reply_append(output_text->valuestring);
            s_reply_started = true;
            emit(MUSE_HATCH_EV_REPLY, output_text->valuestring);
            if (s_text_turn) {
                muse_hatch_console("text", output_text->valuestring, nullptr);
            }
        }
        cJSON *model_turn = member_any(server, "modelTurn", "model_turn");
        cJSON *parts = cJSON_IsObject(model_turn) ? cJSON_GetObjectItemCaseSensitive(model_turn, "parts") : nullptr;
        cJSON *part = nullptr;
        cJSON_ArrayForEach(part, parts) {
            handle_inline_audio(part);
            cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
            if (cJSON_IsString(text) && text->valuestring[0] && !output_text) {
                reply_append(text->valuestring);
                s_reply_started = true;
                emit(MUSE_HATCH_EV_REPLY, text->valuestring);
            }
        }
        cJSON *complete = member_any(server, "turnComplete", "turn_complete");
        if (cJSON_IsTrue(complete)) {
            emit(MUSE_HATCH_EV_DONE, "");
            if (s_text_turn) {
                muse_hatch_console("done", nullptr, "\"complete\":true");
            }
            s_turn_active = false;
            s_text_turn = false;
        }
    }
    cJSON_Delete(root);
}

static bool wait_for_setup(void)
{
    if (!connect_gemini()) {
        return false;
    }
    for (int i = 0; i < 100 && s_connected.load() && !s_setup_complete.load(); ++i) {
        packet_t packet;
        if (xQueueReceive(s_packets, &packet, pdMS_TO_TICKS(100)) == pdTRUE) {
            handle_packet(packet);
            free(packet.data);
        }
    }
    return s_setup_complete.load();
}

static void begin_turn(uint32_t generation)
{
    s_generation.store(generation);
    reply_clear();
    s_reply_started = false;
    s_end_requested = false;
    s_end_sent = false;
    s_text_turn = false;
    if (!wait_for_setup()) {
        emit(MUSE_HATCH_EV_ERROR, "CAN'T CONNECT TO GEMINI");
        return;
    }
    if (!send_json("{\"realtimeInput\":{\"activityStart\":{}}}")) {
        emit(MUSE_HATCH_EV_ERROR, "GEMINI SEND FAILED");
        return;
    }
    s_turn_active = true;
}

static void begin_text_turn(char *text, uint32_t generation)
{
    s_generation.store(generation);
    reply_clear();
    s_reply_started = false;
    s_text_turn = true;
    if (!wait_for_setup()) {
        muse_hatch_console("error", "Can't connect to Gemini", nullptr);
        free(text);
        s_text_turn = false;
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *client = cJSON_AddObjectToObject(root, "clientContent");
    cJSON *turns = cJSON_AddArrayToObject(client, "turns");
    cJSON *turn = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "role", "user");
    cJSON *parts = cJSON_AddArrayToObject(turn, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToArray(turns, turn);
    cJSON_AddBoolToObject(client, "turnComplete", true);
    char *json = cJSON_PrintUnformatted(root);
    bool sent = json && send_json(json);
    cJSON_free(json);
    cJSON_Delete(root);
    muse_hatch_console(sent ? "sent" : "error", sent ? nullptr : "Gemini send failed", nullptr);
    free(text);
    s_turn_active = sent;
    if (!sent) {
        s_text_turn = false;
    }
}

static void session_task(void *)
{
    int64_t reconnect_at = 0;
    for (;;) {
        cmd_t cmd;
        while (xQueueReceive(s_commands, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
            case CMD_CONNECT:
                reconnect_at = 0;
                wait_for_setup();
                break;
            case CMD_FORGET:
                disconnect();
                s_turn_active = false;
                break;
            case CMD_BEGIN:
                begin_turn(cmd.generation);
                break;
            case CMD_END:
                s_end_requested = true;
                break;
            case CMD_CANCEL:
            case CMD_TEXT_CANCEL:
                s_turn_active = false;
                s_text_turn = false;
                xStreamBufferReset(s_input);
                xStreamBufferReset(s_output);
                break;
            case CMD_TEXT:
                begin_text_turn(cmd.text, cmd.generation);
                cmd.text = nullptr;
                break;
            }
            free(cmd.text);
        }

        packet_t packet;
        while (xQueueReceive(s_packets, &packet, 0) == pdTRUE) {
            handle_packet(packet);
            free(packet.data);
        }

        if (s_turn_active && !s_end_sent && s_setup_complete.load()) {
            int16_t pcm[SEND_PCM_FRAMES];
            size_t bytes = xStreamBufferReceive(s_input, pcm, sizeof(pcm), 0);
            if (bytes) {
                send_audio(pcm, bytes / 2);
            } else if (s_end_requested) {
                if (send_json("{\"realtimeInput\":{\"activityEnd\":{}}}")) {
                    s_end_sent = true;
                    emit(MUSE_HATCH_EV_SENT, "");
                } else {
                    emit(MUSE_HATCH_EV_ERROR, "GEMINI SEND FAILED");
                    s_turn_active = false;
                }
            }
        }

        if (!s_connected.load() && s_turn_active) {
            emit(MUSE_HATCH_EV_ERROR, "GEMINI DISCONNECTED");
            s_turn_active = false;
        }
        if (!s_connected.load() && s_ws && !s_turn_active) {
            disconnect();
        }
        if (!s_connected.load() && !s_ws && !s_resting.load() && muse_hatch_configured() &&
            muse_wifi_connected() && esp_timer_get_time() >= reconnect_at) {
            connect_gemini();
            reconnect_at = esp_timer_get_time() + 30000000LL;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void muse_hatch_start(void)
{
    if (s_commands) {
        return;
    }
    s_commands = xQueueCreate(8, sizeof(cmd_t));
    s_events = xQueueCreate(12, sizeof(event_t));
    s_packets = xQueueCreate(8, sizeof(packet_t));
    s_input = xStreamBufferCreateWithCaps(IN_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_output = xStreamBufferCreateWithCaps(OUT_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (!s_commands || !s_events || !s_packets || !s_input || !s_output ||
        xTaskCreatePinnedToCore(session_task, "gemini_live", 8192, nullptr, 5, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "failed to start Gemini task");
        muse_hatch_report(MUSE_HATCH_UNREACHABLE, "Not enough memory");
    }
}

static void post(cmd_type_t type, uint32_t generation = 0, char *text = nullptr)
{
    if (!s_commands) {
        free(text);
        return;
    }
    cmd_t cmd = {type, generation, text};
    if (xQueueSend(s_commands, &cmd, pdMS_TO_TICKS(20)) != pdTRUE) {
        free(text);
    }
}

extern "C" void muse_hatch_chat_connect(void) { post(CMD_CONNECT); }
extern "C" void muse_hatch_chat_forget(void) { post(CMD_FORGET); }
extern "C" bool muse_hatch_ready(void)
{
    return s_commands && muse_hatch_configured() && muse_wifi_connected();
}
extern "C" void muse_hatch_set_resting(bool resting) { s_resting.store(resting); }

extern "C" void muse_hatch_turn_begin(void)
{
    uint32_t generation = s_generation.fetch_add(1) + 1;
    if (s_input) {
        xStreamBufferReset(s_input);
    }
    if (s_output) {
        xStreamBufferReset(s_output);
    }
    post(CMD_BEGIN, generation);
}

extern "C" void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    if (s_input && pcm && frames) {
        xStreamBufferSend(s_input, pcm, frames * 2, 0);
    }
}

extern "C" size_t muse_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    if (!s_input || !pcm || !frames) {
        return 0;
    }
    return xStreamBufferSend(s_input, pcm, frames * 2, pdMS_TO_TICKS(wait_ms)) / 2;
}

extern "C" void muse_hatch_turn_end(void) { post(CMD_END, s_generation.load()); }

extern "C" void muse_hatch_turn_cancel(void)
{
    s_generation.fetch_add(1);
    post(CMD_CANCEL, s_generation.load());
}

extern "C" muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    if (!s_events) {
        return MUSE_HATCH_EV_NONE;
    }
    event_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        if (ev.generation != s_generation.load()) {
            continue;
        }
        if (text && cap) {
            strlcpy(text, ev.text, cap);
        }
        return ev.type;
    }
    return MUSE_HATCH_EV_NONE;
}

extern "C" bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    char copy[REPLY_TEXT];
    portENTER_CRITICAL(&s_reply_lock);
    strlcpy(copy, s_reply, sizeof(copy));
    portEXIT_CRITICAL(&s_reply_lock);
    if (!copy[0]) {
        return false;
    }
    /* Native audio has no word timing. Approximate Vietnamese reading at 16 chars/s. */
    size_t at = played ? (played * 16 / MIC_RATE) : 0;
    if (at >= strlen(copy)) {
        at = strlen(copy) - 1;
    }
    return muse_hatch_caption_at(copy, at, out, cap);
}

extern "C" size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    if (!s_output || !pcm || !frames) {
        return 0;
    }
    return xStreamBufferReceive(s_output, pcm, frames * 2, pdMS_TO_TICKS(wait_ms)) / 2;
}

extern "C" size_t muse_hatch_mp3_selftest(int16_t **pcm)
{
    if (pcm) {
        *pcm = nullptr;
    }
    ESP_LOGW(TAG, "MP3 self-test is unavailable with Gemini native audio");
    return 0;
}

extern "C" void muse_hatch_text_turn(char *text)
{
    uint32_t generation = s_generation.fetch_add(1) + 1;
    post(CMD_TEXT, generation, text);
}

extern "C" void muse_hatch_text_cancel(void)
{
    s_generation.fetch_add(1);
    post(CMD_TEXT_CANCEL, s_generation.load());
}
