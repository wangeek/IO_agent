/*
 * io_led — 把设备状态码真正点亮到实体 LED(WS2812,可选)。详见 include/io_led.h。
 */
#include "io_led.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <driver/rmt_tx.h>

#include "io_launcher.h"
#include "io_net.h"

static const char *TAG = "io_led";

/*
 * 状态码的计算**不依赖 LED 硬件** —— 编译期把灯关掉时,
 * `status.led` 仍然要给出正确的值(它是设备状态的一部分,不是灯的属性)。
 */
static volatile int s_busy_count;

int io_led_state(void)
{
    if (s_busy_count > 0) {
        return IO_LED_ACT_BUSY;          /* 动作执行中,优先级最高 */
    }
    if (!io_launcher_last_selftest_ok()) {
        return IO_LED_FAULT;             /* 上次注入自检失败 */
    }
    if (!io_net_connect_started()) {
        return IO_LED_BOOTING;           /* 还没发起过连接 */
    }
    if (!io_net_is_connected()) {
        return IO_LED_NET_DOWN;
    }
    return IO_LED_READY;
}

void io_led_busy_acquire(void)
{
    s_busy_count++;
}

void io_led_busy_release(void)
{
    if (s_busy_count > 0) {
        s_busy_count--;
    }
}

#if CONFIG_SOPHON_LED_WS2812

#define LED_COUNT        CONFIG_SOPHON_LED_COUNT
#define LED_GPIO         CONFIG_SOPHON_LED_GPIO

/* RMT 时钟分辨率 8 MHz ⇒ 1 tick = 125 ns */
#define LED_RESOLUTION_HZ    8000000
/* WS2812 位时序(取整到 8 MHz 的整数 tick) */
#define LED_T0H             3       /* 375 ns */
#define LED_T0L             7       /* 875 ns */
#define LED_T1H             6       /* 750 ns */
#define LED_T1L             4       /* 500 ns */
#define LED_RESET_TICKS     400     /* 50 µs 复位(要求 >50 µs) */
/* 一个像素 = 3 字节 = 24 bit,每 bit 一个 RMT word */
#define LED_WORDS_PER_PX    24
#define LED_WORDS_MAX       (LED_COUNT * LED_WORDS_PER_PX + 1)

/* 刷新节拍与闪烁周期 */
#define LED_TICK_MS         50
#define LED_BLINK_FAST_MS   100     /* 启动中:亮 100 / 灭 100 */
#define LED_BLINK_SLOW_MS   500     /* 网络未就绪:亮 500 / 灭 500 */

/* 亮度:全亮 255 太刺眼,半亮够看 */
#define LED_BRIGHT          128

/* WS2812 的字节序是 **G,R,B** */
static const uint8_t s_color[5][3] = {
    {LED_BRIGHT, 0, 0},         /* 0 就绪        绿 */
    {0, 0, LED_BRIGHT},         /* 1 启动中      蓝 */
    {LED_BRIGHT, 100, 0},       /* 2 网络未就绪  黄 */
    {0, 0, LED_BRIGHT},         /* 3 动作执行中  蓝 */
    {0, LED_BRIGHT, 0},         /* 4 故障        红 */
};

static rmt_channel_handle_t s_chan;
static int s_last_state = -1;
static bool s_last_on;
static bool s_blink_phase;

/* ------------------------------------------------------------------ */

static void encode_px(rmt_symbol_word_t *items, size_t *n, const uint8_t *grb)
{
    for (int byte = 0; byte < 3; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            const bool one = (grb[byte] >> bit) & 1;
            items[*n].level0 = 1;
            items[*n].duration0 = one ? LED_T1H : LED_T0H;
            items[*n].level1 = 0;
            items[*n].duration1 = one ? LED_T1L : LED_T0L;
            (*n)++;
        }
    }
}

static void ws2812_write(const uint8_t *grb)
{
    rmt_symbol_word_t items[LED_WORDS_MAX];
    size_t n = 0;

    /*
     * 同一串灯的所有像素必须**在一次事务里连续发完**:
     * 中间断一下(哪怕是另一个事务)就会被 WS2812 当成复位,latch 出半截数据。
     */
    for (int px = 0; px < LED_COUNT; px++) {
        encode_px(items, &n, grb);
    }
    /* 复位:拉低 >50 µs,让灯 latch */
    items[n].level0 = 0;
    items[n].duration0 = LED_RESET_TICKS;
    items[n].level1 = 0;
    items[n].duration1 = 0;
    n++;

    rmt_transmit_config_t tx = { .loop_count = -1 };
    esp_err_t err = rmt_transmit(s_chan, items, n, &tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
    }
}

static void show(int state, bool on)
{
    uint8_t grb[3] = {0, 0, 0};
    if (on) {
        memcpy(grb, s_color[state], sizeof(grb));
    }
    ws2812_write(grb);
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t phase = 0;

    for (;;) {
        const int st = io_led_state();
        bool on = true;

        if (st == IO_LED_BOOTING || st == IO_LED_NET_DOWN) {
            const uint32_t period = (st == IO_LED_BOOTING) ? LED_BLINK_FAST_MS
                                                          : LED_BLINK_SLOW_MS;
            phase += LED_TICK_MS;
            if (phase >= period) {
                phase = 0;
                s_blink_phase = !s_blink_phase;
            }
            on = s_blink_phase;
        }

        /* 只在"状态变了"或"亮灭变了"时才发一帧(省电,也省 RMT) */
        if (st != s_last_state || on != s_last_on) {
            show(st, on);
            s_last_state = st;
            s_last_on = on;
        }
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

esp_err_t io_led_init(void)
{
    rmt_tx_channel_config_t tx = {
        .gpio_num = LED_GPIO,
        .clk_src = RMT_CLK_SRC_APB,
        .resolution_hz = LED_RESOLUTION_HZ,
        .mem_block_symbols = LED_WORDS_MAX,
        .trans_queue_depth = 4,
        .intr_priority = 0,
        .flags = { .invert_out = false, .with_dma = false },
    };
    const esp_err_t err = rmt_new_tx_channel(&tx, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        s_chan = NULL;
        return err;
    }
    /* 空闲电平必须是低(WS2812 用低电平复位) */
    rmt_tx_set_idle_level(s_chan, 0);

    if (xTaskCreate(led_task, "io_led", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "led task create failed");
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WS2812 status LED on GPIO%d (%d px)", LED_GPIO, LED_COUNT);
    return ESP_OK;
}

#else   /* !CONFIG_SOPHON_LED_WS2812 */

/* 开关关掉:没有灯,但状态码照算(status.led 还得给对)。 */
esp_err_t io_led_init(void)
{
    return ESP_OK;
}

#endif  /* CONFIG_SOPHON_LED_WS2812 */
