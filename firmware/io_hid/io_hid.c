#include "io_hid.h"

#include <string.h>

#include <esp_log.h>
#include <esp_check.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "tusb.h"
#include "device/usbd_pvt.h"    /* usbd_edpt_busy/claim/release(私有头,仅用于诊断探针) */

static const char *TAG = "io_hid";

#define IO_HID_MAX_KEYS       6       /* 键盘报告最多 6 个同时按下 */
#define IO_HID_MOUSE_PAYLOAD  4       /* buttons + X + Y + Wheel */
/* 发报告时等端点空出来的上限(毫秒)。超时才报"发不出去"。 */
#define IO_HID_EP_WAIT_MS     200

/* 键盘当前按下的键(保持态,这样 press/release 语义才对) */
static uint8_t s_keys[IO_HID_MAX_KEYS];
static uint8_t s_key_count;
/* 鼠标当前按键位 */
static uint8_t s_buttons;

/* US 布局表自检结果:false = 表被改坏了,拒绝打字(宁可不打,也不能打错) */
static bool s_layout_ok;

esp_err_t io_hid_init(void)
{
    memset(s_keys, 0, sizeof(s_keys));
    s_key_count = 0;
    s_buttons = 0;
    s_layout_ok = io_hid_layout_selfcheck();
    if (!s_layout_ok) {
        ESP_LOGE(TAG, "US layout table self-check FAILED -- refusing to type text");
    }
    /* 注意:这里**不能**调 io_hid_ep_probe() —— USB 栈还没初始化,
       usbd 的 mutex 尚未创建,usbd_edpt_claim() 里的 TU_ASSERT(mutex) 会 abort。 */
    return ESP_OK;
}

bool io_hid_ready(void)
{
    return tud_hid_ready();
}

/*
 * 端点状态探针(诊断用)。
 *
 * **只能在 USB 栈初始化之后调用**(usbd 的 mutex 是在 usbd_initialize 时创建的,
 * 之前调用 usbd_edpt_claim() 会撞上 TU_ASSERT(mutex) 直接 abort)。
 *
 * usbd_edpt_busy() 只返回 BUSY 位;而 usbd_edpt_claim() 只有在端点状态**严格
 * 等于 0**(完全空闲)时才成功。所以两个结果组合起来能区分:
 *   busy=1 claim=0 -> 状态含 BUSY:上一次传输在飞但从未完成
 *   busy=0 claim=0 -> 状态 = CLAIMED:端点被认领后没释放
 *   claim=1         -> 端点空闲(claim 成功后立刻 release)
 */
void io_hid_ep_probe(const char *tag)
{
    const bool busy = usbd_edpt_busy(0, 0x81);
    const bool claimed = usbd_edpt_claim(0, 0x81);
    if (claimed) {
        usbd_edpt_release(0, 0x81);
    }
    ESP_LOGI(TAG, "ep81[%s]: busy=%d claim_ok=%d tud_ready=%d",
             tag, (int)busy, (int)claimed, (int)tud_ready());
}

bool io_hid_wait_ready(uint32_t timeout_ms)
{
    /*
     * tud_hid_ready() 含 usbd_edpt_busy() 项:端点正忙时它返回 false,
     * 但那不是"链路故障",只是"上一份报告还没被主机取走"。
     * 所以这里做**有上限的短轮询**等端点空出来,而不是直接报错。
     * 链路活性判据是 tud_ready()(见 io_usb.h),两者别混用。
     */
    for (uint32_t waited = 0; waited <= timeout_ms; waited++) {
        if (tud_hid_ready()) {
            return true;
        }
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, "HID endpoint still busy after %u ms (tud_ready=%d)",
             (unsigned)timeout_ms, (int)tud_ready());
    return false;
}

