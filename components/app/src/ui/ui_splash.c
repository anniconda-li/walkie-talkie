/**
 * @file ui_splash.c
 * @brief 启动画面实现
 *
 * 实现带动画效果的启动画面：
 * 1. 线条动画：水平线条从左向右绘制
 * 2. 面板扩展动画：黑色面板从中间向上下扩展覆盖全屏
 * 动画完成后自动清理所有资源
 */

#include "ui_splash.h"
#include "ui.h"
#include "ui_shell.h"
#include "lvgl.h"

#define MAX_POINTS 30  /* 动画500ms约30帧，够用即可 */

static lv_obj_t *g_line = NULL;           /* 绘制中的线条对象 */
static lv_obj_t *g_black = NULL;          /* 扩展中的黑色面板对象 */
static lv_point_precise_t g_points[MAX_POINTS];
static int g_point_num = 0;

/* 线条绘制回调：x 从 -8 变化到屏幕宽度，逐帧添加线条端点 */
static void anim_draw_cb(void *obj, int32_t x) {
    (void)obj;
    if (g_line == NULL) {
        g_line = lv_line_create(lv_screen_active());  /* 创建线条并添加到当前屏幕 */
        lv_obj_set_style_line_color(g_line, lv_color_hex(0x000000), 0);  /* 设置线条颜色为黑色 */
        lv_obj_set_style_line_width(g_line, 2, 0);  /* 设置线条宽度为2像素 */
    }
    if (g_point_num < MAX_POINTS) {
        g_points[g_point_num].x = (int32_t)x;
        g_points[g_point_num].y = (int32_t)(UI_SCREEN_HEIGHT / 2);
        g_point_num++;
        lv_line_set_points(g_line, (const lv_point_precise_t *)g_points, g_point_num);
    }
}

/* 面板 Y 坐标动画回调：控制面板从中间向上移动 */
static void anim_expand_y_cb(void *obj, int32_t y) {
    lv_obj_set_y(obj, y);
}

/* 面板高度动画回调：控制面板高度从 0 扩展到全屏 */
static void anim_expand_h_cb(void *obj, int32_t h) {
    lv_obj_set_height(obj, h);
}

/* 清理函数：删除所有动画对象，释放内存 */
static void cleanup(void) {
    if (g_line) {
        lv_obj_delete(g_line);
        g_line = NULL;
    }
    if (g_black) {
        lv_obj_delete(g_black);
        g_black = NULL;
    }
}

/* 面板扩展完成回调：清理所有 splash 对象，然后启动主界面 */
static void anim_expand_done_cb(lv_anim_t *a) {
    (void)a;
    cleanup();  /* 删除线条和黑色面板，释放内存 */
    ui_shell_init();  /* 启动主界面壳层，并默认加载 intercom */
}

/* 线条绘制完成回调：创建并启动面板扩展动画 */
static void anim_draw_done_cb(lv_anim_t *a) {
    (void)a;

    int mid_y = UI_SCREEN_HEIGHT / 2;

    /* 删除线条对象 */
    if (g_line) {
        lv_obj_delete(g_line);
        g_line = NULL;
    }

    /* 创建黑色面板，初始位于屏幕中间，高度为0 */
    g_black = lv_obj_create(lv_screen_active());  /* 创建普通对象并添加到当前屏幕 */
    lv_obj_set_pos(g_black, 0, mid_y);  /* 设置对象左对齐，垂直居中 */
    lv_obj_set_size(g_black, UI_SCREEN_WIDTH, 0);  /* 设置宽度为屏幕宽度，高度为0 */
    lv_obj_set_style_bg_color(g_black, lv_color_hex(0x000000), 0);  /* 设置背景色为黑色 */

    /* Y轴动画：面板从中间移动到 y=0（向上扩展） */
    lv_anim_t a_y;
    lv_anim_init(&a_y);  /* 初始化动画结构 */
    lv_anim_set_var(&a_y, g_black);  /* 设置动画作用于 g_black 对象 */
    lv_anim_set_duration(&a_y, 400);  /* 设置动画持续时间400ms */
    lv_anim_set_exec_cb(&a_y, anim_expand_y_cb);  /* 设置位置更新回调 */
    lv_anim_set_values(&a_y, mid_y, 0);  /* 设置 Y 坐标从中间值渐变到0 */
    lv_anim_set_path_cb(&a_y, lv_anim_path_ease_in);  /* 设置缓动曲线为 ease-in */
    lv_anim_start(&a_y);  /* 启动 Y 轴动画 */

    /* 高度动画：面板从高度0扩展到全屏高度（上下同时扩展） */
    lv_anim_t a_h;
    lv_anim_init(&a_h);  /* 初始化动画结构 */
    lv_anim_set_var(&a_h, g_black);  /* 设置动画作用于 g_black 对象 */
    lv_anim_set_duration(&a_h, 400);  /* 设置动画持续时间400ms */
    lv_anim_set_exec_cb(&a_h, anim_expand_h_cb);  /* 设置高度更新回调 */
    lv_anim_set_values(&a_h, 0, UI_SCREEN_HEIGHT);  /* 设置高度从0渐变到屏幕高度 */
    lv_anim_set_path_cb(&a_h, lv_anim_path_ease_in);  /* 设置缓动曲线为 ease-in */
    lv_anim_set_completed_cb(&a_h, anim_expand_done_cb);  /* 设置动画完成回调 */
    lv_anim_start(&a_h);  /* 启动高度动画 */
}

/* 启动画面入口：执行两阶段动画 */
void splash_screen(void) {
    g_point_num = 0;

    /* 创建虚拟对象驱动线条绘制动画（x 从 -8 到屏幕宽度） */
    lv_obj_t *dummy = lv_obj_create(lv_screen_active());  /* 创建普通对象并添加到当前屏幕 */
    lv_obj_set_size(dummy, 1, 1);  /* 设置对象大小为1x1像素 */
    lv_obj_set_pos(dummy, 0, 0);  /* 设置对象位置在左上角 */
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_HIDDEN);  /* 设置隐藏标志，使对象不可见 */

    /* 配置线条绘制动画 */
    lv_anim_t anim;
    lv_anim_init(&anim);  /* 初始化动画结构 */
    lv_anim_set_var(&anim, dummy);  /* 设置动画作用于 dummy 对象 */
    lv_anim_set_values(&anim, -8, UI_SCREEN_WIDTH);  /* 设置动画值范围从-8到屏幕宽度 */
    lv_anim_set_duration(&anim, 500);  /* 设置动画持续时间500ms */
    lv_anim_set_exec_cb(&anim, anim_draw_cb);  /* 设置执行回调，逐帧绘制线条 */
    lv_anim_set_completed_cb(&anim, anim_draw_done_cb);  /* 设置完成回调，启动下一阶段动画 */
    lv_anim_start(&anim);  /* 启动线条绘制动画 */
}
