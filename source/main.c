/*
 * DEEP HARNESS SWITCH — Phase 1: IRS benchmark / probe.
 *
 * 目的:在真机上测量右 Joy-Con 红外摄像头(IRS)的实际采集时序,验证
 * libnx irs.h 的 ImageTransferProcessor 各格式,而不是先做扫描器。
 *
 * 功能:
 *   - 初始化 IRS,取右 Joy-Con 红外摄像头句柄
 *   - 以当前 IrsImageTransferProcessorFormat 运行 ImageTransferProcessor
 *   - 显示 8-bit 灰度原始图
 *   - 测量:首帧延迟、采样间隔(avg/median/p95/max)、采集错误数
 *   - X 循环切换格式(320x240 → 20x15)
 *   - A 切换 IR LED(light_target: 0=全亮 / 3=关闭)
 *   - Y 把当前格式的基准结果写入 SD 卡
 *   - B 重置当前格式的计时
 *
 * 仅基于 libnx irs.h 与 switchbrew/switch-examples 的 API 用法实现,
 * 未复制任何 All Rights Reserved 的第三方源码。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>
#include <switch/arm/counter.h>

#include <SDL.h>
#include <SDL_ttf.h>

#define WIN_W 1280
#define WIN_H 720

/* 图像缓冲:最大格式 320x240 = 76800 字节 (switchbrew 示例的 0x12c00) */
#define IR_BUF_SIZE 0x12c00
#define IR_WORK_SIZE 0x100000
#define MAX_INTERVALS 512

/* 格式表:顺序即循环顺序 */
typedef struct {
    IrsImageTransferProcessorFormat fmt;
    const char *name;
    int w, h;
} ir_format_t;

static const ir_format_t g_formats[] = {
    { IrsImageTransferProcessorFormat_320x240, "320x240", 320, 240 },
    { IrsImageTransferProcessorFormat_160x120, "160x120", 160, 120 },
    { IrsImageTransferProcessorFormat_80x60,   "80x60",   80,  60 },
    { IrsImageTransferProcessorFormat_40x30,   "40x30",   40,  30 },
    { IrsImageTransferProcessorFormat_20x15,   "20x15",   20,  15 },
};
#define N_FORMATS ((int)(sizeof(g_formats) / sizeof(g_formats[0])))

/* ---- IRS 状态 ---- */
static IrsIrCameraHandle g_ir;
static int g_ir_ready = 0;          /* 句柄已取得 */
static char g_ir_err[256] = "";     /* 初始化错误信息 */
static u8 *g_ir_buffer = NULL;
static int g_fmt_idx = 0;
static u32 g_light_target = 0;      /* 0=全亮, 3=关闭 */
static u64 g_sampling = 0;          /* 上次采样号 */
static int g_have_frame = 0;

/* ---- 计时 ---- */
static u64 g_proc_start_ns = 0;     /* 本次 processor 启动时间 */
static u64 g_first_frame_ns = 0;    /* 首帧延迟 */
static int g_got_first = 0;
static u64 g_last_sample_ns = 0;    /* 上一帧时间 */
static u64 g_interval_sum_us = 0;
static u64 g_interval_max_us = 0;
static u64 g_intervals_us[MAX_INTERVALS];
static int  g_interval_n = 0;
static u64 g_frame_age_ms = 0;      /* 距上一帧的年龄 */
static u32 g_acq_errors = 0;        /* 采集返回错误次数 */
static u32 g_total_polls = 0;

/* ---- SDL ---- */
static SDL_Window   *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static TTF_Font *g_font = NULL;      /* 正文 */
static TTF_Font *g_font_title = NULL;
static SDL_Texture *g_img_tex = NULL;/* IR 图像纹理(当前格式尺寸) */
static u32 *g_rgba = NULL;           /* 320*240 的 RGBA 转换缓冲 */

/* ---- 资源(经 objcopy 嵌入的字体) ---- */
__attribute__((weak)) extern const unsigned char _binary_NotoSansCJKsc_Regular_otf_start[];
__attribute__((weak)) extern const unsigned char _binary_NotoSansCJKsc_Regular_otf_end[];

static u64 now_ns(void) {
    return armTicksToNs(armGetSystemTick());
}

static const ir_format_t *cur_fmt(void) {
    return &g_formats[g_fmt_idx];
}

