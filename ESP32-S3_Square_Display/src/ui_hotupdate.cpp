// Runtime hot-update helpers for updating backgrounds and icons without reboot
#include "ui.h"
#include "screen_config_c_api.h"
#include "number_display.h"
#include "dual_number_display.h"
#include "quad_number_display.h"
#include "gauge_number_display.h"
#include "graph_display.h"
#include <lvgl.h>
#include "esp_log.h"
static const char *TAG_UIHOT = "ui_hotupdate";

// Forward declaration for reset function in main.cpp
extern "C" void reset_number_display_tracking(int screen_num);
// Forward declaration for update function in main.cpp
extern "C" void force_update_number_display(int screen_num);

// Forward declarations for embedded image fallbacks (provided by SquareLine ui.h)
extern const char *ui_img_rev_counter_png;
extern const char *ui_img_rev_fuel_png;
extern const char *ui_img_temp_exhaust_png;
extern const char *ui_img_fuel_temp_png;
extern const char *ui_img_oil_temp_png;

static lv_obj_t *get_background_img_obj_for_screen(int s) {
    switch (s) {
        case 0: return ui_RevTemp;
        case 1: return ui_RevFuel;
        case 2: return ui_TempExhaust;
        case 3: return ui_FuelTemp;
        case 4: return ui_OilTemp;
        default: return NULL;
    }
}

// Return a fallback background source for a screen.
// Can be either a string (SD path) or a pointer to an embedded lv_img_dsc_t.
static const void *get_fallback_bg_for_screen(int s) {
    // Prefer a single embedded default image for all screens.
    return &ui_img_default_png;
}

static lv_obj_t *get_top_icon_obj_for_screen(int s) {
    switch (s) {
        case 0: return ui_TopIcon1;
        case 1: return ui_TopIcon2;
        case 2: return ui_TopIcon3;
        case 3: return ui_TopIcon4;
        case 4: return ui_TopIcon5;
        default: return NULL;
    }
}

static lv_obj_t *get_bottom_icon_obj_for_screen(int s) {
    switch (s) {
        case 0: return ui_BottomIcon1;
        case 1: return ui_BottomIcon2;
        case 2: return ui_BottomIcon3;
        case 3: return ui_BottomIcon4;
        case 4: return ui_BottomIcon5;
        default: return NULL;
    }
}

static lv_obj_t *get_upper_needle_obj_for_screen(int s) {
    switch (s) {
        case 0: return ui_Needle;
        case 1: return ui_Needle2;
        case 2: return ui_Needle3;
        case 3: return ui_Needle4;
        case 4: return ui_Needle5;
        default: return NULL;
    }
}

static lv_obj_t *get_lower_needle_obj_for_screen(int s) {
    switch (s) {
        case 0: return ui_Lower_Needle;
        case 1: return ui_Lower_Needle2;
        case 2: return ui_Lower_Needle3;
        case 3: return ui_Lower_Needle4;
        case 4: return ui_Lower_Needle5;
        default: return NULL;
    }
}

