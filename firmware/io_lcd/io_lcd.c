/*
 * io_lcd — ST7735S(SPI,MODE0)驱动 + 画布 + 文字渲染。
 * 硬件事实与依据见 include/io_lcd.h。
 */
#include "io_lcd.h"
#include "io_lcd_font.h"
#include "brand_screen.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char *TAG = "io_lcd";

#if CONFIG_SOPHON_LCD_ST7735

#define LCD_GPIO_SCLK   CONFIG_SOPHON_LCD_GPIO_SCLK
#define LCD_GPIO_MOSI   CONFIG_SOPHON_LCD_GPIO_MOSI
#define LCD_GPIO_CS     CONFIG_SOPHON_LCD_GPIO_CS
#define LCD_GPIO_DC     CONFIG_SOPHON_LCD_GPIO_DC
#define LCD_GPIO_RST    CONFIG_SOPHON_LCD_GPIO_RST

/*
 * 用 FSPI(SPI2_HOST)。
 *
 * 为什么不是 SPI3:N16R8 是**八线 PSRAM**(sdkconfig: CONFIG_SPIRAM_MODE_OCT=y)。
 * ESP32-S3 的 MSPI 块上,flash+PSRAM 占的是 **SPI0/SPI1**(见 IDF
 * components/esp_hw_support/mspi_timing_tuning/port/esp32s3/mspi_timing_config.c
 * 里的 "SPI0 and SPI1 share the register for core clock" + control_both_mspi),
 * SPI2/SPI3 才是留给外设的 ⇒ 屏不会跟 PSRAM 抢总线。
 */
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_SPI_HZ      (40 * 1000 * 1000)

/* ---- ST7735S 命令 ---- */
#define ST7735_SWRESET  0x01
#define ST7735_SLPOUT   0x11
#define ST7735_NORON    0x13
#define ST7735_INVOFF   0x20
#define ST7735_INVON    0x21
#define ST7735_DISPON   0x29
#define ST7735_CASET    0x2A
#define ST7735_RASET    0x2B
#define ST7735_RAMWR    0x2C
#define ST7735_MADCTL   0x36
#define ST7735_COLMOD   0x3A
#define ST7735_FRMCTR1  0xB1
#define ST7735_FRMCTR2  0xB2
#define ST7735_FRMCTR3  0xB3
#define ST7735_INVCTR   0xB4
#define ST7735_DISSET5  0xB6
#define ST7735_PWCTR1   0xC0
#define ST7735_PWCTR2   0xC1
#define ST7735_PWCTR3   0xC2
#define ST7735_PWCTR4   0xC3
#define ST7735_PWCTR5   0xC4
#define ST7735_VMCTR1   0xC5
#define ST7735_PWCTR6   0xFC
#define ST7735_GMCTRP1  0xE0
#define ST7735_GMCTRN1  0xE1

/* MADCTL 位 */
#define MADCTL_MY       0x80
#define MADCTL_MX       0x40
#define MADCTL_MV       0x20
#define MADCTL_ML       0x10
#define MADCTL_BGR      0x08
#define MADCTL_MH       0x04

/* 行缓冲:一次只搬一行,320 字节(横屏 160 像素)。放内部 RAM(SPI DMA 用)。 */
#define LCD_LINE_MAX    160
#define LCD_LINE_BYTES  (LCD_LINE_MAX * 2)

/* "字幕"层:最近一次 io_lcd_print 的文字,缩略图来了以后要压在图上面重画。 */
#define LCD_OVERLAY_MAX 192

/* 文字左上角留白。 */
#define LCD_TEXT_X      2
#define LCD_TEXT_Y      2

static spi_device_handle_t s_dev;
static bool     s_ready;
static int      s_width  = IO_LCD_PANEL_W_PORTRAIT;
static int      s_height = IO_LCD_PANEL_H_PORTRAIT;
static int      s_colstart = 26;
static int      s_rowstart = 1;
static uint16_t *s_fb;          /* 画布,PSRAM */
static uint8_t *s_line;         /* 字节交换后的一行,内部 RAM */
static char     s_overlay[LCD_OVERLAY_MAX];   /* 字幕:最近一次 print 的文字 */
static uint16_t s_overlay_fg = IO_LCD_WHITE;
static uint16_t s_overlay_bg = IO_LCD_BLACK;
static bool     s_overlay_set;

