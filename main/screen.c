#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sensecap-watcher.h"
#include "board.h"
#include "screen.h"

static const char *TAG = "screen";

#define SD_MOUNT_POINT "/sdcard"
#define GIF_DIR        SD_MOUNT_POINT
#define MAX_GIFS       100
#define READ_CHUNK_SIZE  (8 * 1024)

static lv_obj_t *s_label;
static lv_obj_t *s_gif;
static lv_obj_t *s_mic_indicator;

static char *s_gif_files[MAX_GIFS];
static char *s_gif_names[MAX_GIFS]; // lowercase base names without .gif
static int s_gif_count;

static uint8_t *s_gif_data;
static lv_img_dsc_t s_gif_dsc;

/// Cached blank.gif data (always in memory).
static uint8_t *s_blank_data;
static size_t s_blank_size;

/// DMA-capable bounce buffer reused across reads.
static uint8_t *s_dma_buf;

/// Background task that handles blocking SD card reads.
static TaskHandle_t s_loader_task;

/// True while a non-blank GIF is loading/playing — blocks new triggers.
static volatile bool s_playing;

/// Max time (ms) to wait for a GIF to finish before force-resetting s_playing.
/// Guards against looping GIFs that never fire LV_EVENT_READY.
#define ANIMATION_TIMEOUT_MS 10000


static void scan_gif_files(void)
{
    DIR *dir = opendir(GIF_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory %s", GIF_DIR);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_gif_count < MAX_GIFS) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcasecmp(entry->d_name + len - 4, ".gif") == 0
                && strncmp(entry->d_name, "._", 2) != 0
                && strcasecmp(entry->d_name, "blank.gif") != 0) {
            char path[256];
            snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", entry->d_name);
            s_gif_files[s_gif_count] = strdup(path);
            if (s_gif_files[s_gif_count]) {
                // Store lowercase base name (without .gif extension)
                int namelen = (int)len - 4;
                char *name = malloc(namelen + 1);
                if (name) {
                    for (int i = 0; i < namelen; i++)
                        name[i] = tolower((unsigned char)entry->d_name[i]);
                    name[namelen] = '\0';
                    s_gif_names[s_gif_count] = name;
                }
                ESP_LOGI(TAG, "Found GIF: %s [%s]", s_gif_files[s_gif_count],
                         s_gif_names[s_gif_count] ? s_gif_names[s_gif_count] : "?");
                s_gif_count++;
            }
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "Found %d GIF files", s_gif_count);
}

/// Read file into SPIRAM via DMA bounce buffer. Returns size or 0 on failure.
/// Only called from screen_init (during boot) and loader_task — never concurrently.
static size_t read_file(const char *path, uint8_t **out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;

    uint8_t *buf = malloc(st.st_size);
    if (!buf)
        return 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(buf);
        return 0;
    }

    uint32_t t0 = xTaskGetTickCount();

    size_t total = 0;
    while (total < (size_t)st.st_size) {
        size_t to_read = (size_t)st.st_size - total;
        if (to_read > READ_CHUNK_SIZE)
            to_read = READ_CHUNK_SIZE;
        ssize_t n = read(fd, s_dma_buf, to_read);
        if (n <= 0)
            break;
        memcpy(buf + total, s_dma_buf, n);
        total += n;
    }
    close(fd);

    uint32_t elapsed = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "Read %u bytes in %lu ms (%.1f KB/s)",
             (unsigned)total, (unsigned long)elapsed,
             elapsed > 0 ? (total / 1024.0f) / (elapsed / 1000.0f) : 0);

    if (total != (size_t)st.st_size) {
        free(buf);
        return 0;
    }

    *out = buf;
    return total;
}

/// Try to read a file, remounting the SD card on failure (SPI bus recovery).
static size_t read_file_with_retry(const char *path, uint8_t **out)
{
    size_t size = read_file(path, out);
    if (size > 0)
        return size;

    ESP_LOGW(TAG, "Read failed, remounting SD card");
    bsp_sdcard_deinit_default();
    vTaskDelay(pdMS_TO_TICKS(200));
    if (bsp_sdcard_init_default() != ESP_OK)
        return 0;

    return read_file(path, out);
}

/// Display a GIF from an in-memory buffer. Caller must hold the LVGL lock.
static void show_gif_data_locked(const uint8_t *data, size_t size)
{
    s_gif_dsc.header.always_zero = 0;
    s_gif_dsc.header.cf = LV_IMG_CF_RAW;
    s_gif_dsc.header.w = 0;
    s_gif_dsc.header.h = 0;
    s_gif_dsc.data_size = size;
    s_gif_dsc.data = data;

    lv_gif_set_src(s_gif, &s_gif_dsc);

    // Scale to screen width
    lv_img_dsc_t *dsc = (lv_img_dsc_t *)lv_img_get_src(s_gif);
    if (dsc && dsc->header.w > 0) {
        uint16_t zoom = (uint16_t)((DRV_LCD_H_RES * 256) / dsc->header.w);
        lv_img_set_zoom(s_gif, zoom);
    }
    lv_obj_center(s_gif);
}

/// Show the idle blank GIF and unlock animation playback.
/// Caller must hold the LVGL lock.
static void show_blank_locked(void)
{
    if (!s_blank_data)
        return;
    s_playing = false;
    show_gif_data_locked(s_blank_data, s_blank_size);
    ESP_LOGI(TAG, "Showing blank");
}

