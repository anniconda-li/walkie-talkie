#ifndef UI_H
#define UI_H


#define UI_SCREEN_WIDTH  240
#define UI_SCREEN_HEIGHT 320

/**
 * @brief 初始化整个 UI 系统
 *
 * 当前流程会先播放启动动画，启动动画结束后再进入主界面壳层。
 */
void ui_init(void);

#endif /* UI_H */