/* ------------------------------------------------------------------ */
/* 底层:发命令 / 发数据                                              */
/* ------------------------------------------------------------------ */

/*
 * D/C 线(DC)自己管,不依赖 IDF SPI 驱动在命令相位自动拉。
 *
 * 为什么要自己管:命令和数据要**分两次**传输(命令相位 DC=0、数据相位 DC=1),
 * 而一次事务里没法把 DC 切成两段。自己拉 GPIO 是唯一没有歧义的做法,
 * 代价只是每条命令多几十微秒 —— 初始化只有十几条命令,无所谓。
 */
static esp_err_t lcd_tx(const void *data, size_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_dev, &t);
}

static esp_err_t lcd_cmd(uint8_t cmd)
{
    gpio_set_level(LCD_GPIO_DC, 0);
    esp_err_t err = lcd_tx(&cmd, 1);
    gpio_set_level(LCD_GPIO_DC, 1);
    return err;
}

static esp_err_t lcd_data(const void *data, size_t len)
{
    gpio_set_level(LCD_GPIO_DC, 1);
    return lcd_tx(data, len);
}

/* ------------------------------------------------------------------ */
/* 初始化序列                                                          */
/* ------------------------------------------------------------------ */

/*
 * 序列取自厂商 demo 自带的 TFT_eSPI,ST7735_Init.h 里 GREENTAB160x80 的完整分派:
 *
 *     commandList(Rcmd1);            // 15 条
 *     commandList(Rcmd2green);       // 2 条:CASET/RASET
 *     writecommand(ST7735_INVON);    // ★ 160x80 这一档要"反色开"
 *     commandList(Rcmd3);            // ★ 4 条:GMCTRP1/GMCTRN1/NORON/DISPON
 *
 * ★ 踩过的坑:只抄 Rcmd1 + Rcmd2green 是**不够的** ★
 *   NORON(0x13) 和 DISPON(0x29) 在 Rcmd3 里,而 Rcmd3 很容易被漏掉 ——
 *   结果是**面板从来没被打开过**:SPI 事务全部成功、驱动自检也"画完了",
 *   但屏是全黑的、什么图案都没有。
 *   ⇒ 判定"驱动跑通了"不能只看 SPI 返回值,**屏上有没有东西才算数**。
 *
 * Rcmd2green 把 GRAM 地址窗口设成 列 2..129 / 行 1..160,
 * 真正的可见区靠 colstart/rowstart 再偏一次(竖屏 26/1)。
 */
typedef struct {
    uint8_t  cmd;
    uint8_t  nargs;
    uint16_t delay_ms;
    uint8_t  args[16];
} lcd_init_cmd_t;

