/**
 * @file weather_clock.h
 * @brief 待机天气时钟（文字版）接口
 *
 * 背景：设备屏幕由原厂 launcher/wiki.so 驱动，assistant 只能通过
 *       sair_subtitle_push() 字幕通道（0x233+事件5）往屏幕推「文字」。
 *       本模块在待机态周期推送一行「年月日 周X HH:MM | 城市 天气 温度°C」，
 *       数据来自 Open-Meteo（免费、无 Key、HTTPS）。
 *
 * 设计原则：
 *  - 网络抓取在独立 pthread 内进行（http_get 最长阻塞 20s，不占主循环）；
 *  - 抓取线程只写共享缓冲、不碰字幕通道；屏幕推送统一由主循环 weather_clock_tick()
 *    返回待推文本，由 main.c 调用 sair_subtitle_push() 完成（避免跨线程消息竞态）。
 */

#ifndef WEATHER_CLOCK_H
#define WEATHER_CLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

/** 启动：读配置并视需开启抓取线程（幂等）。须在 applib 初始化后调用。 */
void weather_clock_start(void);

/** 停止抓取线程（线程在 1s 内退出；热更新 execvp 无需调用）。 */
void weather_clock_stop(void);

/** 重载配置（WEATHER_* 键）。启动时与运行时配置变更时调用。 */
void weather_clock_reload_config(void);

/** 待机生命周期挂钩：进入待机（重置去重，下一次 tick 立即推送一次）。 */
void weather_clock_on_standby_enter(void);

/** 待机生命周期挂钩：退出待机。 */
void weather_clock_on_standby_exit(void);

/** 是否启用天气时钟（main.c 用其决定是否跳过 STANDBY_TEXT 静态文字）。 */
int weather_clock_is_enabled(void);

/**
 * 主循环周期调用（挂在 standby_tick 之后）。
 * @return 待推送的字幕行（模块静态缓冲，下次调用前有效）；
 *         无需推送（未启用/非待机/内容未变化）返回 NULL。
 */
const char *weather_clock_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_CLOCK_H */
