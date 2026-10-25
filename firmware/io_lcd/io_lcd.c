/*
 * io_lcd — ST7735S(SPI,MODE0)驱动。硬件事实与依据见 include/io_lcd.h。
 */
#include "io_lcd.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char *TAG = "io_lcd";

/* 16 位色 */
#define IO_LCD_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

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

/* 行缓冲:一次只搬一行,320 字节(横屏 160 像素)。省 PSRAM,也省 DMA 带宽。 */
#define LCD_LINE_MAX    160
#define LCD_LINE_BYTES  (LCD_LINE_MAX * 2)

static spi_device_handle_t s_dev;
static bool     s_ready;
static int      s_width  = IO_LCD_PANEL_W_PORTRAIT;
static int      s_height = IO_LCD_PANEL_H_PORTRAIT;
static int      s_colstart = 26;
static int      s_rowstart = 1;
static uint8_t *s_line;          /* 字节交换后的一行 */

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
 * 序列取自厂商 demo 自带的 TFT_eSPI:
 *   ST7735_Init.h 里 Rcmd1(15 条)+ Rcmd2green(2 条),
 *   而 ST7735_Init.h 的分派处明确写着 GREENTAB160x80 走的就是这两张表:
 *       else if (tabcolor == INITR_GREENTAB160x80)
 *           commandList(Rcmd1); commandList(Rcmd2green);
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
    /* Rcmd1 —— 7735R 公共初始化 */
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
    /* Rcmd2green —— 绿版(0.96 寸 80x160 属于这一档) */
    { ST7735_CASET,   4,   0, { 0x00, 0x02, 0x00, 0x81 } },
    { ST7735_RASET,   4,   0, { 0x00, 0x01, 0x00, 0xA0 } },
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

/* 把一行像素写成"高字节在前"的 RGB565。 */
static void lcd_pack_line(uint8_t *dst, const uint16_t *src, int n)
{
    for (int i = 0; i < n; i++) {
        const uint16_t c = src[i];
        dst[2 * i]     = (uint8_t)(c >> 8);
        dst[2 * i + 1] = (uint8_t)(c & 0xFF);
    }
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
    return io_lcd_blank();
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

esp_err_t io_lcd_fill_rect(int x, int y, int w, int h, uint16_t rgb565)
{
    if (!s_ready) {
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

    ESP_RETURN_ON_ERROR(lcd_set_window(x, y, x + w - 1, y + h - 1), TAG, "window");

    for (int i = 0; i < w; i++) {
        s_line[2 * i]     = (uint8_t)(rgb565 >> 8);
        s_line[2 * i + 1] = (uint8_t)(rgb565 & 0xFF);
    }
    for (int row = 0; row < h; row++) {
        ESP_RETURN_ON_ERROR(lcd_data(s_line, (size_t)w * 2), TAG, "fill row");
    }
    return ESP_OK;
}

esp_err_t io_lcd_present(const uint16_t *pixels)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(lcd_set_window(0, 0, s_width - 1, s_height - 1), TAG, "window");

    for (int row = 0; row < s_height; row++) {
        lcd_pack_line(s_line, pixels + (size_t)row * s_width, s_width);
        ESP_RETURN_ON_ERROR(lcd_data(s_line, (size_t)s_width * 2), TAG, "present row");
    }
    return ESP_OK;
}

esp_err_t io_lcd_blank(void)
{
    return io_lcd_fill_rect(0, 0, s_width, s_height, 0x0000);
}

esp_err_t io_lcd_selftest(void)
{
    if (!s_ready) {
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

    for (int i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(io_lcd_fill_rect(i * bw, 0, bw, s_height, bars[i]),
                            TAG, "bar %d", i);
    }
    /* 白边:偏移错位时会露出来,一眼能看出来 */
    ESP_RETURN_ON_ERROR(io_lcd_fill_rect(0, 0, s_width, 1, 0xFFFF), TAG, "border top");
    ESP_RETURN_ON_ERROR(io_lcd_fill_rect(0, s_height - 1, s_width, 1, 0xFFFF), TAG, "border bottom");
    ESP_RETURN_ON_ERROR(io_lcd_fill_rect(0, 0, 1, s_height, 0xFFFF), TAG, "border left");
    ESP_RETURN_ON_ERROR(io_lcd_fill_rect(s_width - 1, 0, 1, s_height, 0xFFFF), TAG, "border right");

    ESP_LOGI(TAG, "selftest drawn (%dx%d, %d bars)", s_width, s_height, n);
    return ESP_OK;
}

esp_err_t io_lcd_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* 行缓冲:320 字节,DMA 可用 */
    s_line = heap_caps_malloc(LCD_LINE_BYTES, MALLOC_CAP_DMA);
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

    ESP_LOGI(TAG, "ST7735S ready: SCLK=%d MOSI=%d CS=%d DC=%d RST=%d @ %d Hz",
             LCD_GPIO_SCLK, LCD_GPIO_MOSI, LCD_GPIO_CS, LCD_GPIO_DC,
             LCD_GPIO_RST, LCD_SPI_HZ);

    return io_lcd_set_rotation(CONFIG_SOPHON_LCD_ROTATION);
}

#else /* !CONFIG_SOPHON_LCD_ST7735 */

/* 编译期关掉:全部空实现,不建总线、不占 GPIO、零开销。 */

esp_err_t io_lcd_init(void)                { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t io_lcd_set_rotation(uint8_t r)    { (void)r; return ESP_ERR_NOT_SUPPORTED; }
int      io_lcd_width(void)                 { return 0; }
int      io_lcd_height(void)                { return 0; }
bool     io_lcd_available(void)             { return false; }
esp_err_t io_lcd_fill_rect(int x, int y, int w, int h, uint16_t c)
{ (void)x; (void)y; (void)w; (void)h; (void)c; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t io_lcd_present(const uint16_t *p) { (void)p; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t io_lcd_blank(void)                { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t io_lcd_selftest(void)             { return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_SOPHON_LCD_ST7735 */