// Apply the background for a single screen. Returns true if object exists and update attempted.
bool apply_background_for_screen(int s) {
    lv_obj_t *bg = get_background_img_obj_for_screen(s);
    if (!bg) return false;
    const char *path = screen_configs[s].background_path;
    if (path && path[0] != '\0') {
        // Log path and LVGL source type for debugging unknown-image warnings
        ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d background path='%s' src_type=%d", s, path, (int)lv_img_src_get_type((const void*)path));
        lv_img_set_src(bg, path);
    } else {
        const void *fb = get_fallback_bg_for_screen(s);
        if (fb) {
            ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d background using fallback src_type=%d", s, (int)lv_img_src_get_type(fb));
            lv_img_set_src(bg, fb);
            lv_obj_clear_flag(bg, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d background none", s);
            lv_obj_add_flag(bg, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_invalidate(bg);
    return true;
}

// Apply top/bottom icon images for a single screen
bool apply_icons_for_screen(int s) {
    lv_obj_t *top = get_top_icon_obj_for_screen(s);
    lv_obj_t *bot = get_bottom_icon_obj_for_screen(s);
    bool any = false;
    if (top) {
        const char *p = screen_configs[s].icon_paths[0];
        ESP_LOGI(TAG_UIHOT, "[LVGL IMG DEBUG] screen=%d top icon ptr=%p path='%s' len=%d", s, p, (p ? p : "NULL"), (p ? strlen(p) : -1));
        if (p && p[0] != '\0') {
            ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d top icon path='%s' src_type=%d", s, p, (int)lv_img_src_get_type((const void*)p));
            lv_img_set_src(top, p);
            lv_obj_set_style_img_opa(top, LV_OPA_COVER, 0);  // Make fully opaque
            lv_obj_clear_flag(top, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d top icon none - HIDING", s);
            lv_obj_set_style_img_opa(top, LV_OPA_TRANSP, 0);  // Make completely transparent
            lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_invalidate(top);
        any = true;
    }
    if (bot) {
        // Respect per-screen show_bottom flag: hide bottom icon if disabled
        if (!screen_configs[s].show_bottom) {
            ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d bottom icon hidden", s);
            lv_obj_add_flag(bot, LV_OBJ_FLAG_HIDDEN);
        } else {
            const char *p = screen_configs[s].icon_paths[1];
            if (p && p[0] != '\0') {
                ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d bottom icon path='%s' src_type=%d", s, p, (int)lv_img_src_get_type((const void*)p));
                lv_img_set_src(bot, p);
                lv_obj_set_style_img_opa(bot, LV_OPA_COVER, 0);  // Make fully opaque
                lv_obj_clear_flag(bot, LV_OBJ_FLAG_HIDDEN);
            } else {
                ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d bottom icon none", s);
                lv_obj_set_style_img_opa(bot, LV_OPA_TRANSP, 0);  // Make completely transparent
                lv_obj_add_flag(bot, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_invalidate(bot);
        }
        any = true;
    }
    // Also hide/show the lower needle line object (the actual second gauge needle)
    lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
    if (lower_needle) {
        if (!screen_configs[s].show_bottom) {
            lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
        }
        any = true;
    }
    return any;
}

// Apply visuals for all screens. Returns true if at least one target object was present.
bool apply_all_screen_visuals() {
    ESP_LOGI(TAG_UIHOT, "[APPLY_ALL] Starting apply_all_screen_visuals()");
    bool any = false;
    for (int s = 0; s < NUM_SCREENS; ++s) {
        ESP_LOGI(TAG_UIHOT, "[APPLY_ALL] Processing screen %d, display_type=%d", s, screen_configs[s].display_type);
        bool a = apply_background_for_screen(s);
        // Skip generic icon handling for GAUGE_NUMBER - it handles icons itself  
        bool b = false;
        if (screen_configs[s].display_type != DISPLAY_TYPE_GAUGE_NUMBER) {
            b = apply_icons_for_screen(s);
        }
        any = any || a || b;
        
        // If this screen is set to number display mode, recreate the number display
        // to apply any changes (font size, color, path, etc.)
        if (screen_configs[s].display_type == DISPLAY_TYPE_NUMBER) {
            // Destroy other display types first
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            // Hide gauge needles (not used in number display)
            lv_obj_t *upper_needle = get_upper_needle_obj_for_screen(s);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (upper_needle) lv_obj_add_flag(upper_needle, LV_OBJ_FLAG_HIDDEN);
            if (lower_needle) lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            lv_obj_t *bot_icon = get_bottom_icon_obj_for_screen(s);
            if (top_icon) lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
            if (bot_icon) lv_obj_add_flag(bot_icon, LV_OBJ_FLAG_HIDDEN);
            number_display_create(s);
            // Reset tracking to force immediate update with current sensor data
            reset_number_display_tracking(s + 1);  // +1 because reset function expects 1-5
            // Force immediate update so description and units appear right away
            force_update_number_display(s + 1);
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_DUAL) {
            // Destroy other display types first
            number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            // Hide gauge needles (not used in dual display)
            lv_obj_t *upper_needle = get_upper_needle_obj_for_screen(s);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (upper_needle) lv_obj_add_flag(upper_needle, LV_OBJ_FLAG_HIDDEN);
            if (lower_needle) lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            lv_obj_t *bot_icon = get_bottom_icon_obj_for_screen(s);
            if (top_icon) lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
            if (bot_icon) lv_obj_add_flag(bot_icon, LV_OBJ_FLAG_HIDDEN);
            // Recreate dual display with updated settings (reads from screen_configs)
            dual_number_display_create(s);
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_QUAD) {
            // Destroy other display types first
            number_display_destroy(s);
            dual_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            // Hide gauge needles (not used in quad display)
            lv_obj_t *upper_needle = get_upper_needle_obj_for_screen(s);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (upper_needle) lv_obj_add_flag(upper_needle, LV_OBJ_FLAG_HIDDEN);
            if (lower_needle) lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            lv_obj_t *bot_icon = get_bottom_icon_obj_for_screen(s);
            if (top_icon) lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
            if (bot_icon) lv_obj_add_flag(bot_icon, LV_OBJ_FLAG_HIDDEN);
            // Recreate quad display with updated settings (reads from screen_configs)
            quad_number_display_create(s);
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_GAUGE_NUMBER) {
            ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] Handling screen=%d as GAUGE_NUMBER", s);
            // Destroy other display types first
            number_display_destroy(s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            graph_display_destroy(s);
            // Hide the bottom gauge needle (gauge+number only shows top gauge)
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (lower_needle) {
                lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_t *bot = get_bottom_icon_obj_for_screen(s);
            if (bot) {
                lv_obj_add_flag(bot, LV_OBJ_FLAG_HIDDEN);
            }
            // Handle top icon visibility based on icon_paths[0]
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            const char *p = screen_configs[s].icon_paths[0];
            ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] screen=%d icon_paths[0]=%p, value='%s', len=%d",
                s, p, (p ? p : "NULL"), (p ? strlen(p) : -1));
            if (top_icon) {
                ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] screen=%d top_icon object exists at %p", s, top_icon);
                if (p && p[0] != '\0') {
                    ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] screen=%d SHOWING icon: '%s'", s, p);
                    lv_img_set_src(top_icon, p);
                    lv_obj_set_style_img_opa(top_icon, LV_OPA_COVER, 0);  // Make fully opaque
                    lv_obj_set_size(top_icon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);  // Restore size
                    lv_obj_align(top_icon, LV_ALIGN_CENTER, 0, -70);  // Restore position
                    lv_obj_clear_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
                } else {
                    ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] screen=%d HIDING icon (empty path)", s);
                    lv_obj_set_style_img_opa(top_icon, LV_OPA_TRANSP, 0);  // Make completely transparent
                    lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_pos(top_icon, -5000, -5000);  // Move way off screen
                    lv_obj_set_size(top_icon, 0, 0);  // Make zero size
                    // Verify it's actually hidden
                    bool is_hidden = lv_obj_has_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
                    ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] screen=%d icon hidden flag set: %s", s, is_hidden ? "YES" : "NO");
                }
            } else {
                ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] screen=%d top_icon object is NULL!", s);
            }
            // Recreate gauge+number display with updated settings
            gauge_number_display_create(
                s,
                screen_configs[s].gauge_num_center_font_size,
                screen_configs[s].gauge_num_center_font_color
            );
            ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] screen=%d gauge_number_display_create() completed", s);
            // Double-check icon is still hidden after display creation
            if (top_icon && (!p || p[0] == '\0')) {
                bool still_hidden = lv_obj_has_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
                ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] screen=%d AFTER display create, icon still hidden: %s", s, still_hidden ? "YES" : "NO");
                if (!still_hidden) {
                    ESP_LOGE(TAG_UIHOT, "[GAUGE_NUM] screen=%d WARNING: Icon was unhidden by display creation! Re-hiding...", s);
                    lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
                }
            }
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_GRAPH) {
            // Destroy other display types first
            number_display_destroy(s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            // Hide gauge needles (not used in graph display)
            lv_obj_t *upper_needle = get_upper_needle_obj_for_screen(s);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (upper_needle) lv_obj_add_flag(upper_needle, LV_OBJ_FLAG_HIDDEN);
            if (lower_needle) lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            lv_obj_t *bot_icon = get_bottom_icon_obj_for_screen(s);
            if (top_icon) lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
            if (bot_icon) lv_obj_add_flag(bot_icon, LV_OBJ_FLAG_HIDDEN);
            // Recreate graph display
            graph_display_create(s);
            any = true;
        } else {
            // Display type is GAUGE - destroy all number/dual/quad/gauge-number/graph displays to show gauges
            number_display_destroy(s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            // Show both gauge needles for regular gauge display
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (lower_needle) {
                if (screen_configs[s].show_bottom) {
                    lv_obj_clear_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
    // Only force refresh if changes were made, for immediate user feedback
    if (any) {
        lv_refr_now(NULL);
    }
    ESP_LOGI(TAG_UIHOT, "[APPLY_ALL] Completed apply_all_screen_visuals(), returning %s", any ? "true" : "false");
    return any;
}
