/**
 * @file ui_app_intercom.h
 * @brief intercom 应用占位界面接口
 */

#ifndef UI_APP_INTERCOM_H
#define UI_APP_INTERCOM_H

#include "lvgl.h"

/**
 * @brief 创建 intercom 应用的根对象
 *
 * @param parent 应用内容挂载的父对象
 * @return lv_obj_t* 创建好的应用根对象
 */
lv_obj_t * ui_app_intercom_create(lv_obj_t * parent);

/**
 * @brief 播放 intercom 应用的入场动画
 *
 * @param root intercom 应用根对象
 */
void ui_app_intercom_enter(lv_obj_t * root);

/**
 * @brief 播放 intercom 应用的退场动画
 *
 * 动画结束后会自动删除 root，然后再调用 done_cb。
 *
 * @param root intercom 应用根对象
 * @param done_cb 退场完成后的回调，可为 NULL
 */
void ui_app_intercom_exit(lv_obj_t * root, lv_anim_completed_cb_t done_cb);

#endif /* UI_APP_INTERCOM_H */