/* ------------------------------------------------------------------ */
/* 键盘                                                                 */
/* ------------------------------------------------------------------ */
static esp_err_t io_hid_send_keyboard_mod(uint8_t modifier)
{
    if (!io_hid_wait_ready(IO_HID_EP_WAIT_MS)) {
        return ESP_ERR_INVALID_STATE;
    }
    /* tud_hid_keyboard_report() 发的载荷是 modifier + reserved + keycode[6] */
    return tud_hid_keyboard_report(IO_HID_RID_KEYBOARD, modifier, s_keys)
                   ? ESP_OK
                   : ESP_FAIL;
}

static esp_err_t io_hid_send_keyboard(void)
{
    return io_hid_send_keyboard_mod(0);
}

esp_err_t io_hid_keyboard_press(uint8_t keycode)
{
    if (keycode == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_key_count >= IO_HID_MAX_KEYS) {
        ESP_LOGW(TAG, "too many simultaneous keys (%d), ignoring", IO_HID_MAX_KEYS);
        return ESP_ERR_INVALID_STATE;
    }
    for (uint8_t i = 0; i < s_key_count; i++) {
        if (s_keys[i] == keycode) {
            return ESP_OK;     /* 已经按着,幂等 */
        }
    }
    s_keys[s_key_count++] = keycode;
    return io_hid_send_keyboard();
}

esp_err_t io_hid_keyboard_release(void)
{
    memset(s_keys, 0, sizeof(s_keys));
    s_key_count = 0;
    return io_hid_send_keyboard();
}

esp_err_t io_hid_keyboard_tap(uint8_t keycode, uint16_t hold_ms)
{
    ESP_RETURN_ON_ERROR(io_hid_keyboard_press(keycode), TAG, "按下失败");
    if (hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }
    return io_hid_keyboard_release();
}

/* ------------------------------------------------------------------ */
/* US 键盘布局                                                          */
/* ------------------------------------------------------------------ */

#define IO_HID_MAP_SHIFT  IO_HID_MOD_LSHIFT

typedef struct {
    uint8_t mod;
    uint8_t key;
} io_hid_keymap_t;

/*
 * US 布局,ASCII 0x20..0x7E(95 个可打印字符),索引 = c - 0x20。
 * 每项 = {modifier, keycode}。同一对"符号/字母"的大小写**共享 keycode**,
 * 只差 modifier —— 这条不变量由 io_hid_layout_selfcheck() 验证。
 *
 * 数字行是 US 布局的"倒序":字符 '1' 在 '2' 左边,keycode('1')=0x1E 比
 * keycode('2')=0x1F 小;而 '0' 在 '9' 右边,keycode('0')=0x27。
 */
