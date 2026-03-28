#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "livekit.h"
#include "livekit_sandbox.h"
#include "media.h"
#include "board.h"
#include "bopi.h"
#include "screen.h"

static const char *TAG = "bopi";

static livekit_room_handle_t room_handle;
static bool s_in_room;
static bool s_mic_muted;

// Long-press detection (FreeRTOS one-shot timer)
#define LONG_PRESS_MS 1000
static TimerHandle_t s_press_timer;
static bool s_long_press_fired;

// ── Forward declarations ────────────────────────────────────────────
static void join_room(void);

// ── Transcription stream callbacks ──────────────────────────────────

static void on_transcription_open(const livekit_data_stream_header_t *header, void *ctx)
{
    ESP_LOGI(TAG, "Transcription stream opened: stream_id=%s sender=%s",
             header->stream_id, header->sender_identity);
}

static void on_transcription_recv(const livekit_data_stream_chunk_t *chunk, void *ctx)
{
    const char *start = (const char *)chunk->content;
    const char *end = start + chunk->content_size;

    // Trim whitespace.
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r'))
        start++;
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t' || *(end - 1) == '\n' || *(end - 1) == '\r'))
        end--;

    int len = end - start;
    if (len == 0 || !s_in_room)
        return;

    // Skip chunks with more than one word.
    for (const char *p = start; p < end; p++) {
        if (*p == ' ' || *p == '\t')
            return;
    }

    char buf[256];
    if (len > (int)sizeof(buf) - 1)
        len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';

    ESP_LOGI(TAG, "Transcription: %s", buf);

    // Try to trigger a matching animation; show word as text otherwise.
    if (!screen_show_gif_by_name(buf)) {
        screen_show_text(buf);
    }
}

static void on_transcription_close(const livekit_data_stream_trailer_t *trailer, void *ctx)
{
    ESP_LOGI(TAG, "Transcription stream closed: stream_id=%s reason=%s",
             trailer->stream_id, trailer->reason);
}

// ── Room state callback ─────────────────────────────────────────────

static void on_state_changed(livekit_connection_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "Room state changed: %s", livekit_connection_state_str(state));

    livekit_failure_reason_t reason = livekit_room_get_failure_reason(room_handle);
    if (reason != LIVEKIT_FAILURE_REASON_NONE) {
        ESP_LOGE(TAG, "Failure reason: %s", livekit_failure_reason_str(reason));
    }
}

// ── Agent events stream (lk.agent.events) ──────────────────────────

/// Go back to idle state when agent disconnects or room drops.
static void go_idle(void)
{
    if (!s_in_room)
        return;
    s_in_room = false;
    media_set_mic_muted(true);
    s_mic_muted = true;
    screen_set_mic_indicator(false);
    screen_show_blank();
    screen_show_text("Tap to talk");
}

static void on_agent_event_recv(const livekit_data_stream_chunk_t *chunk, void *ctx)
{
    if (!chunk->content || chunk->content_size == 0)
        return;

    char buf[512];
    int len = chunk->content_size;
    if (len > (int)sizeof(buf) - 1)
        len = sizeof(buf) - 1;
    memcpy(buf, chunk->content, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root)
        return;

    ESP_LOGI(TAG, "Agent event: %s", buf);

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const cJSON *new_state = cJSON_GetObjectItem(root, "new_state");

    if (cJSON_IsString(type) && strcmp(type->valuestring, "user_state_changed") == 0 &&
        cJSON_IsString(new_state) && strcmp(new_state->valuestring, "away") == 0) {
        ESP_LOGW(TAG, "Agent going away — returning to idle");
        go_idle();
    }

    cJSON_Delete(root);
}

// ── Room lifecycle ──────────────────────────────────────────────────

