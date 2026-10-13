/**
 * @file weather_clock.c
 * @brief 待机天气时钟（文字版）实现，见 weather_clock.h
 *
 * 数据源 Open-Meteo，请求仅取当前温度与天气码（标准版「左日期时间｜右城市天气温度」）：
 *   https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..&current=temperature_2m,weather_code
 * 如需「今日高低温/明日预报」（详情版），可追加 daily 参数并解析，见接口注释。
 */

#include "weather_clock.h"
#include "http_client.h"
#include "plog.h"
#include "applib_api.h"            /* get_config */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>

/* 温度后缀：°C（U+00B0 + 'C'）。若真机字体缺 ° 字形，改为 "度" 即可。 */
#define WC_TEMP_SUFFIX "°C"

#define WC_LINE_MAX    160        /* 完整字幕行缓冲 */
#define WC_WX_MAX      64         /* 天气+温度片段缓冲 */

static const char *WC_WEEK_CN[7] = { "日", "一", "二", "三", "四", "五", "六" };

typedef struct {
    /* 配置（主线程写；抓取线程只读 lat/lon/update_min，异步可见，
     * 最坏情况旧值多生效一个抓取周期，可接受） */
    volatile int running;         /* 抓取线程运行标志 */
    int enabled;                  /* WEATHER_CLOCK_ENABLE */
    int update_min;               /* 抓取周期（分钟），下限 5 */
    int tz_hours;                 /* 屏幕时钟时区偏移（默认 +8） */
    char city[32];
    char lat[16];
    char lon[16];

    /* 天气数据（抓取线程写，tick 读，mutex 保护） */
    pthread_mutex_t lock;
    int weather_valid;            /* 1=已取到天气 */
    char weather_txt[WC_WX_MAX];  /* "晴 24°C" 片段 */
    time_t weather_fetched_at;

    pthread_t thread;

    /* 待机态 + 去重（主循环 tick 使用） */
    int standby_active;
    time_t last_tick;
    char last_line[WC_LINE_MAX];  /* 上次推送的字幕行 */
} wc_ctx_t;

static wc_ctx_t g_wc;

/* ------------------------------------------------------------------ */
/* 配置读写（真库 get_config 只支持字符串类型，字符串读整型走 atoi）       */
/* ------------------------------------------------------------------ */
static int wc_cfg_get_int(const char *key, int def)
{
    char b[32];
    if (get_config(key, b, sizeof(b)) > 0 && b[0])
        return atoi(b);
    return def;
}

static void wc_cfg_get_str(const char *key, const char *def, char *out, size_t sz)
{
    if (get_config(key, out, (int)sz) <= 0 || !out[0])
        snprintf(out, sz, "%s", def);
}

/* ------------------------------------------------------------------ */
/* WMO 天气码 → 中文                                                    */
/* ------------------------------------------------------------------ */
static const char *wc_code_to_cn(int code)
{
    switch (code)
    {
    case 0:  return "晴";
    case 1:  return "大部晴";
    case 2:  return "多云";
    case 3:  return "阴";
    case 45:
    case 48: return "雾";
    case 51:
    case 53:
    case 55: return "毛毛雨";
    case 56:
    case 57: return "冻雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66:
    case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "雪粒";
    case 80:
    case 81:
    case 82: return "阵雨";
    case 85:
    case 86: return "阵雪";
    case 95: return "雷阵雨";
    case 96:
    case 99: return "雷雨冰雹";
    default: return "未知";
    }
}

/* ------------------------------------------------------------------ */
/* 天气抓取                                                            */
/* ------------------------------------------------------------------ */
/* 从 Open-Meteo 响应里提取 current 温度与天气码。
 * 纯 strstr 解析(与 api_server.c 同款), 不依赖 cJSON —— 见 README《关键约束》:
 * uClibc 的 malloc 锁非递归, 子线程里 cJSON 解析/打印会踩锁。 */
static int wc_parse_current(const char *body, int *temp_out, int *code_out)
{
    if (!body || !temp_out || !code_out)
        return -1;

    /* 定位 "current":{ 对象(避免误命中前面的 "current_units":{) */
    const char *cur = strstr(body, "\"current\":{");
    if (!cur)
        return -1;

    const char *tp = strstr(cur, "\"temperature_2m\":");
    if (!tp)
        return -1;
    tp += strlen("\"temperature_2m\":");
    double tv = strtod(tp, NULL);
    *temp_out = (int)(tv >= 0 ? tv + 0.5 : tv - 0.5);   /* 四舍五入取整 */

    const char *wp = strstr(cur, "\"weather_code\":");
    if (!wp)
        return -1;
    wp += strlen("\"weather_code\":");
    *code_out = atoi(wp);

    return 0;
}

static int wc_fetch_once(void)
{
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%s&longitude=%s&current=temperature_2m,weather_code",
             g_wc.lat, g_wc.lon);

    http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    if (http_get(url, NULL, &resp) != 0 || resp.status_code != 200 || !resp.body)
    {
        PLOG_W("WC", "天气请求失败 (status=%d)", resp.status_code);
        http_response_free(&resp);
        return 0;
    }

    int temp = 0, code = 0;
    int ok = 0;
    if (wc_parse_current(resp.body, &temp, &code) == 0)
    {
        const char *cn = wc_code_to_cn(code);
        char txt[WC_WX_MAX];
        snprintf(txt, sizeof(txt), "%s %d%s", cn, temp, WC_TEMP_SUFFIX);

        pthread_mutex_lock(&g_wc.lock);
        strncpy(g_wc.weather_txt, txt, sizeof(g_wc.weather_txt) - 1);
        g_wc.weather_txt[sizeof(g_wc.weather_txt) - 1] = '\0';
        g_wc.weather_valid = 1;
        g_wc.weather_fetched_at = time(NULL);
        pthread_mutex_unlock(&g_wc.lock);

        PLOG_I("WC", "天气已更新: %s", txt);
        ok = 1;
    }
    else
    {
        PLOG_W("WC", "天气解析失败, body=%.380s", resp.body);
    }

    http_response_free(&resp);
    return ok;
}