static const io_hid_keymap_t s_us_map[95] = {
    /* 0x20 space */ {0, 0x2C},
    /* 0x21 !     */ {IO_HID_MAP_SHIFT, 0x1E},
    /* 0x22 "     */ {IO_HID_MAP_SHIFT, 0x34},
    /* 0x23 #     */ {IO_HID_MAP_SHIFT, 0x20},
    /* 0x24 $     */ {IO_HID_MAP_SHIFT, 0x21},
    /* 0x25 %     */ {IO_HID_MAP_SHIFT, 0x22},
    /* 0x26 &     */ {IO_HID_MAP_SHIFT, 0x24},
    /* 0x27 '     */ {0, 0x34},
    /* 0x28 (     */ {IO_HID_MAP_SHIFT, 0x26},
    /* 0x29 )     */ {IO_HID_MAP_SHIFT, 0x27},
    /* 0x2A *     */ {IO_HID_MAP_SHIFT, 0x25},
    /* 0x2B +     */ {IO_HID_MAP_SHIFT, 0x2E},
    /* 0x2C ,     */ {0, 0x36},
    /* 0x2D -     */ {0, 0x2D},
    /* 0x2E .     */ {0, 0x37},
    /* 0x2F /     */ {0, 0x38},
    /* 0x30 0     */ {0, 0x27},
    /* 0x31 1     */ {0, 0x1E},
    /* 0x32 2     */ {0, 0x1F},
    /* 0x33 3     */ {0, 0x20},
    /* 0x34 4     */ {0, 0x21},
    /* 0x35 5     */ {0, 0x22},
    /* 0x36 6     */ {0, 0x23},
    /* 0x37 7     */ {0, 0x24},
    /* 0x38 8     */ {0, 0x25},
    /* 0x39 9     */ {0, 0x26},
    /* 0x3A :     */ {IO_HID_MAP_SHIFT, 0x33},
    /* 0x3B ;     */ {0, 0x33},
    /* 0x3C <     */ {IO_HID_MAP_SHIFT, 0x36},
    /* 0x3D =     */ {0, 0x2E},
    /* 0x3E >     */ {IO_HID_MAP_SHIFT, 0x37},
    /* 0x3F ?     */ {IO_HID_MAP_SHIFT, 0x38},
    /* 0x40 @     */ {IO_HID_MAP_SHIFT, 0x1F},
    /* 0x41 A     */ {IO_HID_MAP_SHIFT, 0x04},
    /* 0x42 B     */ {IO_HID_MAP_SHIFT, 0x05},
    /* 0x43 C     */ {IO_HID_MAP_SHIFT, 0x06},
    /* 0x44 D     */ {IO_HID_MAP_SHIFT, 0x07},
    /* 0x45 E     */ {IO_HID_MAP_SHIFT, 0x08},
    /* 0x46 F     */ {IO_HID_MAP_SHIFT, 0x09},
    /* 0x47 G     */ {IO_HID_MAP_SHIFT, 0x0A},
    /* 0x48 H     */ {IO_HID_MAP_SHIFT, 0x0B},
    /* 0x49 I     */ {IO_HID_MAP_SHIFT, 0x0C},
    /* 0x4A J     */ {IO_HID_MAP_SHIFT, 0x0D},
    /* 0x4B K     */ {IO_HID_MAP_SHIFT, 0x0E},
    /* 0x4C L     */ {IO_HID_MAP_SHIFT, 0x0F},
    /* 0x4D M     */ {IO_HID_MAP_SHIFT, 0x10},
    /* 0x4E N     */ {IO_HID_MAP_SHIFT, 0x11},
    /* 0x4F O     */ {IO_HID_MAP_SHIFT, 0x12},
    /* 0x50 P     */ {IO_HID_MAP_SHIFT, 0x13},
    /* 0x51 Q     */ {IO_HID_MAP_SHIFT, 0x14},
    /* 0x52 R     */ {IO_HID_MAP_SHIFT, 0x15},
    /* 0x53 S     */ {IO_HID_MAP_SHIFT, 0x16},
    /* 0x54 T     */ {IO_HID_MAP_SHIFT, 0x17},
    /* 0x55 U     */ {IO_HID_MAP_SHIFT, 0x18},
    /* 0x56 V     */ {IO_HID_MAP_SHIFT, 0x19},
    /* 0x57 W     */ {IO_HID_MAP_SHIFT, 0x1A},
    /* 0x58 X     */ {IO_HID_MAP_SHIFT, 0x1B},
    /* 0x59 Y     */ {IO_HID_MAP_SHIFT, 0x1C},
    /* 0x5A Z     */ {IO_HID_MAP_SHIFT, 0x1D},
    /* 0x5B [     */ {0, 0x2F},
    /* 0x5C \     */ {0, 0x31},
    /* 0x5D ]     */ {0, 0x30},
    /* 0x5E ^     */ {IO_HID_MAP_SHIFT, 0x23},
    /* 0x5F _     */ {IO_HID_MAP_SHIFT, 0x2D},
    /* 0x60 `     */ {0, 0x35},
    /* 0x61 a     */ {0, 0x04},
    /* 0x62 b     */ {0, 0x05},
    /* 0x63 c     */ {0, 0x06},
    /* 0x64 d     */ {0, 0x07},
    /* 0x65 e     */ {0, 0x08},
    /* 0x66 f     */ {0, 0x09},
    /* 0x67 g     */ {0, 0x0A},
    /* 0x68 h     */ {0, 0x0B},
    /* 0x69 i     */ {0, 0x0C},
    /* 0x6A j     */ {0, 0x0D},
    /* 0x6B k     */ {0, 0x0E},
    /* 0x6C l     */ {0, 0x0F},
    /* 0x6D m     */ {0, 0x10},
    /* 0x6E n     */ {0, 0x11},
    /* 0x6F o     */ {0, 0x12},
    /* 0x70 p     */ {0, 0x13},
    /* 0x71 q     */ {0, 0x14},
    /* 0x72 r     */ {0, 0x15},
    /* 0x73 s     */ {0, 0x16},
    /* 0x74 t     */ {0, 0x17},
    /* 0x75 u     */ {0, 0x18},
    /* 0x76 v     */ {0, 0x19},
    /* 0x77 w     */ {0, 0x1A},
    /* 0x78 x     */ {0, 0x1B},
    /* 0x79 y     */ {0, 0x1C},
    /* 0x7A z     */ {0, 0x1D},
    /* 0x7B {     */ {IO_HID_MAP_SHIFT, 0x2F},
    /* 0x7C |     */ {IO_HID_MAP_SHIFT, 0x31},
    /* 0x7D }     */ {IO_HID_MAP_SHIFT, 0x30},
    /* 0x7E ~     */ {IO_HID_MAP_SHIFT, 0x35},
};

