/*
 * io_log — 设备日志通过 WiFi 暴露给开发机(UDP server + 源 IP allowlist)。
 * 详见 include/io_log.h。
 */
#include "io_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_log_write.h>

#include <lwip/sockets.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *TAG = "io_log";

/* 环形缓冲:8 KB 的日志尾巴,突发时也不至于立刻丢光 */
#define IO_LOG_RING_SIZE   8192
/* 单条日志最长(超过就截断) */
#define IO_LOG_LINE_MAX    256
/*
 * 接收 task 的栈:lwIP 的 socket 调用链
 * (recvfrom → lwip_recvfrom → netconn_recv_udp_raw → netconn_recv_data
 *  → sys_arch_mbox_fetch)全在这个任务的栈上,所以比普通任务要宽。
 * 3072 不够 —— 栈一溢出就把相邻内存踩坏(真机踩到过 LoadProhibited)。
 */
#define IO_LOG_TASK_STACK  6144
#define IO_LOG_TASK_PRIO   3
/* 收包轮询周期(非阻塞 recvfrom 的等待间隔) */
#define IO_LOG_POLL_MS     20
/* allowlist 最多几个 IP */
#define IO_LOG_MAX_IPS     8
/* 帧头:magic(2) ver(1) flags(1) seq(2) len(2) */
#define IO_LOG_HDR_SIZE    8

static char s_ring[IO_LOG_RING_SIZE];
/*
 * s_head 是**单调递增**的写位置(不回绕),有效区 = [s_head - s_len, s_head)。
 *
 * ★ 别改回"head 回绕 + 满了就 head++"那套 ★
 *   逐字节写的时候,"满了就 head++"是对的(满时 head 恰好指向最老的数据)。
 *   一旦改成**整块 memcpy**(一次写 len 字节),这个不变量就不成立了:
 *   head 是"下一个写位置",不是"最老数据位置"。按老逻辑丢数据会算错
 *   s_len ⇒ **死循环 + 越界写** ⇒ 踩坏内存变成 LoadProhibited
 *   (2026-10-25 真机踩到:开机日志一填满 8 KB 就炸)。
 *   单调写指针 + "有效区长度"就没有这个坑。
 */
static uint32_t s_head;
static uint32_t s_len;      /* 有效字节数 */
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_hook_calls;
static volatile uint32_t s_rx_packets;
static volatile uint32_t s_rx_rejected;
static volatile uint32_t s_tx_packets;
static volatile uint32_t s_tx_bytes;
static volatile bool s_armed;   /* 钩子装了、授权表有了 */
static volatile bool s_opened;   /* socket 起来了、开始收包 */

/* 允许的源 IP(授权表) */
static struct in_addr s_allow[IO_LOG_MAX_IPS];
static uint32_t s_allow_n;

static int s_sock = -1;
static uint16_t s_seq;
static volatile bool s_stop;

/* 保留原来的 vprintf:日志的 UART 输出要走它,不能丢 */
static vprintf_like_t s_prev_vprintf;

/* ------------------------------------------------------------------ */
/* 环形缓冲                                                            */
/* ------------------------------------------------------------------ */

/*
 * 整块写入(不是逐字节):日志钩子每条日志都会被调一次,逐字节循环是
 * 白白烧 CPU。环形满了就丢最老的(日志更关心"最近的")。
 */
static void ring_push(const char *data, size_t len)
{
    if (!s_lock || !len) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (len > IO_LOG_RING_SIZE) {
        data += len - IO_LOG_RING_SIZE;    /* 只留尾巴 */
        len = IO_LOG_RING_SIZE;
    }
    if (s_len + len > IO_LOG_RING_SIZE) {
        /* 丢最老的:有效区起点前进,写指针不动 */
        s_len -= (s_len + len - IO_LOG_RING_SIZE);
    }
    const uint32_t pos = s_head % IO_LOG_RING_SIZE;
    uint32_t first = IO_LOG_RING_SIZE - pos;
    if (first > len) {
        first = len;
    }
    memcpy(s_ring + pos, data, first);
    if (len > first) {
        memcpy(s_ring, data + first, len - first);
    }
    s_head += len;
    s_len += len;
    xSemaphoreGive(s_lock);
}

