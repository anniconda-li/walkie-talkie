#include "ui_app_intercom.h"
#include "ui.h"
#include "ui_event.h"
#include "ui_font.h"
#include "ui_i18n.h"
#include "ui_theme.h"

typedef struct {
    lv_anim_completed_cb_t done_cb;
    lv_obj_t * root;
} app_exit_ctx_t;

static ui_intercom_view_t g_intercom_view;
static lv_obj_t *g_display_panel;
static lv_obj_t *g_control_panel;

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

static lv_obj_t *create_panel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_style_bg_color(obj, lv_color_make(0x24, 0x24, 0x24), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, UI_COLOR_INTERCOM, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *create_button(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, const char *text)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x3A, 0x3A, 0x3A), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_set_style_border_width(btn, 1, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return btn;
}

lv_obj_t * ui_app_intercom_create(lv_obj_t * parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);

    g_display_panel = create_panel(root, 24, 66, 192, 124);

    g_intercom_view.channel_label = lv_label_create(g_display_panel);
    g_intercom_view.channel = 1;
    lv_obj_set_style_text_color(g_intercom_view.channel_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_intercom_view.channel_label, ui_font_normal(), 0);
    lv_obj_align(g_intercom_view.channel_label, LV_ALIGN_CENTER, 0, -14);

    g_intercom_view.channel_hint_label = lv_label_create(g_display_panel);
    lv_label_set_text(g_intercom_view.channel_hint_label, ui_i18n_text(UI_TEXT_INTERCOM_CHANNEL));
    lv_obj_set_style_text_color(g_intercom_view.channel_hint_label, lv_color_make(0xA8, 0xA8, 0xA8), 0);
    lv_obj_align(g_intercom_view.channel_hint_label, LV_ALIGN_CENTER, 0, 28);

    for(int32_t i = 0; i < 3; i++) {
        g_intercom_view.broadcast_rings[i] = lv_obj_create(g_display_panel);
        lv_obj_remove_flag(g_intercom_view.broadcast_rings[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(g_intercom_view.broadcast_rings[i], 58 + i * 24, 58 + i * 24);
        lv_obj_set_style_radius(g_intercom_view.broadcast_rings[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(g_intercom_view.broadcast_rings[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(g_intercom_view.broadcast_rings[i], UI_COLOR_INTERCOM, 0);
        lv_obj_set_style_border_width(g_intercom_view.broadcast_rings[i], 1, 0);
        lv_obj_set_style_opa(g_intercom_view.broadcast_rings[i], (lv_opa_t)(100 - i * 24), 0);
        lv_obj_center(g_intercom_view.broadcast_rings[i]);
        lv_obj_add_flag(g_intercom_view.broadcast_rings[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(g_intercom_view.channel_label);
    lv_obj_move_foreground(g_intercom_view.channel_hint_label);

    g_control_panel = create_panel(root, 16, 220, 208, 70);
    lv_obj_set_style_radius(g_control_panel, 20, 0);
    g_intercom_view.channel_dec_button = create_button(g_control_panel, 10, 14, 42, 42, "-");
    g_intercom_view.ptt_button = create_button(g_control_panel, 66, 8, 76, 54, ui_i18n_text(UI_TEXT_INTERCOM_PTT));
    g_intercom_view.channel_inc_button = create_button(g_control_panel, 156, 14, 42, 42, "+");

    ui_event_register_intercom(&g_intercom_view);
    return root;
}

void ui_app_intercom_enter(lv_obj_t * root)
{
    (void)root;

    lv_obj_set_y(g_display_panel, -132);
    lv_obj_set_y(g_control_panel, UI_SCREEN_HEIGHT + 18);
    start_y_anim(g_display_panel, -132, 66, 260, 0, lv_anim_path_ease_out, NULL, NULL);
    start_y_anim(g_control_panel, UI_SCREEN_HEIGHT + 18, 220, 260, 45, lv_anim_path_ease_out, NULL, NULL);
}

void ui_app_intercom_exit(lv_obj_t * root, lv_anim_completed_cb_t done_cb)
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

    start_y_anim(g_display_panel, lv_obj_get_y(g_display_panel), -132, 220, 0, lv_anim_path_ease_in, NULL, NULL);
    start_y_anim(g_control_panel,
                 lv_obj_get_y(g_control_panel),
                 UI_SCREEN_HEIGHT + 18,
                 220,
                 35,
                 lv_anim_path_ease_in,
                 app_exit_done_cb,
                 ctx);
}
