#include "ui_app_camera.h"
#include "ui.h"
#include "ui_event.h"
#include "ui_i18n.h"

typedef struct {
    lv_anim_completed_cb_t done_cb;
    lv_obj_t * root;
} app_exit_ctx_t;

static ui_camera_view_t g_camera_view;

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

static lv_obj_t *create_button(lv_obj_t *parent,
                               int32_t x,
                               int32_t y,
                               int32_t w,
                               int32_t h,
                               const char *text,
                               lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x36, 0x36, 0x36), 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x20, 0x20, 0x20), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(btn, lv_color_make(0x75, 0x75, 0x75), 0);
    lv_obj_set_style_border_width(btn, 1, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    if(label_out != NULL) {
        *label_out = label;
    }
    return btn;
}

lv_obj_t * ui_app_camera_create(lv_obj_t * parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);

    /*
     * 预览区域不创建任何 LVGL 对象。摄像头画面由 app_camera 直接绘制到
     * LCD 的固定区域，底部按钮仍由 LVGL 管理。这样可以排除预览区对象被
     * LVGL flush 覆盖导致的花屏。
     */
    g_camera_view.capture_button = create_button(root, 8, 278, 68, 34,
                                                 ui_i18n_text(UI_TEXT_CAMERA_CAPTURE),
                                                 &g_camera_view.capture_label);
    g_camera_view.upload_button = create_button(root, 86, 278, 68, 34,
                                                ui_i18n_text(UI_TEXT_CAMERA_UPLOAD),
                                                &g_camera_view.upload_label);
    g_camera_view.retake_button = create_button(root, 164, 278, 68, 34,
                                                ui_i18n_text(UI_TEXT_CAMERA_RETAKE),
                                                &g_camera_view.retake_label);
    g_camera_view.frozen = false;

    ui_event_register_camera(&g_camera_view);
    return root;
}

void ui_app_camera_enter(lv_obj_t * root)
{
    (void)root;

    ui_event_notify_camera_entered();

    lv_obj_set_y(g_camera_view.capture_button, UI_SCREEN_HEIGHT + 10);
    lv_obj_set_y(g_camera_view.upload_button, UI_SCREEN_HEIGHT + 10);
    lv_obj_set_y(g_camera_view.retake_button, UI_SCREEN_HEIGHT + 10);

    start_y_anim(g_camera_view.capture_button, UI_SCREEN_HEIGHT + 10, 278, 240, 40, lv_anim_path_ease_out, NULL, NULL);
    start_y_anim(g_camera_view.upload_button, UI_SCREEN_HEIGHT + 10, 278, 240, 65, lv_anim_path_ease_out, NULL, NULL);
    start_y_anim(g_camera_view.retake_button, UI_SCREEN_HEIGHT + 10, 278, 240, 90, lv_anim_path_ease_out, NULL, NULL);
}

void ui_app_camera_exit(lv_obj_t * root, lv_anim_completed_cb_t done_cb)
{
    ui_event_notify_camera_exited();

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

    start_y_anim(g_camera_view.capture_button,
                 lv_obj_get_y(g_camera_view.capture_button),
                 UI_SCREEN_HEIGHT + 10,
                 210,
                 30,
                 lv_anim_path_ease_in,
                 NULL,
                 NULL);
    start_y_anim(g_camera_view.upload_button,
                 lv_obj_get_y(g_camera_view.upload_button),
                 UI_SCREEN_HEIGHT + 10,
                 210,
                 50,
                 lv_anim_path_ease_in,
                 NULL,
                 NULL);
    start_y_anim(g_camera_view.retake_button,
                 lv_obj_get_y(g_camera_view.retake_button),
                 UI_SCREEN_HEIGHT + 10,
                 210,
                 70,
                 lv_anim_path_ease_in,
                 app_exit_done_cb,
                 ctx);
}