static void *wc_thread_func(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "weather", 0, 0, 0);

    while (g_wc.running)
    {
        int ok = wc_fetch_once();
        /* 成功按周期睡；失败 60s 快速重试（启动时网络未就绪可尽快补上） */
        int total = ok ? (g_wc.update_min * 60) : 60;
        for (int i = 0; i < total && g_wc.running; i++)
            sleep(1);
    }
    return NULL;
}

static void wc_thread_start(void)
{
    if (g_wc.running)
        return;
    g_wc.running = 1;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&g_wc.thread, &attr, wc_thread_func, NULL) != 0)
    {
        PLOG_E("WC", "抓取线程创建失败, 功能停用");
        g_wc.running = 0;
    }
    pthread_attr_destroy(&attr);
}

/* ------------------------------------------------------------------ */
/* 字幕行组装                                                           */
/* ------------------------------------------------------------------ */
static void wc_build_line(char *out, size_t out_sz)
{
    /* 屏幕时钟：显式按 tz_hours 偏移后用 gmtime（不依赖系统时区配置） */
    time_t now = time(NULL) + (time_t)g_wc.tz_hours * 3600;
    struct tm tm;
    gmtime_r(&now, &tm);

    char datebuf[40];
    snprintf(datebuf, sizeof(datebuf), "%d年%d月%d日 周%s",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             WC_WEEK_CN[tm.tm_wday]);
    char timebuf[8];
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d", tm.tm_hour, tm.tm_min);

    pthread_mutex_lock(&g_wc.lock);
    char wxb[WC_WX_MAX];
    if (g_wc.weather_valid)
        snprintf(wxb, sizeof(wxb), "%s", g_wc.weather_txt);
    else
        snprintf(wxb, sizeof(wxb), "天气获取中");
    pthread_mutex_unlock(&g_wc.lock);

    snprintf(out, out_sz, "%s %s | %s %s", datebuf, timebuf, g_wc.city, wxb);
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                             */
/* ------------------------------------------------------------------ */
void weather_clock_start(void)
{
    weather_clock_reload_config();
}

void weather_clock_stop(void)
{
    g_wc.running = 0;
}

void weather_clock_reload_config(void)
{
    g_wc.enabled = wc_cfg_get_int("WEATHER_CLOCK_ENABLE", 0) ? 1 : 0;
    wc_cfg_get_str("WEATHER_CITY", "深圳", g_wc.city, sizeof(g_wc.city));
    wc_cfg_get_str("WEATHER_LAT", "22.5431", g_wc.lat, sizeof(g_wc.lat));
    wc_cfg_get_str("WEATHER_LON", "114.0579", g_wc.lon, sizeof(g_wc.lon));
    g_wc.update_min = wc_cfg_get_int("WEATHER_UPDATE_MIN", 30);
    if (g_wc.update_min < 5)
        g_wc.update_min = 5;
    if (g_wc.update_min > 1440)
        g_wc.update_min = 1440;
    g_wc.tz_hours = wc_cfg_get_int("WEATHER_TZ_HOURS", 8);
    if (g_wc.tz_hours < -12)
        g_wc.tz_hours = -12;
    if (g_wc.tz_hours > 14)
        g_wc.tz_hours = 14;

    PLOG_I("WC", "配置: enable=%d city=%s lat=%s lon=%s update=%dm tz=%d",
           g_wc.enabled, g_wc.city, g_wc.lat, g_wc.lon,
           g_wc.update_min, g_wc.tz_hours);

    if (g_wc.enabled && !g_wc.running)
        wc_thread_start();
}

void weather_clock_on_standby_enter(void)
{
    g_wc.standby_active = 1;
    /* 重置去重，下一次 tick 立即组装并推送一条（即使内容与上次相同） */
    g_wc.last_tick = 0;
    g_wc.last_line[0] = '\0';
    PLOG_I("WC", "进入待机, 天气时钟接管字幕通道");
}

void weather_clock_on_standby_exit(void)
{
    g_wc.standby_active = 0;
    g_wc.last_tick = 0;
}

int weather_clock_is_enabled(void)
{
    return g_wc.enabled ? 1 : 0;
}

const char *weather_clock_tick(void)
{
    if (!g_wc.enabled || !g_wc.standby_active)
        return NULL;

    /* 节流：同一秒内重复调用不重复组装/比较 */
    time_t now = time(NULL);
    if (now == g_wc.last_tick)
        return NULL;
    g_wc.last_tick = now;

    char line[WC_LINE_MAX];
    wc_build_line(line, sizeof(line));
    if (strcmp(line, g_wc.last_line) == 0)
        return NULL;              /* 内容未变（分钟未变且天气未更新） */

    strncpy(g_wc.last_line, line, sizeof(g_wc.last_line) - 1);
    g_wc.last_line[sizeof(g_wc.last_line) - 1] = '\0';
    return g_wc.last_line;
}
