/*
 * io_screen — 屏上放什么(内容层)。
 *
 * io_lcd 是"屏本身"(驱动 + 画布 + 文字);io_screen 是"内容从哪来"。
 *
 * 现在只有一个来源:共享盘上的 **SCRN.BMP** —— 目标机把抓到的画面缩到
 * 160x80 存成 BMP,设备发现它变了就画到屏上。
 *
 * ★ 为什么是 BMP 而不是 JPEG ★(用户 2026-10-25 提的,原方案是 JPEG,改了)
 *   目标机那边是 Windows + PowerShell,watcher 里已经有 GDI+ 的
 *   `Scale-Bitmap`(缩放)和 `LockBits`(整块 memcpy):
 *     → 存一个 160x80 的 BMP 是**零逐像素开销**的,脚本里一个像素都不用碰。
 *   而 JPEG 要在**设备**上解(熵解码 + 反量化 + IDCT),那才是贵的那个:
 *     picojpeg 解一张 160x80 约 10~25 ms,而 BGR888→RGB565 只要约 1 ms。
 *   ⇒ 编码的活儿交给"有 GDI+ 的目标机",设备只做颜色转换。
 *   代价:文件大一点(160x80x24bpp = 38 KB,裸 RGB565 只要 25.6 KB)——
 *   但那是 USB MSC 往 RAM disk 里写,实测速率够,而解码省下的是**每次**的。
 *
 * ★ 怎么发现"文件变了" ★
 *   FAT16 的 mtime 只有 **2 秒**精度 ⇒ 拿 mtime 当变化信号,缩略图最多
 *   0.5 fps(哪怕 watcher 写 2 fps)。所以这里改成:
 *     每 250 ms 读一遍文件(38 KB 从 RAM disk 读就是一次 memcpy,很便宜),
 *     算个 32 位 hash;hash 变了才解码+刷屏。
 *   ⇒ 屏上能到 ~4 fps,不用被 FAT 的时间精度卡住。
 *
 * ★ 撕裂帧怎么办 ★
 *   宿主写文件时设备可能读到写了一半的内容(FAT 没有原子替换)。
 *   两种兜底:① 设备侧:文件长度对不上 BMP 头就丢弃(已实现);
 *   ② 宿主侧:先写 SCRN.TMP 再改名成 SCRN.BMP —— 改名那一下目录项是
 *      一次改完的,设备要么看到旧文件、要么看到新文件,不会看到半个。
 */
#include "io_screen.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_heap_caps.h>

#include "io_disk.h"
#include "io_lcd.h"

static const char *TAG = "io_screen";

/* 宿主放缩略图的文件名(8.3)。 */
#define IO_SCREEN_FILE   "SCRN.BMP"

/* 轮询周期。目标机 2 fps 写帧,这里 250 ms 一次 ⇒ 不漏帧。 */
#define IO_SCREEN_POLL_MS 250

/* 读文件用的暂存缓冲:160x80x32bpp + 头 = 51.3 KB,留够 64 KB。 */
#define IO_SCREEN_BUF_BYTES (64 * 1024)

static uint8_t *s_buf;

/* 32 位 FNV-1a,用来判"文件内容变了没"。 */
static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void screen_task(void *arg)
{
    (void)arg;
    uint32_t last_hash = 0;
    bool first = true;

    /* 等屏起来(它是另一个 task 在 init) */
    while (!io_lcd_available()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(IO_SCREEN_POLL_MS));

        io_file_info_t info;
        if (!io_disk_find_file(IO_SCREEN_FILE, &info) || info.size == 0) {
            continue;               /* 宿主还没写,屏上保持原样 */
        }
        if (info.size > IO_SCREEN_BUF_BYTES) {
            ESP_LOGW(TAG, "%s too big (%u bytes) -- skipped",
                     IO_SCREEN_FILE, (unsigned)info.size);
            continue;
        }

        size_t got = 0;
        if (io_disk_read_file_at(IO_SCREEN_FILE, 0, s_buf,
                                 IO_SCREEN_BUF_BYTES, &got) != ESP_OK || got == 0) {
            continue;
        }

        const uint32_t h = fnv1a(s_buf, got);
        if (!first && h == last_hash) {
            continue;               /* 没变,不动屏 */
        }
        last_hash = h;
        first = false;

        if (io_lcd_load_bmp(s_buf, got) != ESP_OK) {
            ESP_LOGW(TAG, "%s is not a usable BMP -- skipped", IO_SCREEN_FILE);
            continue;
        }
        if (io_lcd_flush() != ESP_OK) {
            ESP_LOGW(TAG, "flush failed");
            continue;
        }
        ESP_LOGI(TAG, "displayed %s (%u bytes)", IO_SCREEN_FILE, (unsigned)got);
    }
}

esp_err_t io_screen_start(void)
{
    if (s_buf != NULL) {
        return ESP_OK;
    }
    s_buf = heap_caps_malloc(IO_SCREEN_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        s_buf = heap_caps_malloc(IO_SCREEN_BUF_BYTES, MALLOC_CAP_8BIT);
    }
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "no memory for %d byte staging buffer", IO_SCREEN_BUF_BYTES);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(screen_task, "screen", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create screen task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "watching %s on the shared disk", IO_SCREEN_FILE);
    return ESP_OK;
}
