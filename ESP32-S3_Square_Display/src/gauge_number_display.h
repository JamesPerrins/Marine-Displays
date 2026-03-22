#ifndef GAUGE_NUMBER_DISPLAY_H
#define GAUGE_NUMBER_DISPLAY_H

#include <lvgl.h>

// Create gauge+number display for a screen (gauge on top, number in center)
void gauge_number_display_create(int screen_num, 
                                   uint8_t center_font_size, 
                                   const char* center_font_color);

// Update center number display with new value, units, and description.
// display_name: Signal K meta.displayName — used as label if non-empty,
//               otherwise falls back to acronym derived from description.
void gauge_number_display_update_center(int screen_num, float value, const char* unit, const char* description, const char* display_name = "");

// Destroy gauge+number display for a screen
void gauge_number_display_destroy(int screen_num);

#endif // GAUGE_NUMBER_DISPLAY_H