bool io_hid_key_for_char(char c, uint8_t *mod, uint8_t *key)
{
    const uint8_t u = (uint8_t)c;
    if (u < 0x20 || u > 0x7E) {
        return false;
    }
    const io_hid_keymap_t *e = &s_us_map[u - 0x20];
    if (mod) {
        *mod = e->mod;
    }
    if (key) {
        *key = e->key;
    }
    return true;
}

/*
 * 命名键表:远程 act 按名字发键(修饰键本身也能当键用)。
 *
 * 关键点:微软拼音的"中/英文切换"只在**单独**按 Shift 时触发;
 * Shift+A 只是输出一个大写字母,不算切换。所以必须能指名发一个裸 Shift。
 */
static const struct {
    const char *name;
    uint8_t key;
} s_named_keys[] = {
    { "SHIFT",     IO_HID_KEY_LEFTSHIFT },
    { "CTRL",      IO_HID_KEY_LEFTCONTROL },
    { "ALT",       IO_HID_KEY_LEFTALT },
    { "WIN",       IO_HID_KEY_LEFTGUI },
    { "ENTER",     IO_HID_KEY_ENTER },
    { "ESC",       IO_HID_KEY_ESC },
    { "SPACE",     IO_HID_KEY_SPACE },
    { "TAB",       IO_HID_KEY_TAB },
    { "BACKSPACE", IO_HID_KEY_BACKSPACE },
    { "DELETE",    IO_HID_KEY_DELETE },
};

static bool ascii_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') { ca = (char)(ca - 'a' + 'A'); }
        if (cb >= 'a' && cb <= 'z') { cb = (char)(cb - 'a' + 'A'); }
        if (ca != cb) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

bool io_hid_key_for_named(const char *name, uint8_t *mod, uint8_t *key)
{
    if (!name || !name[0]) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_named_keys) / sizeof(s_named_keys[0]); i++) {
        if (ascii_ieq(name, s_named_keys[i].name)) {
            if (mod) { *mod = 0; }
            if (key) { *key = s_named_keys[i].key; }
            return true;
        }
    }
    return false;
}

bool io_hid_keycode_for_letter(char c, uint8_t *key)
{
    char lower = c;
    if (lower >= 'A' && lower <= 'Z') {
        lower = (char)(lower - 'A' + 'a');
    }
    if (lower < 'a' || lower > 'z') {
        return false;
    }
    if (key) {
        *key = (uint8_t)(0x04 + (lower - 'a'));
    }
    return true;
}

