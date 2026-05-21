#include "ui_app_camera.h"
#include "ui.h"
#include "ui_assets.h"
#include "ui_event.h"

static ui_camera_view_t g_camera_view;

#define CAMERA_BTN_LEFT_X   8
#define CAMERA_BTN_CENTER_X 86
#define CAMERA_BTN_RIGHT_X  164
#define CAMERA_BTN_Y        278
#define CAMERA_BTN_W        68
#define CAMERA_BTN_H        34

static lv_obj_t *create_button(lv_obj_t *parent,
                               int32_t x,
                               int32_t y,
                               int32_t w,
                               int32_t h,
                               const lv_image_dsc_t *src,
                               lv_obj_t **icon_out)
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

    lv_obj_t *icon = lv_image_create(btn);
    lv_image_set_src(icon, src);
    lv_image_set_scale(icon, 160);
    lv_obj_center(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    if(icon_out != NULL) {
        *icon_out = icon;
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
    g_camera_view.upload_button = create_button(root, CAMERA_BTN_LEFT_X, CAMERA_BTN_Y, CAMERA_BTN_W, CAMERA_BTN_H,
                                                &icon_camera_upload,
                                                &g_camera_view.upload_icon);
    g_camera_view.capture_button = create_button(root, CAMERA_BTN_CENTER_X, CAMERA_BTN_Y, CAMERA_BTN_W, CAMERA_BTN_H,
                                                 &icon_camera_capture,
                                                 &g_camera_view.capture_icon);
    g_camera_view.retake_button = create_button(root, CAMERA_BTN_RIGHT_X, CAMERA_BTN_Y, CAMERA_BTN_W, CAMERA_BTN_H,
                                                &icon_camera_back,
                                                &g_camera_view.retake_icon);
    g_camera_view.frozen = false;

    ui_event_register_camera(&g_camera_view);
    return root;
}

void ui_app_camera_enter(lv_obj_t * root)
{
    (void)root;

    ui_event_notify_camera_entered();

    lv_obj_set_y(g_camera_view.capture_button, CAMERA_BTN_Y);
    lv_obj_set_y(g_camera_view.upload_button, CAMERA_BTN_Y);
    lv_obj_set_y(g_camera_view.retake_button, CAMERA_BTN_Y);
}

void ui_app_camera_exit(lv_obj_t * root, lv_anim_completed_cb_t done_cb)
{
    ui_event_notify_camera_exited();

    lv_obj_delete(root);
    if(done_cb != NULL) {
        done_cb(NULL);
    }
}