static const lcd_init_cmd_t s_init_seq[] = {
    /* --- Rcmd1:7735R 公共初始化 --- */
    { ST7735_SWRESET, 0, 150, { 0 } },
    { ST7735_SLPOUT,  0, 500, { 0 } },
    { ST7735_FRMCTR1, 3,   0, { 0x01, 0x2C, 0x2D } },
    { ST7735_FRMCTR2, 3,   0, { 0x01, 0x2C, 0x2D } },
    { ST7735_FRMCTR3, 6,   0, { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D } },
    { ST7735_INVCTR,  1,   0, { 0x07 } },
    { ST7735_PWCTR1,  3,   0, { 0xA2, 0x02, 0x84 } },
    { ST7735_PWCTR2,  1,   0, { 0xC5 } },
    { ST7735_PWCTR3,  2,   0, { 0x0A, 0x00 } },
    { ST7735_PWCTR4,  2,   0, { 0x8A, 0x2A } },
    { ST7735_PWCTR5,  2,   0, { 0x8A, 0xEE } },
    { ST7735_VMCTR1,  1,   0, { 0x0E } },
    { ST7735_INVOFF,  0,   0, { 0 } },
    { ST7735_MADCTL,  1,   0, { 0xC0 | MADCTL_BGR } },
    { ST7735_COLMOD,  1,   0, { 0x05 } },
    /* --- Rcmd2green:绿版(0.96 寸 80x160 属于这一档) --- */
    { ST7735_CASET,   4,   0, { 0x00, 0x02, 0x00, 0x81 } },
    { ST7735_RASET,   4,   0, { 0x00, 0x01, 0x00, 0xA0 } },
    /* --- 160x80 专属:反色开 --- */
    { ST7735_INVON,   0,   0, { 0 } },
    /* --- Rcmd3:gamma + ★ 开机(缺这两条屏就是全黑) --- */
    { ST7735_GMCTRP1, 16,  0, { 0x02, 0x1C, 0x07, 0x12,
                               0x37, 0x32, 0x29, 0x2D,
                               0x29, 0x25, 0x2B, 0x39,
                               0x00, 0x01, 0x03, 0x10 } },
    { ST7735_GMCTRN1, 16,  0, { 0x03, 0x1D, 0x07, 0x06,
                               0x2E, 0x2C, 0x29, 0x2D,
                               0x2E, 0x2E, 0x37, 0x3F,
                               0x00, 0x00, 0x02, 0x10 } },
    { ST7735_NORON,   0,  10, { 0 } },
    { ST7735_DISPON,  0, 100, { 0 } },
};