bool io_hid_layout_selfcheck(void)
{
    /* 1) 字母 a-z 必须连续映射到 0x04..0x1D */
    for (int i = 0; i < 26; i++) {
        if (s_us_map[('a' + i) - 0x20].key != (uint8_t)(0x04 + i)) {
            return false;
        }
        if (s_us_map[('A' + i) - 0x20].key != (uint8_t)(0x04 + i)) {
            return false;
        }
    }
    /* 2) 大写带 shift、小写不带 */
    for (int i = 0; i < 26; i++) {
        if ((s_us_map[('A' + i) - 0x20].mod & IO_HID_MAP_SHIFT) == 0) {
            return false;
        }
        if ((s_us_map[('a' + i) - 0x20].mod & IO_HID_MAP_SHIFT) != 0) {
            return false;
        }
    }
    /* 3) 数字行:US 布局的"倒序"keycode */
    for (int d = 0; d <= 9; d++) {
        const uint8_t expect = (d == 0) ? 0x27u : (uint8_t)(0x1E + d - 1);
        if (s_us_map[('0' + d) - 0x20].key != expect) {
            return false;
        }
    }
    /* 4) 已知的 shift/非shift 配对必须共享 keycode */
    static const struct { char shifted, plain; } pairs[] = {
        {'!', '1'}, {'@', '2'}, {'#', '3'}, {'$', '4'}, {'%', '5'},
        {'^', '6'}, {'&', '7'}, {'*', '8'}, {'(', '9'}, {')', '0'},
        {'_', '-'}, {'+', '='}, {'{', '['}, {'}', ']'}, {'|', '\\'},
        {':', ';'}, {'"', '\''}, {'<', ','}, {'>', '.'}, {'?', '/'},
        {'~', '`'},
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        const io_hid_keymap_t *a = &s_us_map[(uint8_t)pairs[i].shifted - 0x20];
        const io_hid_keymap_t *b = &s_us_map[(uint8_t)pairs[i].plain - 0x20];
        if (a->key != b->key) {
            return false;
        }
        if ((a->mod & IO_HID_MAP_SHIFT) == 0 || (b->mod & IO_HID_MAP_SHIFT) != 0) {
            return false;
        }
    }
    return true;
}

/* 按下"modifier + 单个键",保持 hold_ms,然后整体抬起回到基线。 */
static esp_err_t io_hid_tap_mod(uint8_t modifier, uint8_t keycode, uint16_t hold_ms)
{
    /* 先清基线:保证这次只发这一个键,不带任何残留状态 */
    memset(s_keys, 0, sizeof(s_keys));
    s_key_count = 0;
    if (keycode) {
        s_keys[s_key_count++] = keycode;
    }
    ESP_RETURN_ON_ERROR(io_hid_send_keyboard_mod(modifier), TAG, "按下失败");
    if (hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }
    memset(s_keys, 0, sizeof(s_keys));
    s_key_count = 0;
    return io_hid_send_keyboard_mod(0);
}

esp_err_t io_hid_hotkey(uint8_t modifier, uint8_t keycode, uint16_t hold_ms)
{
    return io_hid_tap_mod(modifier, keycode, hold_ms);
}

esp_err_t io_hid_hotkey_split(uint8_t modifier, uint8_t keycode, uint16_t hold_ms,
                              uint16_t gap_ms)
{
    /*
     * 分三步:① 只报修饰键 ② 报"修饰键 + 键" ③ 抬起。
     * ① 与 ② 必须是**两个独立的 HID 报告** —— 同一个报告里同时给出
     * 修饰键位和键码时,IME 的热键识别不触发(实测 Ctrl+Space 失效)。
     */
    memset(s_keys, 0, sizeof(s_keys));
    s_key_count = 0;
    ESP_RETURN_ON_ERROR(io_hid_send_keyboard_mod(modifier), TAG, "修饰键按下失败");
    if (gap_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(gap_ms));
    }

    if (keycode) {
        s_keys[s_key_count++] = keycode;
    }
    ESP_RETURN_ON_ERROR(io_hid_send_keyboard_mod(modifier), TAG, "按键按下失败");
    if (hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }

    memset(s_keys, 0, sizeof(s_keys));
    s_key_count = 0;
    ESP_RETURN_ON_ERROR(io_hid_send_keyboard_mod(modifier), TAG, "按键抬起失败");
    if (gap_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(gap_ms));
    }
    return io_hid_send_keyboard_mod(0);
}

