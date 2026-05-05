#include "ui_app_ai.h"
#include "ui.h"
#include "ui_event.h"
#include "ui_i18n.h"

typedef struct {
    lv_anim_completed_cb_t done_cb;
    lv_obj_t * root;
} app_exit_ctx_t;

static ui_ai_view_t g_ai_view;
static lv_obj_t *g_answer_panel;

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

lv_obj_t * ui_app_ai_create(lv_obj_t * parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);

    g_answer_panel = lv_obj_create(root);
    lv_obj_remove_flag(g_answer_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_answer_panel, 18, 56);
    lv_obj_set_size(g_answer_panel, 204, 170);
    lv_obj_set_style_radius(g_answer_panel, 16, 0);
    lv_obj_set_style_bg_color(g_answer_panel, lv_color_make(0x22, 0x22, 0x22), 0);
    lv_obj_set_style_bg_opa(g_answer_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_answer_panel, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_set_style_border_width(g_answer_panel, 1, 0);
    lv_obj_set_style_pad_all(g_answer_panel, 12, 0);

    g_ai_view.answer_label = lv_label_create(g_answer_panel);
    lv_label_set_text(g_ai_view.answer_label, ui_i18n_text(UI_TEXT_AI_IDLE));
    lv_label_set_long_mode(g_ai_view.answer_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_ai_view.answer_label, 180);
    lv_obj_set_style_text_color(g_ai_view.answer_label, lv_color_make(0xEA, 0xEA, 0xEA), 0);
    lv_obj_set_pos(g_ai_view.answer_label, 0, 0);

    for(int32_t i = 0; i < 4; i++) {
        g_ai_view.voice_bars[i] = lv_obj_create(root);
        lv_obj_remove_flag(g_ai_view.voice_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(g_ai_view.voice_bars[i], 7, 18 + (i % 2) * 12);
        lv_obj_set_pos(g_ai_view.voice_bars[i], 88 + i * 16, 238 - (i % 2) * 6);
        lv_obj_set_style_radius(g_ai_view.voice_bars[i], 3, 0);
        lv_obj_set_style_bg_color(g_ai_view.voice_bars[i], lv_color_make(0xFF, 0x66, 0x00), 0);
        lv_obj_set_style_border_width(g_ai_view.voice_bars[i], 0, 0);
        lv_obj_add_flag(g_ai_view.voice_bars[i], LV_OBJ_FLAG_HIDDEN);
    }

    g_ai_view.ask_button = lv_button_create(root);
    lv_obj_set_pos(g_ai_view.ask_button, 60, 262);
    lv_obj_set_size(g_ai_view.ask_button, 120, 42);
    lv_obj_set_style_radius(g_ai_view.ask_button, 21, 0);
    lv_obj_set_style_bg_color(g_ai_view.ask_button, lv_color_make(0x36, 0x36, 0x36), 0);
    lv_obj_set_style_border_color(g_ai_view.ask_button, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_obj_set_style_border_width(g_ai_view.ask_button, 1, 0);

    g_ai_view.ask_label = lv_label_create(g_ai_view.ask_button);
    lv_label_set_text(g_ai_view.ask_label, ui_i18n_text(UI_TEXT_AI_ASK));
    lv_obj_set_style_text_color(g_ai_view.ask_label, lv_color_white(), 0);
    lv_obj_center(g_ai_view.ask_label);

    g_ai_view.speaking = false;
    ui_event_register_ai(&g_ai_view);
    return root;
}

void ui_app_ai_enter(lv_obj_t * root)
{
    (void)root;

    lv_obj_set_y(g_answer_panel, -180);
    lv_obj_set_y(g_ai_view.ask_button, UI_SCREEN_HEIGHT + 12);
    for(int32_t i = 0; i < 4; i++) {
        lv_obj_set_y(g_ai_view.voice_bars[i], UI_SCREEN_HEIGHT + 12);
    }

    start_y_anim(g_answer_panel, -180, 56, 270, 0, lv_anim_path_ease_out, NULL, NULL);
    for(int32_t i = 0; i < 4; i++) {
        int32_t target_y = 238 - (i % 2) * 6;
        start_y_anim(g_ai_view.voice_bars[i],
                     UI_SCREEN_HEIGHT + 12,
                     target_y,
                     240,
                     (uint32_t)(40 + i * 18),
                     lv_anim_path_ease_out,
                     NULL,
                     NULL);
    }
    start_y_anim(g_ai_view.ask_button, UI_SCREEN_HEIGHT + 12, 262, 250, 75, lv_anim_path_ease_out, NULL, NULL);
}

void ui_app_ai_exit(lv_obj_t * root, lv_anim_completed_cb_t done_cb)
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

    start_y_anim(g_answer_panel, lv_obj_get_y(g_answer_panel), -180, 220, 0, lv_anim_path_ease_in, NULL, NULL);
    for(int32_t i = 0; i < 4; i++) {
        start_y_anim(g_ai_view.voice_bars[i],
                     lv_obj_get_y(g_ai_view.voice_bars[i]),
                     UI_SCREEN_HEIGHT + 12,
                     210,
                     (uint32_t)(25 + i * 15),
                     lv_anim_path_ease_in,
                     NULL,
                     NULL);
    }
    start_y_anim(g_ai_view.ask_button,
                 lv_obj_get_y(g_ai_view.ask_button),
                 UI_SCREEN_HEIGHT + 12,
                 220,
                 70,
                 lv_anim_path_ease_in,
                 app_exit_done_cb,
                 ctx);
}