/// Called when a non-looping GIF finishes — return to blank.
static void gif_done_cb(lv_event_t *e)
{
    (void)e;
    show_blank_locked();
}

/// Background task: waits for a notification, loads and displays a GIF.
static void loader_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t val;
        BaseType_t got = xTaskNotifyWait(0, ULONG_MAX, &val,
                                          pdMS_TO_TICKS(ANIMATION_TIMEOUT_MS));

        // Timeout: a looping GIF may have locked s_playing — force reset.
        if (got == pdFALSE) {
            if (s_playing) {
                ESP_LOGW(TAG, "Animation timeout — resetting playback lock");
                lvgl_port_lock(0);
                show_blank_locked();
                lvgl_port_unlock();
            }
            continue;
        }

        if (s_gif_count == 0)
            continue;

        int idx = (int)val;
        if (idx < 0 || idx >= s_gif_count)
            continue;
        ESP_LOGI(TAG, "Loading: %s", s_gif_files[idx]);

        uint8_t *data = NULL;
        size_t size = read_file_with_retry(s_gif_files[idx], &data);
        if (!data) {
            ESP_LOGE(TAG, "Failed to read after SD remount");
            lvgl_port_lock(0);
            show_blank_locked();
            lvgl_port_unlock();
            continue;
        }

        lvgl_port_lock(0);
        free(s_gif_data);
        s_gif_data = data;
        show_gif_data_locked(data, size);
        lvgl_port_unlock();

    }
}

void screen_init(void)
{
    esp_err_t err = bsp_sdcard_init_default();
    if (err == ESP_OK) {
        scan_gif_files();
    } else {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
    }

    // Allocate reusable DMA bounce buffer
    s_dma_buf = heap_caps_malloc(READ_CHUNK_SIZE, MALLOC_CAP_DMA);

    // Background task for non-blocking GIF loading
    xTaskCreate(loader_task, "gif_loader", 4096, NULL, 5, &s_loader_task);

    // Preload blank.gif — stays in memory permanently
    s_blank_size = read_file(SD_MOUNT_POINT "/blank.gif", &s_blank_data);
    if (!s_blank_data) {
        ESP_LOGW(TAG, "blank.gif not found on SD card");
    }

    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_gif = lv_gif_create(scr);
    lv_obj_center(s_gif);
    lv_obj_add_event_cb(s_gif, gif_done_cb, LV_EVENT_READY, NULL);

    s_label = lv_label_create(scr);
    lv_label_set_text(s_label, "");
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_label, lv_pct(90));
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s_label, LV_OPA_60, 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_label, LV_ALIGN_BOTTOM_MID, 0, -40);

    // Red mic-recording indicator (hidden by default)
    s_mic_indicator = lv_obj_create(scr);
    lv_obj_remove_style_all(s_mic_indicator);
    lv_obj_set_size(s_mic_indicator, 12, 12);
    lv_obj_set_style_radius(s_mic_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mic_indicator, lv_color_make(255, 0, 0), 0);
    lv_obj_set_style_bg_opa(s_mic_indicator, LV_OPA_COVER, 0);
    lv_obj_align(s_mic_indicator, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(s_mic_indicator, LV_OBJ_FLAG_HIDDEN);

    // Show blank.gif immediately at boot
    if (s_blank_data) {
        show_gif_data_locked(s_blank_data, s_blank_size);
    } else if (s_gif_count == 0) {
        lv_label_set_text(s_label, "No GIFs found on SD card");
    }

    lvgl_port_unlock();

    // Turn backlight on now — blank.gif (or black) is already rendered
    board_set_lcd_brightness(100);
}

void screen_show_text(const char *text)
{
    // Filter to printable ASCII — the font has no Unicode glyphs.
    char buf[256];
    int j = 0;
    for (int i = 0; text[i] && j < (int)sizeof(buf) - 1; i++) {
        if (text[i] >= 0x20 && text[i] <= 0x7E)
            buf[j++] = text[i];
    }
    buf[j] = '\0';

    lvgl_port_lock(0);
    lv_label_set_text(s_label, buf);
    lvgl_port_unlock();
}

void screen_show_blank(void)
{
    lvgl_port_lock(0);
    s_playing = false;
    if (s_blank_data)
        show_gif_data_locked(s_blank_data, s_blank_size);
    free(s_gif_data);
    s_gif_data = NULL;
    lvgl_port_unlock();
}

bool screen_show_gif_by_name(const char *name)
{
    if (s_gif_count == 0 || !name || !*name)
        return false;

    // Lowercase + strip trailing punctuation (e.g. "angry!" -> "angry")
    char lower[64];
    int i;
    for (i = 0; name[i] && i < (int)sizeof(lower) - 1; i++)
        lower[i] = tolower((unsigned char)name[i]);
    while (i > 0 && !isalpha((unsigned char)lower[i - 1]))
        i--;
    lower[i] = '\0';
    if (i == 0)
        return false;

    for (int j = 0; j < s_gif_count; j++) {
        if (s_gif_names[j] && strcmp(s_gif_names[j], lower) == 0) {
            if (!s_playing) {
                s_playing = true;
                xTaskNotify(s_loader_task, (uint32_t)j, eSetValueWithOverwrite);
            }
            return true;
        }
    }
    return false;
}

void screen_set_mic_indicator(bool recording)
{
    lvgl_port_lock(0);
    if (recording)
        lv_obj_clear_flag(s_mic_indicator, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_mic_indicator, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}