esp_err_t io_hid_type_text(const char *text, uint16_t per_char_ms)
{
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_layout_ok) {
        /* 表被改坏了:打出来的会是错字(实测踩过:括号打错 -> cmd 语法错 ->
           watcher 起不来)。宁可不打,也不能打错。 */
        ESP_LOGE(TAG, "type_text refused: US layout table failed self-check");
        return ESP_ERR_INVALID_STATE;
    }
    for (const char *p = text; *p; p++) {
        uint8_t mod = 0;
        uint8_t key = 0;

        if (*p == '\n' || *p == '\r') {
            key = IO_HID_KEY_ENTER;
        } else {
            const uint8_t u = (uint8_t)*p;
            if (u < 0x20 || u > 0x7E) {
                ESP_LOGW(TAG, "unsupported char 0x%02X, skipped", (unsigned)u);
                continue;
            }
            mod = s_us_map[u - 0x20].mod;
            key = s_us_map[u - 0x20].key;
        }
        ESP_RETURN_ON_ERROR(io_hid_tap_mod(mod, key, per_char_ms), TAG, "打字失败");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 鼠标                                                                 */
/* ------------------------------------------------------------------ */
static esp_err_t io_hid_send_mouse(int8_t dx, int8_t dy, int8_t wheel)
{
    if (!tud_hid_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t payload[IO_HID_MOUSE_PAYLOAD] = {
        s_buttons,
        (uint8_t)dx,
        (uint8_t)dy,
        (uint8_t)wheel,
    };
    return tud_hid_report(IO_HID_RID_MOUSE, payload, sizeof(payload)) ? ESP_OK : ESP_FAIL;
}

esp_err_t io_hid_mouse_move(int16_t dx, int16_t dy)
{
    if (dx == 0 && dy == 0) {
        return ESP_OK;
    }

    /* 相对坐标是有符号 1 字节,范围 -127..127,超出就切片多次上报 */
    const int32_t adx = (dx < 0) ? -(int32_t)dx : (int32_t)dx;
    const int32_t ady = (dy < 0) ? -(int32_t)dy : (int32_t)dy;
    int32_t step = adx;
    if (ady > step) {
        step = ady;
    }
    const uint16_t chunks = (uint16_t)((step + 126) / 127);

    /*
     * 每一刀按"剩余量 / 剩余刀数"分配,余数留给最后一刀。
     * 直��用 dx/chunks 会丢余数:dx=300,dy=-1 时 chunks=3,
     * sy = -1/3 = 0 -> 纵向位移整个消失。
     */
    int32_t rx = dx;
    int32_t ry = dy;
    for (uint16_t i = 0; i < chunks; i++) {
        const int32_t left = (int32_t)(chunks - i);
        int32_t sx = rx / left;
        int32_t sy = ry / left;
        if (i + 1 == chunks) {
            sx = rx;      /* 最后一刀把余数全带走 */
            sy = ry;
        }
        if (sx > 127) {
            sx = 127;
        } else if (sx < -127) {
            sx = -127;
        }
        if (sy > 127) {
            sy = 127;
        } else if (sy < -127) {
            sy = -127;
        }
        rx -= sx;
        ry -= sy;
        ESP_RETURN_ON_ERROR(io_hid_send_mouse((int8_t)sx, (int8_t)sy, 0), TAG, "移动失败");
        /* 切片之间留一点间隔,让主机有机会处理,避免端点忙 */
        vTaskDelay(1);
    }
    return ESP_OK;
}

esp_err_t io_hid_mouse_scroll(int8_t dy)
{
    return io_hid_send_mouse(0, 0, dy);
}

esp_err_t io_hid_mouse_buttons(uint8_t buttons)
{
    /* 报告描述符里只声明 5 个按键 + 3 位填充,填充位必须送 0,
       否则主机可能把填充位当成第 6 个按键的按下沿。 */
    s_buttons = buttons & 0x1F;
    return io_hid_send_mouse(0, 0, 0);
}