static TTF_Font *open_font(int ptsize) {
    TTF_Font *f = TTF_OpenFont("romfs:/NotoSansCJKsc-Regular.otf", ptsize);
    if (f) return f;
    if (_binary_NotoSansCJKsc_Regular_otf_start != NULL) {
        size_t size = (size_t)(_binary_NotoSansCJKsc_Regular_otf_end -
                               _binary_NotoSansCJKsc_Regular_otf_start);
        if (size > 0 && size < 0x7FFFFFFF) {
            SDL_RWops *rw = SDL_RWFromConstMem(_binary_NotoSansCJKsc_Regular_otf_start,
                                               (int)size);
            if (rw) return TTF_OpenFontRW(rw, 1, ptsize);
        }
    }
    return NULL;
}

static void draw_text(TTF_Font *f, int x, int y, SDL_Color c, const char *s) {
    if (!f || !s) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
    if (tex) {
        SDL_Rect d = { x, y, surf->w, surf->h };
        SDL_RenderCopy(g_ren, tex, NULL, &d);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

/* 释放并重建图像纹理为当前格式尺寸 */
static void rebuild_img_texture(void) {
    if (g_img_tex) { SDL_DestroyTexture(g_img_tex); g_img_tex = NULL; }
    int w = cur_fmt()->w, h = cur_fmt()->h;
    g_img_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
}

/* 停止并重启 ImageTransferProcessor(应用当前格式 + light_target),重置计时 */
static void restart_processor(void) {
    if (g_ir_ready) irsStopImageProcessor(g_ir);

    IrsImageTransferProcessorConfig cfg;
    irsGetDefaultImageTransferProcessorConfig(&cfg);
    cfg.format = cur_fmt()->fmt;
    cfg.light_target = g_light_target;

    Result rc = irsRunImageTransferProcessor(g_ir, &cfg, IR_WORK_SIZE);
    if (R_FAILED(rc))
        snprintf(g_ir_err, sizeof(g_ir_err),
                 "irsRunImageTransferProcessor: 0x%x", rc);
    else
        g_ir_err[0] = '\0';

    rebuild_img_texture();

    /* 重置采集/计时 */
    g_sampling = 0;
    g_have_frame = 0;
    g_got_first = 0;
    g_first_frame_ns = 0;
    g_interval_sum_us = 0;
    g_interval_max_us = 0;
    g_interval_n = 0;
    g_acq_errors = 0;
    g_total_polls = 0;
    g_frame_age_ms = 0;
    g_proc_start_ns = now_ns();
    g_last_sample_ns = 0;
}

/* 保存当前格式基准报告到 SD */
static void save_report(void) {
    mkdir("sdmc:/switch/deep-harness-switch", 0777);
    char path[128];
    snprintf(path, sizeof(path), "sdmc:/switch/deep-harness-switch/bench_%s.log",
             cur_fmt()->name);

    FILE *f = fopen(path, "w");
    if (!f) return;

    u64 median = 0, p95 = 0;
    if (g_interval_n > 0) {
        u64 sorted[MAX_INTERVALS];
        memcpy(sorted, g_intervals_us, sizeof(u64) * g_interval_n);
        /* 冒泡排序(基准数据量小) */
        for (int i = 0; i < g_interval_n - 1; i++)
            for (int j = i + 1; j < g_interval_n; j++)
                if (sorted[j] < sorted[i]) { u64 t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
        median = sorted[g_interval_n / 2];
        int pi = (int)(g_interval_n * 0.95);
        if (pi >= g_interval_n) pi = g_interval_n - 1;
        p95 = sorted[pi];
    }
    u64 avg = g_interval_n > 0 ? g_interval_sum_us / g_interval_n : 0;

    fprintf(f, "DEEP HARNESS SWITCH - IRS BENCH\n");
    fprintf(f, "format: %s\n", cur_fmt()->name);
    fprintf(f, "resolution: %dx%d\n", cur_fmt()->w, cur_fmt()->h);
    fprintf(f, "light_target: %u\n", g_light_target);
    fprintf(f, "samples: %d\n", g_interval_n);
    fprintf(f, "first_frame_ms: %llu\n",
            (unsigned long long)(g_got_first ? g_first_frame_ns / 1000000 : 0));
    fprintf(f, "avg_interval_us: %llu\n", (unsigned long long)avg);
    fprintf(f, "median_interval_us: %llu\n", (unsigned long long)median);
    fprintf(f, "p95_interval_us: %llu\n", (unsigned long long)p95);
    fprintf(f, "max_interval_us: %llu\n", (unsigned long long)g_interval_max_us);
    fprintf(f, "acq_errors: %u\n", g_acq_errors);
    fprintf(f, "total_polls: %u\n", g_total_polls);
    fprintf(f, "--- intervals (us) ---\n");
    for (int i = 0; i < g_interval_n; i++)
        fprintf(f, "%llu\n", (unsigned long long)g_intervals_us[i]);
    fclose(f);
}

/* ---- 采集一帧(在渲染前调用) ---- */
static void poll_irs(void) {
    if (!g_ir_ready || !g_ir_buffer) return;

    IrsImageTransferProcessorState st;
    g_total_polls++;
    Result rc = irsGetImageTransferProcessorState(g_ir, g_ir_buffer,
                                                  IR_BUF_SIZE, &st);
    u64 now = now_ns();

    if (R_FAILED(rc)) {
        g_acq_errors++; /* 尚无图像 / 传输错误 */
        g_frame_age_ms = (now - g_last_sample_ns) / 1000000;
        return;
    }

    /* 采样号变化 = 新的一帧 */
    if (st.sampling_number != g_sampling) {
        if (!g_got_first) {
            g_got_first = 1;
            g_first_frame_ns = now - g_proc_start_ns;
        } else if (g_last_sample_ns != 0) {
            u64 interval_us = (now - g_last_sample_ns) / 1000;
            g_interval_sum_us += interval_us;
            if (interval_us > g_interval_max_us) g_interval_max_us = interval_us;
            if (g_interval_n < MAX_INTERVALS)
                g_intervals_us[g_interval_n++] = interval_us;
        }
        g_last_sample_ns = now;
        g_sampling = st.sampling_number;
        g_have_frame = 1;
    }
    g_frame_age_ms = (now - g_last_sample_ns) / 1000000;
}

/* ---- 渲染 ---- */
static void render(void) {
    SDL_SetRenderDrawColor(g_ren, 12, 12, 14, 255);
    SDL_RenderClear(g_ren);

    /* 标题 */
    draw_text(g_font_title, 24, 12, (SDL_Color){235, 238, 242, 255},
              "DEEP HARNESS SWITCH - IRS BENCH");

    /* 图像区(左) */
    if (g_img_tex) {
        if (g_have_frame) {
            int w = cur_fmt()->w, h = cur_fmt()->h;
            for (int i = 0; i < w * h; i++) {
                u32 v = g_ir_buffer[i];
                g_rgba[i] = 0xFF000000u | (v << 16) | (v << 8) | v;
            }
            SDL_UpdateTexture(g_img_tex, NULL, g_rgba, w * 4);
        }
        SDL_Rect dst = { 24, 70, 640, 480 };
        SDL_RenderCopy(g_ren, g_img_tex, NULL, &dst);
        /* 边框 */
        SDL_SetRenderDrawColor(g_ren, 53, 54, 56, 255);
        SDL_Rect border = { 24, 70, 640, 480 };
        SDL_RenderDrawRect(g_ren, &border);
    }

    /* 状态区(右) */
    int x = 700, y = 70;
    SDL_Color c_text = {235, 238, 242, 255};
    SDL_Color c_dim  = {151, 157, 166, 255};
    SDL_Color c_ok   = {34, 197, 94, 255};
    SDL_Color c_bad  = {239, 68, 68, 255};

    char buf[256];

    if (!g_ir_ready) {
        draw_text(g_font, x, y, c_bad, "NO RIGHT JOY-CON IR CAMERA");
        draw_text(g_font, x, y + 40, c_dim, g_ir_err);
    } else {
        if (g_ir_err[0]) {
            draw_text(g_font, x, y, c_bad, g_ir_err);
            y += 44;
        }
        draw_text(g_font, x, y, c_text, "STATUS");
        y += 36;
        snprintf(buf, sizeof(buf), "FORMAT   %s (%dx%d)",
                 cur_fmt()->name, cur_fmt()->w, cur_fmt()->h);
        draw_text(g_font, x, y, c_ok, buf); y += 32;
        snprintf(buf, sizeof(buf), "LED      %s",
                 g_light_target == 0 ? "ALL ON" : "OFF");
        draw_text(g_font, x, y, c_text, buf); y += 32;
        snprintf(buf, sizeof(buf), "SAMPLE   %llu",
                 (unsigned long long)g_sampling);
        draw_text(g_font, x, y, c_text, buf); y += 32;
        snprintf(buf, sizeof(buf), "FRAME AGE %llu ms",
                 (unsigned long long)g_frame_age_ms);
        draw_text(g_font, x, y, c_text, buf); y += 32;

        y += 12;
        draw_text(g_font, x, y, c_text, "TIMING"); y += 36;
        snprintf(buf, sizeof(buf), "FIRST    %llu ms",
                 (unsigned long long)(g_got_first ? g_first_frame_ns / 1000000 : 0));
        draw_text(g_font, x, y, g_got_first ? c_ok : c_dim, buf); y += 32;
        snprintf(buf, sizeof(buf), "SAMPLES  %d", g_interval_n);
        draw_text(g_font, x, y, c_text, buf); y += 32;

        u64 avg = g_interval_n > 0 ? g_interval_sum_us / g_interval_n : 0;
        snprintf(buf, sizeof(buf), "AVG      %llu ms", (unsigned long long)(avg / 1000));
        draw_text(g_font, x, y, c_text, buf); y += 32;
        snprintf(buf, sizeof(buf), "MAX      %llu ms",
                 (unsigned long long)(g_interval_max_us / 1000));
        draw_text(g_font, x, y, c_text, buf); y += 32;
        snprintf(buf, sizeof(buf), "ERRORS   %u / %u polls", g_acq_errors, g_total_polls);
        draw_text(g_font, x, y, g_acq_errors ? c_bad : c_dim, buf); y += 32;
    }

    /* 底部操作提示 */
    draw_text(g_font, 24, WIN_H - 40, (SDL_Color){129, 133, 140, 255},
              "X 格式  A LED  B 重置  Y 存报告  + 退出");
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* SDL */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    g_win = SDL_CreateWindow("DEEP HARNESS SWITCH", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, WIN_W, WIN_H, 0);
    if (!g_win) return 1;
    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) return 1;
    if (TTF_Init() != 0) return 1;
    g_font = open_font(26);
    g_font_title = open_font(34);
    if (!g_font || !g_font_title) return 1;

    g_rgba = (u32 *)malloc(320 * 240 * sizeof(u32));
    g_ir_buffer = (u8 *)malloc(IR_BUF_SIZE);
    if (!g_rgba || !g_ir_buffer) return 1;
    memset(g_ir_buffer, 0, IR_BUF_SIZE);

    /* 输入 */
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    /* IRS 初始化 */
    Result rc = irsInitialize();
    if (R_FAILED(rc)) {
        snprintf(g_ir_err, sizeof(g_ir_err), "irsInitialize: 0x%x", rc);
    } else {
        /* 取右 Joy-Con 红外摄像头句柄(player 1 / 手持) */
        rc = irsGetIrCameraHandle(&g_ir, HidNpadIdType_No1);
        if (R_FAILED(rc))
            snprintf(g_ir_err, sizeof(g_ir_err), "irsGetIrCameraHandle: 0x%x", rc);
        else
            g_ir_ready = 1;
    }

    if (g_ir_ready) restart_processor();

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) break;
        if (kDown & HidNpadButton_X) {
            g_fmt_idx = (g_fmt_idx + 1) % N_FORMATS;
            restart_processor();
        }
        if (kDown & HidNpadButton_A) {
            g_light_target = (g_light_target == 0) ? 3 : 0;
            restart_processor();
        }
        if (kDown & HidNpadButton_B) {
            restart_processor();
        }
        if (kDown & HidNpadButton_Y) {
            save_report();
        }

        poll_irs();
        render();
        SDL_RenderPresent(g_ren);
    }

    /* 清理 */
    if (g_ir_ready) irsStopImageProcessor(g_ir);
    irsExit();
    if (g_img_tex) SDL_DestroyTexture(g_img_tex);
    free(g_rgba);
    free(g_ir_buffer);
    if (g_font) TTF_CloseFont(g_font);
    if (g_font_title) TTF_CloseFont(g_font_title);
    TTF_Quit();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
