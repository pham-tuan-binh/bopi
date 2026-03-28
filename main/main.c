#include "esp_heap_caps.h"
#include "esp_log.h"
#include "board.h"
#include "esp_netif_sntp.h"
#include "bopi.h"
#include "livekit_example_utils.h"
#include "media.h"
#include "screen.h"

#include "livekit.h"

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    livekit_system_init();
    board_init();
    screen_init();
    media_init();
    if (lk_example_network_connect()) {
        // Init SNTP after network is up — if started before, the first query
        // fails and the retry interval is 1 hour, so sync_wait always times out.
        esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
            ESP_SNTP_SERVER_LIST("time.google.com", "pool.ntp.org"));
        esp_netif_sntp_init(&sntp_config);
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
            ESP_LOGW("main", "NTP sync timed out — proceeding anyway");
        }

        ESP_LOGI("main", "Free internal heap: %lu (min: %lu)",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        bopi_init();
    }
}
