#include <assert.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_lvgl_port.h"
#include "sensecap-watcher.h"
#include "board.h"

static const char *TAG = "board";

/**
 * Recover the touch I2C bus. bsp_i2c_bus_init() configures the touch I2C pins
 * (GPIO 38/39) as outputs driven low, which can leave the SPD2010 touch
 * controller in a stuck state. Toggle SCL 9 times followed by a STOP condition
 * to force any stuck slave to release the bus.
 */
static void touch_i2c_bus_recover(void)
{
    gpio_set_direction(BSP_TOUCH_I2C_SDA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BSP_TOUCH_I2C_SDA, GPIO_PULLUP_ONLY);
    gpio_set_direction(BSP_TOUCH_I2C_SCL, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(BSP_TOUCH_I2C_SCL, GPIO_PULLUP_ONLY);

    for (int i = 0; i < 9; i++) {
        gpio_set_level(BSP_TOUCH_I2C_SCL, 1);
        esp_rom_delay_us(5);
        gpio_set_level(BSP_TOUCH_I2C_SCL, 0);
        esp_rom_delay_us(5);
    }

    /* STOP condition: SDA transitions low-to-high while SCL is high */
    gpio_set_direction(BSP_TOUCH_I2C_SDA, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_TOUCH_I2C_SDA, 0);
    esp_rom_delay_us(5);
    gpio_set_level(BSP_TOUCH_I2C_SCL, 1);
    esp_rom_delay_us(5);
    gpio_set_level(BSP_TOUCH_I2C_SDA, 1);
    esp_rom_delay_us(5);
}

void board_init()
{
    ESP_LOGI(TAG, "Initializing board");

    // Initialize codec using SenseCAP Watcher BSP
    ESP_ERROR_CHECK(bsp_codec_init());

    // Recover touch I2C bus before LVGL init attempts to talk to the SPD2010
    touch_i2c_bus_recover();

    // Initialize display and LVGL.
    // BSP_LCD_DEFAULT_BRIGHTNESS=0 keeps backlight off during init.
    // screen_init() turns it on after painting the first frame.
    lv_disp_t *disp = bsp_lvgl_init();
    assert(disp);

    // Immediately paint black and kill backlight — prevents white flash
    // if the sdkconfig brightness default wasn't applied.
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    lvgl_port_unlock();
    bsp_lcd_brightness_set(0);
}

/// Find the encoder input device registered by the BSP.
static lv_indev_t *find_encoder(void)
{
    lv_indev_t *indev = NULL;
    while (1) {
        indev = lv_indev_get_next(indev);
        if (indev == NULL || indev->driver->type == LV_INDEV_TYPE_ENCODER)
            break;
    }
    return indev;
}

static void btn_cb_wrapper(void *arg, void *arg2)
{
    void (*cb)(void) = arg2;
    if (cb) cb();
}

void board_set_btn_press_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (enc == NULL) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_PRESS_DOWN, btn_cb_wrapper, cb);
}

void board_set_btn_release_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (enc == NULL) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_PRESS_UP, btn_cb_wrapper, cb);
}

void board_set_lcd_brightness(int percent)
{
    bsp_lcd_brightness_set(percent);
}
