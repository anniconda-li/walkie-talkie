#include "ui_app_settings.h"
#include "ui.h"
#include "ui_event.h"
#include "ui_i18n.h"

typedef struct {
    lv_anim_completed_cb_t done_cb;
    lv_obj_t * root;
} app_exit_ctx_t;

static ui_settings_view_t g_settings_view;
static lv_obj_t *g_settings_panel;

static void anim_set_y(void *obj, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)obj, y);
}

static void start_y_anim(lv_obj_t *obj,
                         int32_t from,
                         int32_t to,
                         uint32_t duration,
                         uint32_t delay,
                         lv_anim_path_cb_t path_cb,
                         lv_anim_completed_cb_t done_cb,
                         void *user_data)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, anim_set_y);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_path_cb(&anim, path_cb);
    if(done_cb != NULL) {
        lv_anim_set_user_data(&anim, user_data);
        lv_anim_set_completed_cb(&anim, done_cb);
    }
    lv_anim_start(&anim);
}

static void app_exit_done_cb(lv_anim_t *a)
{
    app_exit_ctx_t *ctx = (app_exit_ctx_t *)lv_anim_get_user_data(a);

    if(ctx->root) {
        lv_obj_delete(ctx->root);
    }

    if(ctx->done_cb) {
        ctx->done_cb(a);
    }

    lv_free(ctx);
}

static lv_obj_t *create_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(panel, 14, 48);
    lv_obj_set_size(panel, 212, 252);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_bg_color(panel, lv_color_make(0x24, 0x24, 0x24), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    return panel;
}

static lv_obj_t *create_caption(lv_obj_t *parent, const char *text, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, 2, y);
    lv_obj_set_style_text_color(label, lv_color_make(0xD8, 0xD8, 0xD8), 0);
    return label;
}

static lv_obj_t *create_slider(lv_obj_t *parent, int32_t y, int32_t value)
{
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, 2, y);
    lv_obj_set_size(slider, 188, 10);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_make(0x45, 0x45, 0x45), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_make(0xFF, 0x66, 0x00), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    return slider;
}

lv_obj_t * ui_app_settings_create(lv_obj_t * parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);

    g_settings_panel = create_panel(root);

    g_settings_view.brightness_label = create_caption(g_settings_panel, ui_i18n_text(UI_TEXT_SETTINGS_BRIGHTNESS), 0);
    g_settings_view.brightness_slider = create_slider(g_settings_panel, 30, 72);

    g_settings_view.volume_label = create_caption(g_settings_panel, ui_i18n_text(UI_TEXT_SETTINGS_VOLUME), 55);
    g_settings_view.volume_slider = create_slider(g_settings_panel, 84, 56);

    g_settings_view.language_label = create_caption(g_settings_panel, ui_i18n_text(UI_TEXT_SETTINGS_LANGUAGE), 106);
    g_settings_view.language_dropdown = lv_dropdown_create(g_settings_panel);
    lv_obj_set_pos(g_settings_view.language_dropdown, 2, 130);
    lv_obj_set_size(g_settings_view.language_dropdown, 132, 32);
    lv_dropdown_set_options(g_settings_view.language_dropdown, "中文\nEnglish");
    lv_dropdown_set_selected(g_settings_view.language_dropdown, ui_i18n_is_english() ? 1 : 0);
    /* Disable LVGL's built-in dropdown symbol and draw our own ASCII marker.
     * The built-in symbol uses LVGL's private icon glyphs, which are easy to
     * miss in Windows TTF loading and ESP32-S3 reduced CJK fonts. */
    lv_dropdown_set_symbol(g_settings_view.language_dropdown, NULL);
    lv_obj_set_style_radius(g_settings_view.language_dropdown, 8, 0);
    lv_obj_set_style_bg_color(g_settings_view.language_dropdown, lv_color_make(0x36, 0x36, 0x36), 0);
    lv_obj_set_style_text_color(g_settings_view.language_dropdown, lv_color_white(), 0);
    lv_obj_set_style_pad_left(g_settings_view.language_dropdown, 14, 0);
    lv_obj_set_style_pad_right(g_settings_view.language_dropdown, 30, 0);

    g_settings_view.language_symbol_label = lv_label_create(g_settings_panel);
    lv_label_set_text(g_settings_view.language_symbol_label, "v");
    lv_obj_set_style_text_color(g_settings_view.language_symbol_label, lv_color_white(), 0);
    lv_obj_set_pos(g_settings_view.language_symbol_label, 114, 134);
    lv_obj_add_flag(g_settings_view.language_symbol_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_settings_view.language_symbol_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    g_settings_view.server_label = create_caption(g_settings_panel, ui_i18n_text(UI_TEXT_SETTINGS_SERVER), 168);

    g_settings_view.ip_label = lv_label_create(g_settings_panel);
    lv_label_set_text(g_settings_view.ip_label, ui_i18n_text(UI_TEXT_SETTINGS_IP));
    lv_obj_set_pos(g_settings_view.ip_label, 2, 192);
    lv_obj_set_style_text_color(g_settings_view.ip_label, lv_color_make(0xD8, 0xD8, 0xD8), 0);

    g_settings_view.port_label = lv_label_create(g_settings_panel);
    lv_label_set_text(g_settings_view.port_label, ui_i18n_text(UI_TEXT_SETTINGS_PORT));
    lv_obj_set_pos(g_settings_view.port_label, 2, 214);
    lv_obj_set_style_text_color(g_settings_view.port_label, lv_color_make(0xD8, 0xD8, 0xD8), 0);

    ui_event_register_settings(&g_settings_view);
    return root;
}

void ui_app_settings_enter(lv_obj_t * root)
{
    (void)root;

    lv_obj_set_y(g_settings_panel, -260);
    start_y_anim(g_settings_panel, -260, 48, 300, 0, lv_anim_path_ease_out, NULL, NULL);
}

void ui_app_settings_exit(lv_obj_t * root, lv_anim_completed_cb_t done_cb)
{
    app_exit_ctx_t *ctx = lv_malloc(sizeof(app_exit_ctx_t));
    if(ctx == NULL) {
        lv_obj_delete(root);
        if(done_cb != NULL) {
            done_cb(NULL);
        }
        return;
    }
    ctx->done_cb = done_cb;
    ctx->root = root;

    start_y_anim(g_settings_panel,
                 lv_obj_get_y(g_settings_panel),
                 -260,
                 240,
                 0,
                 lv_anim_path_ease_in,
                 app_exit_done_cb,
                 ctx);
}