/*
 * 取出一段**完整行**(到最后一个 '\n' 为止)。
 * 半行留在缓冲里等下一段 —— 日志的一行可能被拆成好几次 vprintf 调用
 * (前缀 / 正文 / 换行 各一次),所以必须自己拼行。
 *
 * 只取**完整行**是刻意的:接收端按字节流看,半行会让它把一行劈成两条。
 */
static size_t ring_take(char *out, size_t max)
{
    if (!s_lock) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    const uint32_t start = s_head - s_len;      /* 有效区起点 */
    /* 找 max 以内最后一个换行 —— 保证不把一行劈成两半 */
    int last_nl = -1;
    size_t limit = (s_len < max) ? s_len : max;
    for (size_t i = 0; i < limit; i++) {
        if (s_ring[(start + i) % IO_LOG_RING_SIZE] == '\n') {
            last_nl = (int)i;
        }
    }
    if (last_nl < 0) {
        xSemaphoreGive(s_lock);
        return 0;               /* 还没有完整行 */
    }
    const size_t cut = (size_t)last_nl + 1;
    const uint32_t pos = start % IO_LOG_RING_SIZE;
    size_t first = IO_LOG_RING_SIZE - pos;
    if (first > cut) {
        first = cut;
    }
    memcpy(out, s_ring + pos, first);
    if (cut > first) {
        memcpy(out + first, s_ring, cut - first);
    }
    /* 丢掉已取走的部分:有效区长度减掉,起点自然前移 —— head 不用动 */
    s_len -= cut;
    xSemaphoreGive(s_lock);
    return cut;
}

/* ------------------------------------------------------------------ */
/* vprintf 钩子                                                        */
/* ------------------------------------------------------------------ */

/*
 * IDF 的日志前缀(level/时间戳/tag)与正文、换行是**分三次** vprintf 调用,
 * 所以这里只管"原样搬运字节",拼行交给 ring_take。
 */
static int log_net_vprintf(const char *fmt, va_list args)
{
    s_hook_calls++;
    char line[IO_LOG_LINE_MAX];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        ring_push(line, len);
    }
    /* UART 那一路照旧 —— 串口没了也只是"没人看",行为不变 */
    if (s_prev_vprintf) {
        return s_prev_vprintf(fmt, args);
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 授权:源 IP allowlist                                                */
/* ------------------------------------------------------------------ */

/*
 * 解析 "ip1,ip2,ip3"。解析不出来 / 一个都没有 ⇒ 返回 false(日志不开)。
 */
static bool parse_allowlist(const char *csv)
{
    s_allow_n = 0;
    if (!csv || !csv[0]) {
        return false;
    }
    char tmp[IO_LOG_IPS_MAX];
    size_t n = strlen(csv);
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, csv, n);
    tmp[n] = '\0';

    char *p = tmp;
    while (p && *p) {
        char *comma = strchr(p, ',');
        if (comma) {
            *comma = '\0';
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t len = strlen(p);
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
            p[--len] = '\0';
        }
        if (p[0]) {
            if (s_allow_n < IO_LOG_MAX_IPS) {
                struct in_addr a;
                if (inet_pton(AF_INET, p, &a) == 1) {
                    s_allow[s_allow_n++] = a;
                } else {
                    ESP_LOGW(TAG, "allowlist: bad ip \"%s\" -- ignored", p);
                }
            } else {
                ESP_LOGW(TAG, "allowlist: more than %d ips -- extra ignored", IO_LOG_MAX_IPS);
            }
        }
        p = comma ? comma + 1 : NULL;
    }
    return s_allow_n > 0;
}