static esp_err_t lcd_run_init_seq(void)
{
    for (size_t i = 0; i < sizeof(s_init_seq) / sizeof(s_init_seq[0]); i++) {
        const lcd_init_cmd_t *c = &s_init_seq[i];
        ESP_RETURN_ON_ERROR(lcd_cmd(c->cmd), TAG, "init cmd 0x%02X", c->cmd);
        if (c->nargs > 0) {
            ESP_RETURN_ON_ERROR(lcd_data(c->args, c->nargs), TAG,
                                "init data 0x%02X", c->cmd);
        }
        if (c->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    return ESP_OK;
}

/* 设置可见窗口(x0,y0)-(x1,y1),并进入写显存状态。 */
static esp_err_t lcd_set_window(int x0, int y0, int x1, int y1)
{
    const int cx0 = x0 + s_colstart;
    const int cx1 = x1 + s_colstart;
    const int ry0 = y0 + s_rowstart;
    const int ry1 = y1 + s_rowstart;
    uint8_t buf[4];

    ESP_RETURN_ON_ERROR(lcd_cmd(ST7735_CASET), TAG, "CASET");
    buf[0] = (uint8_t)(cx0 >> 8); buf[1] = (uint8_t)(cx0 & 0xFF);
    buf[2] = (uint8_t)(cx1 >> 8); buf[3] = (uint8_t)(cx1 & 0xFF);
    ESP_RETURN_ON_ERROR(lcd_data(buf, 4), TAG, "CASET data");

    ESP_RETURN_ON_ERROR(lcd_cmd(ST7735_RASET), TAG, "RASET");
    buf[0] = (uint8_t)(ry0 >> 8); buf[1] = (uint8_t)(ry0 & 0xFF);
    buf[2] = (uint8_t)(ry1 >> 8); buf[3] = (uint8_t)(ry1 & 0xFF);
    ESP_RETURN_ON_ERROR(lcd_data(buf, 4), TAG, "RASET data");

    ESP_RETURN_ON_ERROR(lcd_cmd(ST7735_RAMWR), TAG, "RAMWR");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 对外 API                                                            */
/* ------------------------------------------------------------------ */

esp_err_t io_lcd_set_rotation(uint8_t rotation)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t madctl;
    switch (rotation & 3) {
    case 0:
        madctl = MADCTL_MX | MADCTL_MY | MADCTL_MH | MADCTL_BGR;
        s_colstart = 26; s_rowstart = 1;
        s_width = IO_LCD_PANEL_W_PORTRAIT; s_height = IO_LCD_PANEL_H_PORTRAIT;
        break;
    case 1:
        madctl = MADCTL_MV | MADCTL_MY | MADCTL_BGR;
        s_colstart = 1;  s_rowstart = 26;
        s_width = IO_LCD_PANEL_H_PORTRAIT; s_height = IO_LCD_PANEL_W_PORTRAIT;
        break;
    case 2:
        madctl = MADCTL_BGR;
        s_colstart = 26; s_rowstart = 1;
        s_width = IO_LCD_PANEL_W_PORTRAIT; s_height = IO_LCD_PANEL_H_PORTRAIT;
        break;
    default:
        madctl = MADCTL_MX | MADCTL_MV | MADCTL_BGR;
        s_colstart = 1;  s_rowstart = 26;
        s_width = IO_LCD_PANEL_H_PORTRAIT; s_height = IO_LCD_PANEL_W_PORTRAIT;
        break;
    }
    ESP_RETURN_ON_ERROR(lcd_cmd(ST7735_MADCTL), TAG, "MADCTL");
    ESP_RETURN_ON_ERROR(lcd_data(&madctl, 1), TAG, "MADCTL data");
    ESP_LOGI(TAG, "rotation %u -> %dx%d (colstart=%d rowstart=%d)",
             rotation & 3, s_width, s_height, s_colstart, s_rowstart);
    if (s_fb) {
        memset(s_fb, 0, (size_t)s_width * s_height * 2);
    }
    return ESP_OK;
}

int io_lcd_width(void)
{
    return s_ready ? s_width : 0;
}

int io_lcd_height(void)
{
    return s_ready ? s_height : 0;
}

bool io_lcd_available(void)
{
    return s_ready;
}

uint16_t *io_lcd_fb(void)
{
    return s_fb;
}

esp_err_t io_lcd_clear(void)
{
    if (!s_ready || s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_fb, 0, (size_t)s_width * s_height * 2);
    return ESP_OK;
}

esp_err_t io_lcd_brand_show(void)
{
    if (!s_ready || s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 位图是按横屏 160x80 生成的;分辨率不对就别画(错位比不画更糟) */
    if (s_width != IO_BRAND_W || s_height != IO_BRAND_H) {
        ESP_LOGW(TAG, "brand screen is %dx%d but panel is %dx%d -- skipped",
                 IO_BRAND_W, IO_BRAND_H, s_width, s_height);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *p = io_brand_pixels;
    for (int y = 0; y < IO_BRAND_H; y++) {
        uint16_t *line = s_fb + (size_t)y * s_width;
        for (int x = 0; x < IO_BRAND_W; x += 4) {
            const uint8_t b = *p++;
            /* 每字节 4 个像素,高位在前 */
            line[x + 0] = io_brand_palette[(b >> 6) & 0x03];
            line[x + 1] = io_brand_palette[(b >> 4) & 0x03];
            line[x + 2] = io_brand_palette[(b >> 2) & 0x03];
            line[x + 3] = io_brand_palette[(b >> 0) & 0x03];
        }
    }
    return ESP_OK;
}

esp_err_t io_lcd_fill_rect(int x, int y, int w, int h, uint16_t rgb565)
{
    if (!s_ready || s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 裁到可见区 */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_width)  { w = s_width - x; }
    if (y + h > s_height) { h = s_height - y; }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }
    for (int row = y; row < y + h; row++) {
        uint16_t *line = s_fb + (size_t)row * s_width;
        for (int col = x; col < x + w; col++) {
            line[col] = rgb565;
        }
    }
    return ESP_OK;
}

int io_lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    return io_lcd_text_scaled(x, y, s, fg, bg, 1);
}

int io_lcd_text_scaled(int x, int y, const char *s,
                       uint16_t fg, uint16_t bg, int scale)
{
    if (!s_ready || s_fb == NULL || s == NULL) {
        return -1;
    }
    if (scale < 1)  scale = 1;
    if (scale > 4)  scale = 4;

    const int cell_w = IO_LCD_FONT5X7_CELL_W * scale;
    const int cell_h = IO_LCD_FONT5X7_CELL_H * scale;
    int drawn = 0;
    for (const char *p = s; *p; p++) {
        const int cx = x + drawn * cell_w;
        if (cx >= s_width) {
            break;
        }
        const unsigned char code = (unsigned char)*p;
        const uint8_t *g = NULL;
        if (code >= IO_LCD_FONT5X7_FIRST &&
            code < IO_LCD_FONT5X7_FIRST + IO_LCD_FONT5X7_GLYPHS) {
            g = io_lcd_font5x7[code - IO_LCD_FONT5X7_FIRST];
        }
        /*
         * 字模是**列优先**:g[c] 是一列,它的 bit r 是第 r 行。
         * 所以要按行取位(先定行,再定列),不是按列取位。
         * 放大 = 一个字模像素铺成 scale x scale 的方块。
         */
        for (int r = 0; r < cell_h; r++) {
            const int py = y + r;
            if (py < 0 || py >= s_height) {
                continue;
            }
            uint16_t *line = s_fb + (size_t)py * s_width;
            for (int c = 0; c < cell_w; c++) {
                const int px = cx + c;
                if (px < 0 || px >= s_width) {
                    continue;
                }
                const bool ink = (g != NULL) &&
                                 (c / scale < IO_LCD_FONT5X7_W) &&
                                 (r / scale < IO_LCD_FONT5X7_H) &&
                                 ((g[c / scale] >> (r / scale)) & 1);
                line[px] = ink ? fg : bg;
            }
        }
        drawn++;
    }
    return drawn;
}

/*
 * 画一段文字到画布(自动按屏宽折行,'\n' 强制换行)。**不清屏**。
 * @return 画了几行
 */
static int draw_text_block(const char *s, uint16_t fg, uint16_t bg)
{
    const int cols = s_width / IO_LCD_FONT5X7_CELL_W;
    const int rows = s_height / IO_LCD_FONT5X7_CELL_H;
    int row = 0, col = 0;

    if (s != NULL) {
        for (const char *p = s; *p; p++) {
            if (*p == '\n') {
                row++;
                col = 0;
                if (row >= rows) {
                    break;
                }
                continue;
            }
            if (col >= cols) {
                row++;
                col = 0;
                if (row >= rows) {
                    break;
                }
            }
            char ch[2] = { *p, 0 };
            io_lcd_text(LCD_TEXT_X + col * IO_LCD_FONT5X7_CELL_W,
                        LCD_TEXT_Y + row * IO_LCD_FONT5X7_CELL_H, ch, fg, bg);
            col++;
        }
    }
    return row + 1;
}

int io_lcd_print(const char *s, uint16_t fg, uint16_t bg)
{
    if (!s_ready || s_fb == NULL) {
        return -1;
    }
    /* 清画布 */
    io_lcd_clear();

    const int lines = draw_text_block(s, fg, bg);

    /*
     * 记住这段文字。
     *
     * 为什么:缩略图是"底图",agent 写的这段是"字幕" —— 底图换新的时候,
     * 字幕要**压在图上面**重画一遍,否则 agent 写的话两秒就被帧刷掉了。
     */
    if (s != NULL) {
        strncpy(s_overlay, s, sizeof(s_overlay) - 1);
        s_overlay[sizeof(s_overlay) - 1] = '\0';
        s_overlay_fg = fg;
        s_overlay_bg = bg;
        s_overlay_set = true;
    } else {
        s_overlay_set = false;
    }

    io_lcd_flush();
    return lines;
}

/* ------------------------------------------------------------------ */
/* BMP 解码(底图)                                                     */
/* ------------------------------------------------------------------ */

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t io_lcd_load_bmp(const uint8_t *data, size_t len)
{
    if (!s_ready || s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* BMP 文件头 14 字节 + BITMAPINFOHEADER 40 字节 */
    if (data == NULL || len < 54) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t data_off = rd32le(data + 10);
    const int32_t  w        = (int32_t)rd32le(data + 18);
    const int32_t  h        = (int32_t)rd32le(data + 22);
    const uint16_t bpp      = rd16le(data + 28);
    const uint32_t comp     = rd32le(data + 30);

    if (comp != 0) {                    /* 只支持 BI_RGB(未压缩) */
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (bpp != 24 && bpp != 32) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (w <= 0 || h == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int bytes_pp = (int)(bpp / 8);
    const int stride   = ((int)w * bytes_pp + 3) & ~3;   /* 每行 4 字节对齐 */
    const int rows     = (h < 0) ? -h : h;                /* 负高度 = 自上而下 */
    const bool top_down = (h < 0);

    if ((size_t)data_off + (size_t)stride * (size_t)rows > len) {
        return ESP_ERR_INVALID_SIZE;    /* 文件被截断(宿主正写到一半) */
    }

    for (int y = 0; y < rows && y < s_height; y++) {
        /* BMP 默认自下而上存 ⇒ 目标第 y 行取源里的 rows-1-y 行 */
        const int src_row = top_down ? y : (rows - 1 - y);
        const uint8_t *src = data + data_off + (size_t)src_row * stride;
        uint16_t *dst = s_fb + (size_t)y * s_width;
        for (int x = 0; x < w && x < s_width; x++) {
            /* BMP 内存里是 BGR */
            const uint8_t b = src[x * bytes_pp + 0];
            const uint8_t g = src[x * bytes_pp + 1];
            const uint8_t r = src[x * bytes_pp + 2];
            dst[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }

    /* 字幕压在图上面重画一遍 */
    if (s_overlay_set) {
        draw_text_block(s_overlay, s_overlay_fg, s_overlay_bg);
    }
    return ESP_OK;
}

esp_err_t io_lcd_flush(void)
{
    if (!s_ready || s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(lcd_set_window(0, 0, s_width - 1, s_height - 1),
                        TAG, "window");

    for (int row = 0; row < s_height; row++) {
        const uint16_t *src = s_fb + (size_t)row * s_width;
        for (int i = 0; i < s_width; i++) {
            const uint16_t c = src[i];
            s_line[2 * i]     = (uint8_t)(c >> 8);
            s_line[2 * i + 1] = (uint8_t)(c & 0xFF);
        }
        ESP_RETURN_ON_ERROR(lcd_data(s_line, (size_t)s_width * 2), TAG, "flush row");
    }
    return ESP_OK;
}

esp_err_t io_lcd_blank(void)
{
    ESP_RETURN_ON_ERROR(io_lcd_clear(), TAG, "clear");
    return io_lcd_flush();
}

esp_err_t io_lcd_selftest(void)
{
    if (!s_ready || s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    static const uint16_t bars[8] = {
        0xF800, /* 红 */
        0x07E0, /* 绿 */
        0x001F, /* 蓝 */
        0xFFFF, /* 白 */
        0x0000, /* 黑 */
        0xFFE0, /* 黄 */
        0xF81F, /* 品红 */
        0x07FF, /* 青 */
    };
    const int n = (int)(sizeof(bars) / sizeof(bars[0]));
    const int bw = s_width / n;

    memset(s_fb, 0, (size_t)s_width * s_height * 2);
    for (int i = 0; i < n; i++) {
        io_lcd_fill_rect(i * bw, 0, bw, s_height, bars[i]);
    }
    /* 白边:偏移错位时会露出来,一眼能看出来 */
    io_lcd_fill_rect(0, 0, s_width, 1, 0xFFFF);
    io_lcd_fill_rect(0, s_height - 1, s_width, 1, 0xFFFF);
    io_lcd_fill_rect(0, 0, 1, s_height, 0xFFFF);
    io_lcd_fill_rect(s_width - 1, 0, 1, s_height, 0xFFFF);

    ESP_LOGI(TAG, "selftest drawn (%dx%d, %d bars)", s_width, s_height, n);
    return io_lcd_flush();
}

esp_err_t io_lcd_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* 行缓冲:320 字节,内部 RAM(SPI DMA 要能访问) */
    s_line = heap_caps_malloc(LCD_LINE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_line == NULL) {
        ESP_LOGE(TAG, "no memory for line buffer");
        return ESP_ERR_NO_MEM;
    }

    /* D/C 与 RST 是普通 GPIO;CS 交给 SPI 驱动(spics_io_num) */
    const gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << LCD_GPIO_DC) | (1ULL << LCD_GPIO_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio config");

    /* 复位时序:低 >=20 ms,再拉高等 >=120 ms */
    gpio_set_level(LCD_GPIO_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_GPIO_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_GPIO_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = LCD_GPIO_MOSI,
        .miso_io_num = -1,           /* 屏是只写的 */
        .sclk_io_num = LCD_GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_LINE_BYTES,
        .flags = 0,
        .intr_flags = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init");

    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = LCD_SPI_HZ,
        .mode = 0,                   /* MODE0 —— 面板要求 */
        .spics_io_num = LCD_GPIO_CS,
        .queue_size = 1,
        .flags = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_SPI_HOST, &dev_cfg, &s_dev),
                        TAG, "spi add device");

    /* s_ready 必须在第一次 lcd_cmd 之前置位 —— 底层要检查它 */
    s_ready = true;

    ESP_RETURN_ON_ERROR(lcd_run_init_seq(), TAG, "init sequence");

    /* 旋转先定下来,画布尺寸才对 */
    ESP_RETURN_ON_ERROR(io_lcd_set_rotation(CONFIG_SOPHON_LCD_ROTATION),
                        TAG, "rotation");

    /* 画布放 PSRAM(25.6 KB)—— 设备有 8 MB,内部 RAM 只有 512 KB,别抢 */
    const size_t fb_bytes = (size_t)s_width * s_height * sizeof(uint16_t);
    s_fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_fb == NULL) {
        s_fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_8BIT);
    }
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "no memory for %u byte framebuffer", (unsigned)fb_bytes);
        s_ready = false;
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, fb_bytes);

    ESP_LOGI(TAG, "ST7735S ready: SCLK=%d MOSI=%d CS=%d DC=%d RST=%d @ %d Hz, %dx%d",
             LCD_GPIO_SCLK, LCD_GPIO_MOSI, LCD_GPIO_CS, LCD_GPIO_DC,
             LCD_GPIO_RST, LCD_SPI_HZ, s_width, s_height);

    return ESP_OK;
}

#else /* !CONFIG_SOPHON_LCD_ST7735 */

/* 编译期关掉:全部空实现,不建总线、不占 GPIO、零开销。 */

esp_err_t io_lcd_init(void)                { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t io_lcd_set_rotation(uint8_t r)    { (void)r; return ESP_ERR_NOT_SUPPORTED; }
int      io_lcd_width(void)                 { return 0; }
int      io_lcd_height(void)                { return 0; }
bool     io_lcd_available(void)             { return false; }
uint16_t *io_lcd_fb(void)                   { return NULL; }
esp_err_t io_lcd_flush(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t io_lcd_clear(void)  { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t io_lcd_brand_show(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t io_lcd_fill_rect(int x, int y, int w, int h, uint16_t c)
{ (void)x; (void)y; (void)w; (void)h; (void)c; return ESP_ERR_NOT_SUPPORTED; }
int      io_lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{ (void)x; (void)y; (void)s; (void)fg; (void)bg; return -1; }
int      io_lcd_text_scaled(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sc)
{ (void)x; (void)y; (void)s; (void)fg; (void)bg; (void)sc; return -1; }
int      io_lcd_print(const char *s, uint16_t fg, uint16_t bg)
{ (void)s; (void)fg; (void)bg; return -1; }
esp_err_t io_lcd_load_bmp(const uint8_t *d, size_t n)
{ (void)d; (void)n; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t io_lcd_blank(void)                { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t io_lcd_selftest(void)             { return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_SOPHON_LCD_ST7735 */
