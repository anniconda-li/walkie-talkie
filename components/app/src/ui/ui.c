#include "ui.h"
#include "ui_font.h"
#include "ui_splash.h"
#include "lvgl.h"

void ui_init(void)
{
    lv_obj_t *screen = lv_screen_active();

    ui_font_init();
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_font(screen, ui_font_normal(), 0);

    /* UI 入口暂时保持简单：
     * 1. 先播放开机动画
     * 2. 开机动画结束后，再由 splash 模块切到主界面壳层
     * 这样可以让启动流程与主界面逻辑解耦，后面扩展也更方便。 */
    splash_screen();
}
