#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize the screen UI.
void screen_init(void);

/// Show text at the bottom of the screen, replacing any previous text.
void screen_show_text(const char *text);

/// Show blank.gif and reset animation lock.
void screen_show_blank(void);

/// Show a GIF whose base filename (without .gif) matches \p name (case-insensitive).
/// Returns true if a matching GIF was found and displayed.
bool screen_show_gif_by_name(const char *name);

/// Show or hide the red mic-recording indicator at the bottom of the screen.
void screen_set_mic_indicator(bool recording);

#ifdef __cplusplus
}
#endif
