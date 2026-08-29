#include "fish_ui_style.h"

#include "fonts/fish_font_24.h"
#include "lvgl.h"

static lv_style_t s_default_font_style;
static bool s_style_ready = false;

void fish_ui_apply_default_font(lv_obj_t *root)
{
    if (!s_style_ready) {
        lv_style_init(&s_default_font_style);
        lv_style_set_text_font(&s_default_font_style, &fish_font_24);
        s_style_ready = true;
    }
    if (root) {
        lv_obj_add_style(root, &s_default_font_style, LV_PART_MAIN);
    }
}
