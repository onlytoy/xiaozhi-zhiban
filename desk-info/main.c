/**
 * @file main.c
 * @brief desk-info — 横屏信息台 V1（独立项目，直接绘制 /dev/fb0）
 *
 * 目标设备：智伴 GS705B（ACTIONS OWL / Linux 3.10 / uClibc）
 * 屏：帧缓冲 /dev/fb0 = 480×1600 虚拟（2 帧 480×800 上下叠），RGB565；
 *     物理横屏 800×480（竖帧旋转 90° 驱动）。
 *
 * V1 只做：渐变背景 + 顶部红色方向标记 + 居中大时钟(HH:MM) + 日期，
 * 用于验证「直接画帧缓冲」通路 + 确认旋转方向。天气/信息栏在 V2 加。
 *
 * 编译：静态链接(避免设备 hard-float uClibc ABI 不匹配)，
 *      arm-buildroot-linux-uclibcgnueabi-gcc -static -Os。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

typedef unsigned short u16;

static u16 *fb = NULL;
static int FB_W, FB_H, FB_VH;   /* 帧缓冲：宽 / 单帧高 / 虚拟高 */
static int SCR_W, SCR_H;        /* 物理横屏：宽 / 高 */

/* ---- RGB565 ---- */
static inline u16 rgb565(int r, int g, int b)
{
    return (u16)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | ((b & 0xf8) >> 3));
}

/* ---- 屏坐标(横屏) -> 帧缓冲，90° 旋转 ----
 * 横屏(sx:0..SCR_W-1, sy:0..SCR_H-1) 映射到竖帧。
 * 若方向反了(左右/上下颠倒)，改 fy 的两种写法之一即可。 */
static inline void px(int sx, int sy, u16 c)
{
    if (sx < 0 || sx >= SCR_W || sy < 0 || sy >= SCR_H) return;
    int fx = sy;
    int fy = FB_H - 1 - sx;          /* 旋转方向 A；反了改 fy = sx */
    long off = (long)fy * FB_W + fx;
    fb[off] = c;
    fb[off + (long)FB_W * FB_H] = c; /* 双缓冲：两帧都写 */
}

/* ---- 5x7 点阵字体（bit4=最左列）---- */
static const unsigned char FONT[12][7] = {
    /* 0 */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* 1 */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* 2 */ {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    /* 3 */ {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    /* 4 */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    /* 5 */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* 6 */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    /* 7 */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* 8 */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* 9 */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* : */ {0x00,0x06,0x06,0x00,0x06,0x06,0x00},
    /* - */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
};

static void draw_char(char ch, int sx, int sy, int scale, u16 color)
{
    int idx;
    if (ch >= '0' && ch <= '9')      idx = ch - '0';
    else if (ch == ':')              idx = 10;
    else if (ch == '-')              idx = 11;
    else                             idx = 11; /* 未知字符画横杠 */
    for (int r = 0; r < 7; r++) {
        for (int c = 0; c < 5; c++) {
            if (!(FONT[idx][r] & (0x10 >> c))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    px(sx + c * scale + dx, sy + r * scale + dy, color);
        }
    }
}

/* 返回字符串绘制后的像素宽度 */
static int draw_text(const char *s, int sx, int sy, int scale, u16 color)
{
    int x = sx;
    for (; *s; s++) {
        if (*s == ' ') { x += 3 * scale; continue; }
        draw_char(*s, x, sy, scale, color);
        x += 6 * scale; /* 5 列 + 1 列间距 */
    }
    return x - sx;
}

int main(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open /dev/fb0"); return 1; }

    struct fb_var_screeninfo vi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0) { perror("ioctl"); return 1; }
    FB_W  = vi.xres;
    FB_H  = vi.yres;
    FB_VH = vi.yres_virtual;
    SCR_W = FB_H;   /* 竖帧高 -> 横屏宽 */
    SCR_H = FB_W;   /* 竖帧宽 -> 横屏高 */

    long bpp = vi.bits_per_pixel / 8;
    long fbsize = (long)FB_W * FB_VH * bpp;
    fb = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }

    for (;;) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);

        char clock[16], date[40];
        snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
        snprintf(date, sizeof(date), "%d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

        /* 背景渐变 */
        for (int sy = 0; sy < SCR_H; sy++) {
            u16 c = rgb565(14, 18 + sy / 14, 34 + sy / 7);
            for (int sx = 0; sx < SCR_W; sx++)
                px(sx, sy, c);
        }

        /* 方向标记：顶部一条红边（用于确认旋转方向） */
        for (int sx = 0; sx < SCR_W; sx++) px(sx, 0, rgb565(255, 0, 0));

        /* 居中大时钟 */
        int scale = 22;                       /* 字符 110x154 */
        int cw = draw_text(clock, 0, 0, scale, rgb565(0,0,0)); /* 先测宽 */
        int sx0 = (SCR_W - cw) / 2;
        int sy0 = (SCR_H - 7 * scale) / 2 - 20;
        draw_text(clock, sx0, sy0, scale, rgb565(255, 255, 255));

        /* 日期（时钟下方，小一号） */
        int dscale = 5;
        int dw = draw_text(date, 0, 0, dscale, rgb565(0,0,0));
        draw_text(date, (SCR_W - dw) / 2, sy0 + 7 * scale + 20, dscale, rgb565(200, 210, 255));

        sleep(1);
    }
    return 0;
}