static void join_room(void)
{
    if (room_handle != NULL) {
        ESP_LOGE(TAG, "Room already created");
        return;
    }

    livekit_room_options_t room_options = {
        .publish = {
            .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
            .audio_encode = {
                .codec = LIVEKIT_AUDIO_CODEC_OPUS,
                .sample_rate = 16000,
                .channel_count = 1},
            .capturer = media_get_capturer()},
        .subscribe = {.kind = LIVEKIT_MEDIA_TYPE_AUDIO, .renderer = media_get_renderer()},
        .on_state_changed = on_state_changed};
    if (livekit_room_create(&room_handle, &room_options) != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to create room");
        return;
    }

    livekit_data_stream_handler_t transcription_handler = {
        .on_recv = on_transcription_recv,
        .on_open = on_transcription_open,
        .on_close = on_transcription_close,
    };
    if (livekit_room_data_stream_topic_register(room_handle, "lk.transcription", &transcription_handler) != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to register transcription stream handler");
    }

    livekit_data_stream_handler_t agent_event_handler = {
        .on_recv = on_agent_event_recv,
    };
    if (livekit_room_data_stream_topic_register(room_handle, "lk.agent.events", &agent_event_handler) != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to register agent events stream handler");
    }

    livekit_err_t connect_res;
#ifdef CONFIG_LK_BOPI_USE_SANDBOX
    livekit_sandbox_res_t res = {};
    livekit_sandbox_options_t gen_options = {
        .sandbox_id = CONFIG_LK_BOPI_SANDBOX_ID,
        .room_name = CONFIG_LK_BOPI_ROOM_NAME,
        .participant_name = CONFIG_LK_BOPI_PARTICIPANT_NAME};
    if (!livekit_sandbox_generate(&gen_options, &res)) {
        ESP_LOGE(TAG, "Failed to generate sandbox token");
        livekit_room_destroy(room_handle);
        room_handle = NULL;
        return;
    }
    connect_res = livekit_room_connect(room_handle, res.server_url, res.token);
    livekit_sandbox_res_free(&res);
#else
    connect_res = livekit_room_connect(
        room_handle,
        CONFIG_LK_BOPI_SERVER_URL,
        CONFIG_LK_BOPI_TOKEN);
#endif

    if (connect_res != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to connect to room");
    }
}

// ── Button callbacks ────────────────────────────────────────────────

static void connect_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Connecting to room...");
    screen_show_text("Connecting...");
    room_handle = NULL;
    join_room();
    s_in_room = true;
    s_mic_muted = true;
    media_set_mic_muted(true);
    screen_show_text("");
    vTaskDelete(NULL);
}

static void restart_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Long press — closing room and restarting");
    if (room_handle != NULL) {
        livekit_room_close(room_handle);
    }
    // Brief delay for the close to propagate to server
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void press_timer_cb(TimerHandle_t t)
{
    (void)t;
    s_long_press_fired = true;
    xTaskCreate(restart_task, "restart", 4096, NULL, 5, NULL);
}

static void on_btn_press(void)
{
    s_long_press_fired = false;
    xTimerReset(s_press_timer, 0);
}

static void on_btn_release(void)
{
    xTimerStop(s_press_timer, 0);
    if (s_long_press_fired)
        return;

    if (!s_in_room) {
        // Tap to connect
        xTaskCreate(connect_task, "connect", 8192, NULL, 5, NULL);
    } else {
        // Tap to toggle mute
        s_mic_muted = !s_mic_muted;
        media_set_mic_muted(s_mic_muted);
        screen_set_mic_indicator(!s_mic_muted);
        ESP_LOGI(TAG, "Mic %s", s_mic_muted ? "muted" : "unmuted");
    }
}

// ── Public entry point ──────────────────────────────────────────────

void bopi_init(void)
{
    s_press_timer = xTimerCreate("lpress", pdMS_TO_TICKS(LONG_PRESS_MS),
                                  pdFALSE, NULL, press_timer_cb);

    board_set_btn_press_cb(on_btn_press);
    board_set_btn_release_cb(on_btn_release);

    // Start idle — tap to connect.
    s_in_room = false;
    s_mic_muted = true;
    media_set_mic_muted(true);
    screen_show_text("Tap to talk");
}
