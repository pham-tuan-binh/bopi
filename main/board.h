#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize board.
void board_init(void);

/// Register a callback for when the knob button is pressed down.
void board_set_btn_press_cb(void (*cb)(void));

/// Register a callback for when the knob button is released.
void board_set_btn_release_cb(void (*cb)(void));

/// Set LCD backlight brightness (0–100%).
void board_set_lcd_brightness(int percent);

#ifdef __cplusplus
}
#endif