/* 源 IP 在不在授权表里 —— 不在就直接丢,连回包都不给 */
static bool ip_allowed(const struct in_addr *src)
{
    for (uint32_t i = 0; i < s_allow_n; i++) {
        if (s_allow[i].s_addr == src->s_addr) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* UDP server                                                          */
/* ------------------------------------------------------------------ */

/*
 * 一问一答:开发机发一个包过来,设备把环里的日志回给**请求的源地址**。
 * 回包走的是同一条会话(开发机先发的),所以 Windows 防火墙放行回程流量
 * ⇒ 开发机不用加任何规则。
 *
 * ★ 收包必须**非阻塞** ★
 *   阻塞版 recvfrom 会一路走到 lwIP 的
 *   `netconn_recv_data → sys_arch_mbox_fetch → xQueueReceive(portMAX_DELAY)`,
 *   而 `xQueueReceive` 进的是 **FreeRTOS 临界区**(Xtensa 上 = 关中断 +
 *   抢 portMux 自旋锁)。一旦**另一个核**正持有 portMux(比如 Core 1 在
 *   idle hook 里做 RF 校准,要读 eFuse ⇒ flash 操作),Core 0 就
 *   **关着中断无限期等锁** ⇒ WiFi 中断没人处理 ⇒
 *   `Guru Meditation: Interrupt wdt timeout on CPU0`。
 *   实测就是这个组合炸的(回溯: recvfrom → sys_arch_mbox_fetch →
 *   xQueueReceive → vPortEnterCritical → spinlock_acquire)。
 *   ⇒ 改成 MSG_DONTWAIT: 没包就走 `sys_arch_mbox_tryfetch`
 *   (临界区只瞬时进出),有包才处理。代价是 20 ms 一次的轮询,完全可接受。
 */
static void io_log_serve(void *arg)
{
    /*
     * socket 句柄当参数传进来:**本 task 只认自己那个 socket**。
     * (用全局 s_sock 的话,stop 之后马上 start 会撞车:老 task 退出时
     *  会把新 task 的 socket 关掉。)
     */
    const int sock = (int)(intptr_t)arg;
    uint8_t req[64];
    char payload[IO_LOG_FRAME_MAX];
    uint8_t frame[IO_LOG_HDR_SIZE + IO_LOG_FRAME_MAX];
    bool reported = false;

    for (;;) {
        if (s_stop) {
            break;
        }
        /*
         * 一次把队列里的包收完:非阻塞 recvfrom 一个包就返回 EWOULDBLOCK,
         * 如果只收一个就睡 20 ms,lwIP 的收包队列(深度很小)会被后来的包
         * 撑爆 —— 实测 30 个请求只到了 3 个。循环收干,应答才跟得上。
         */
        bool handled = false;
        for (int i = 0; i < 16; i++) {
            struct sockaddr_in src;
            socklen_t sl = sizeof(src);
            const int n = recvfrom(sock, req, sizeof(req), MSG_DONTWAIT,
                                   (struct sockaddr *)&src, &sl);
            if (n < 0) {
                break;              /* 队列空了 */
            }
            handled = true;
            s_rx_packets++;

            /* ★ 授权检查 ★ */
            if (!ip_allowed(&src.sin_addr)) {
                s_rx_rejected++;
                continue;           /* 不在白名单:直接丢,不回包 */
            }

            size_t got = 0;
            while (got < sizeof(payload)) {
                const size_t k = ring_take(payload + got, sizeof(payload) - got);
                if (k == 0) {
                    break;
                }
                got += k;
            }

            const uint16_t seq = s_seq++;
            frame[0] = 'S';
            frame[1] = 'L';
            frame[2] = 1;                        /* ver */
            frame[3] = 0;                        /* flags */
            frame[4] = (uint8_t)(seq >> 8);
            frame[5] = (uint8_t)seq;
            frame[6] = (uint8_t)(got >> 8);
            frame[7] = (uint8_t)got;
            if (got) {
                memcpy(frame + IO_LOG_HDR_SIZE, payload, got);
            }
            const int sent = sendto(sock, frame, IO_LOG_HDR_SIZE + got, 0,
                                    (struct sockaddr *)&src, sizeof(src));
            if (sent > 0) {
                s_tx_packets++;
                s_tx_bytes += (uint32_t)sent;
                if (!reported) {
                    /* 报一次栈水位:lwIP socket 调用链吃栈,水位太低要加栈 */
                    reported = true;
                    ESP_LOGI(TAG, "log task stack headroom: %u bytes",
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                }
            }
        }
        if (!handled) {
            vTaskDelay(pdMS_TO_TICKS(IO_LOG_POLL_MS));
        }
    }

    /* 收尾:task 自己关 socket(别的 task 可能在 sendto 中途) */
    close(sock);
    if (s_sock == sock) {
        s_sock = -1;
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 启动                                                                */
/* ------------------------------------------------------------------ */

esp_err_t io_log_net_start(const char *ips_csv)
{
    /*
     * 没配 allowlist = 不开日志。**连钩子都不装**,
     * 所以量产固件的日志面和开销都是零。
     */
    if (!parse_allowlist(ips_csv)) {
        ESP_LOGI(TAG, "no log allowlist configured -- network log stays off");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_armed) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    /*
     * 第一步:装钩子、开始缓冲。
     * **先于 socket 装**,是为了把开机日志也攒进去 —— 等 socket 起来
     * 时,第一次请求就能把整段开机日志补回来。
     */
    s_prev_vprintf = esp_log_set_vprintf(log_net_vprintf);
    s_armed = true;
    ESP_LOGI(TAG, "network log armed (allowlist: %u ip(s), udp port %d)",
             s_allow_n, IO_LOG_PORT);
    return ESP_OK;
}

esp_err_t io_log_net_open(void)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_opened) {
        return ESP_OK;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "udp socket failed");
        return ESP_FAIL;
    }
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(IO_LOG_PORT);
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "udp bind %d failed", IO_LOG_PORT);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    s_stop = false;
    if (xTaskCreate(io_log_serve, "io_log", IO_LOG_TASK_STACK,
                    (void *)(intptr_t)s_sock, IO_LOG_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "log task create failed");
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    s_opened = true;
    ESP_LOGI(TAG, "network log on udp://0.0.0.0:%d, %u allowed source ip(s)",
             IO_LOG_PORT, s_allow_n);
    return ESP_OK;
}

esp_err_t io_log_stop(void)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "network log stopping");
    s_armed = false;
    s_opened = false;
    s_allow_n = 0;
    /* 撤钩子:别再往环里塞(环里的内容留着,下次 start 还能补发) */
    if (s_prev_vprintf) {
        esp_log_set_vprintf(s_prev_vprintf);
        s_prev_vprintf = NULL;
    }
    /* 唤醒卡在 recvfrom 的那个 task,它自己会关 socket 退出 */
    s_stop = true;
    if (s_sock >= 0) {
        shutdown(s_sock, SHUT_RDWR);
    }
    /*
     * 等它真的退出(socket 被关掉)再返回 —— 否则紧接着 start() 会
     * 撞上 EADDRINUSE(端口还没释放)。
     */
    for (int i = 0; i < 100 && s_sock >= 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

uint16_t io_log_port(void)
{
    return IO_LOG_PORT;
}

bool io_log_active(void)
{
    return s_armed;
}

bool io_log_opened(void)
{
    return s_opened;
}

uint32_t io_log_allow_count(void)
{
    return s_allow_n;
}

uint32_t io_log_rx_packets(void)
{
    return s_rx_packets;
}

uint32_t io_log_rx_rejected(void)
{
    return s_rx_rejected;
}

uint32_t io_log_tx_packets(void)
{
    return s_tx_packets;
}

uint32_t io_log_tx_bytes(void)
{
    return s_tx_bytes;
}

uint32_t io_log_hook_calls(void)
{
    return s_hook_calls;
}
