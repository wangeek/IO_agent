/*
 * io_led — 把设备状态码真正点亮到实体 LED(WS2812,可选)。详见 include/io_led.h。
 */
#include "io_led.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>

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
/* 一个像素 = 3 字节 = 24 bit,每 bit 一个 RMT symbol */
#define LED_WORDS_PER_PX    24
/* 末尾 +1 是复位符号。★ mem_block_symbols **必须是偶数**(IDF 的硬要求),
 *   所以 +2 而不是 +1。 */
#define LED_WORDS_MAX       (LED_COUNT * LED_WORDS_PER_PX + 2)

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
static rmt_encoder_handle_t s_enc;
static int s_last_state = -1;
static bool s_last_on;
static bool s_blink_phase;

/* ------------------------------------------------------------------ */
/* WS2812 编码器                                                        */
/* ------------------------------------------------------------------ */

/*
 * ★ IDF 5.5 的 RMT 换了架构 ★
 *   旧版直接把 `rmt_symbol_word_t` 数组交给 rmt_transmit();5.5 改成
 *   "encoder + 原始 payload":`rmt_transmit(chan, encoder, payload, bytes, cfg)`,
 *   由 encoder 的回调把原始数据翻译成 RMT symbol。
 *   内置的 bytes/simple encoder 表达不了 WS2812(0 位和 1 位的高/低电平宽度
 *   不一样),所以自己写回调 —— 这也是 IDF 5.5 没带 led_strip 组件的原因。
 *
 * payload = 原始 bit 流(每字节 MSB first),一个像素 3 字节 G,R,B。
 */
static size_t ws2812_encode_cb(const void *data, size_t data_size,
                                size_t symbols_written, size_t symbols_free,
                                rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;
    const uint8_t *bits = (const uint8_t *)data;
    const size_t total_bits = data_size * 8;
    size_t written = 0;

    while (symbols_written + written < total_bits) {
        if (written >= symbols_free) {
            break;              /* 空间不够,等 RMT 腾出空间再调 */
        }
        const size_t bi = symbols_written + written;
        const bool one = (bits[bi / 8] >> (7 - (bi % 8))) & 1;
        symbols[written].level0 = 1;
        symbols[written].duration0 = one ? LED_T1H : LED_T0H;
        symbols[written].level1 = 0;
        symbols[written].duration1 = one ? LED_T1L : LED_T0L;
        written++;
    }

    /* 末尾补复位符号:DIN 拉低 >50 µs,灯才 latch 这一帧 */
    if (symbols_written + written == total_bits && written < symbols_free) {
        symbols[written].level0 = 0;
        symbols[written].duration0 = LED_RESET_TICKS;
        symbols[written].level1 = 0;
        symbols[written].duration1 = 0;
        written++;
        *done = true;
    }
    return written;
}

static void ws2812_write_multi(const uint8_t (*grb)[3], int count)
{
    uint8_t bits[LED_COUNT * 3];

    /*
     * 同一串灯的所有像素必须**连续发完**:WS2812 逐像素 latch,中间断一下
     * (哪怕是另一个事务)就被当成复位 ⇒ 颜色对不齐。少发一颗 ⇒ 后面的灯珠
     * 收不到数据、保持旧值。
     */
    for (int i = 0; i < count * 3; i++) {
        bits[i] = grb[i / 3][i % 3];        /* G,R,B 顺序 */
    }

    rmt_transmit_config_t tx = { .loop_count = 0 };   /* 0 = 只发一次 */
    const esp_err_t err = rmt_transmit(s_chan, s_enc, bits, (size_t)count * 3, &tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
        return;
    }
    /* 等这一帧真的发完(否则下一帧的复位会被吞掉) */
    rmt_tx_wait_all_done(s_chan, 100);
}

static void show(int state, bool on)
{
    uint8_t grb[LED_COUNT][3] = {{0}};
    if (on) {
        for (int i = 0; i < LED_COUNT; i++) {
            memcpy(grb[i], s_color[state], sizeof(grb[i]));
        }
    }
    ws2812_write_multi(grb, LED_COUNT);
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
        .flags = { .invert_out = false, .with_dma = false, .init_level = 0 },
    };
    esp_err_t err = rmt_new_tx_channel(&tx, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        s_chan = NULL;
        return err;
    }
    /*
     * ★ 必须 rmt_enable() ★
     *   IDF 5.5 里 rmt_new_tx_channel() **不会**自动 enable 通道。
     *   不 enable 的话事务永远不开始 —— 表现是 rmt_tx_wait_all_done() 报
     *   "flush timeout"(第一次踩到就是这个,灯一点反应都没有)。
     */
    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return err;
    }

    rmt_simple_encoder_config_t enc = {
        .callback = ws2812_encode_cb,
        .arg = NULL,
        .min_chunk_size = 64,
    };
    err = rmt_new_simple_encoder(&enc, &s_enc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_simple_encoder failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return err;
    }

    if (xTaskCreate(led_task, "io_led", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "led task create failed");
        rmt_del_encoder(s_enc);
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WS2812 status LED on GPIO%d (%d px)", LED_GPIO, LED_COUNT);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 自检:让硬件把"几颗灯珠 / 什么字节序"演出来                            */
/* ------------------------------------------------------------------ */

static void led_test_task(void *arg)
{
    (void)arg;
    const int saved_state = s_last_state;
    const bool saved_on = s_last_on;

    /*
     * 循环跑(默认 30 轮 ≈ 2 分钟):自检是独立 task,UART 控制台照常收命令,
     * 所以"什么时候抬头看都在循环" —— 一次性 4 秒的窗口太容易错过。
     */
    for (int round = 0; round < 30; round++) {
        /* 阶段 1:逐颗红 —— 数一数有几颗(每颗亮 300 ms) */
        for (int px = 0; px < LED_COUNT; px++) {
            uint8_t grb[LED_COUNT][3] = {{0}};
            grb[px][1] = LED_BRIGHT;          /* 红 = G0 R=Bright B0 */
            ws2812_write_multi(grb, LED_COUNT);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        vTaskDelay(pdMS_TO_TICKS(400));

        /* 阶段 2:全体依次 红/绿/蓝/白 —— 若"红"显示成蓝,说明字节序反了 */
        static const uint8_t seq[4][3] = {
            {0, LED_BRIGHT, 0},                    /* 红 */
            {LED_BRIGHT, 0, 0},                    /* 绿 */
            {0, 0, LED_BRIGHT},                    /* 蓝 */
            {LED_BRIGHT, LED_BRIGHT, LED_BRIGHT},   /* 白 */
        };
        for (int i = 0; i < 4; i++) {
            uint8_t grb[LED_COUNT][3] = {{0}};
            for (int px = 0; px < LED_COUNT; px++) {
                memcpy(grb[px], seq[i], 3);
            }
            ws2812_write_multi(grb, LED_COUNT);
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }

    ESP_LOGI(TAG, "LED self-test done");
    s_last_state = saved_state;
    s_last_on = saved_on;
    vTaskDelete(NULL);
}

esp_err_t io_led_test(void)
{
    if (!s_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(led_test_task, "io_ledt", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else   /* !CONFIG_SOPHON_LED_WS2812 */

/* 开关关掉:没有灯,但状态码照算(status.led 还得给对)。 */
esp_err_t io_led_init(void)
{
    return ESP_OK;
}

esp_err_t io_led_test(void)
{
    return ESP_ERR_INVALID_STATE;    /* 没有灯可测 */
}

#endif  /* CONFIG_SOPHON_LED_WS2812 */
