/*
 * io_usb — USB 复合设备栈(HID 键鼠 + MSC 虚拟 U 盘)
 *
 * 拓扑(方案 A′,M1 冻结):
 *   接口 0 = HID   :单接口复合报告描述符,Report ID 1=键盘 / 2=鼠标
 *   接口 1 = MSC   :块设备,介质是 io_disk 的 PSRAM RAM disk
 *   富余端点保持空置,留给后续 vendor bulk(M2 再加)
 *
 * 端点分配:
 *   EP0        控制
 *   0x81       HID IN  (interrupt, 8 B)
 *   0x02       MSC OUT (bulk, 64 B)   主机 → 设备(唯一合法数据入口)
 *   0x82       MSC IN  (bulk, 64 B)   设备 → 主机(只读回读结果)
 *
 * 关键约束:
 *   - 设备端零 FAT 逻辑。MSC 只吐块,FAT 由 Windows 自己维护,
 *     镜像的 FAT 正确性由 tools/mkfatimg.py 保证。
 *   - esp_tinyusb 内置的 MSC 层在这里**不参与编译**(CONFIG_TINYUSB_MSC_ENABLED=n),
 *     所以本文件自己实现 TinyUSB 强制的 5 个 MSC 回调。
 *     背景见 components/README-vendor-patches.md。
 */
#include "io_usb.h"

#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_check.h>

#include "tinyusb.h"   /* esp_tinyusb: tinyusb_config_t / tinyusb_driver_install */
#include "tusb.h"      /* TinyUSB: tusb_desc_device_t / TUD_* 描述符宏 */
#include "io_disk.h"
#include "io_hid.h"     /* Report ID 常量 */

static const char *TAG = "io_usb";

/* ------------------------------------------------------------------ */
/* 接口 / 端点                                                          */
/* ------------------------------------------------------------------ */
#define IO_USB_ITF_HID          0
#define IO_USB_ITF_MSC          1
#define IO_USB_ITF_COUNT        2

#define IO_USB_EP_HID_IN        0x81
#define IO_USB_EP_MSC_OUT       0x02
#define IO_USB_EP_MSC_IN        0x82

/*
 * HID IN 端点大小。
 *
 * **必须是 16,不能是 8**:复合报告描述符下,键盘报告是 **9 字节**
 * (Report ID 1 + modifier 1 + reserved 1 + 6 个 keycode),鼠标是 5 字节。
 * interrupt 端点的 wMaxPacketSize 必须 >= 报告长度 —— 8 字节端点装不下
 * 9 字节的键盘报告,主机侧的 HID 栈不会完成这次传输,设备端端点从此一直 busy。
 * 实测症状:第一次 press 报告"发出去"了,之后每一份 release 都发不出去
 * (wait_ready 一直超时),表现为"按键毫无反应"。
 * TinyUSB 官方的 usb_cv 示例(同样是复合键鼠)用的就是 16。
 */
#define IO_USB_EP_HID_SIZE      64
#define IO_USB_EP_MSC_SIZE      64
/* HID 轮询间隔(ms)。官方 TinyUSB 示例用 5~10ms;1ms 太激进,
   主机的 HID 栈可能因此把集合配错(HidUsb 启动失败,实测 ProblemCode=10)。 */
#define IO_USB_HID_INTERVAL_MS  10

/* ------------------------------------------------------------------ */
/* 字符串描述符索引                                                     */
/* ------------------------------------------------------------------ */
#define IO_USB_STR_LANGID       0
#define IO_USB_STR_MANUF        1
#define IO_USB_STR_PRODUCT      2
#define IO_USB_STR_SERIAL       3
#define IO_USB_STR_HID          4
#define IO_USB_STR_MSC          5
#define IO_USB_STR_COUNT        6

/* Espressif VID + 自定 PID(M1 占位,量产需申请独立 VID/PID) */
#define IO_USB_VID              0x303A
#define IO_USB_PID              0x8001

/* 序列号后缀:改动它 = 换一个"新设备"身份,强制 Windows 重新解析描述符。
 * 每次**改动 HID 报告描述符**都要 bump 这个值,否则 Windows 会复用旧实例、
 * 从不重新解析,我们的修改看起来"没生效"。 */
#define IO_USB_SERIAL_SUFFIX    "-M3"

static const char s_langid_bytes[2] = {0x09, 0x04};   /* LANGID 0x0409 (US) */

/* 序列号在 io_usb_init() 里按芯片 MAC 填充,必须是静态存储(指针要长期有效) */
static char s_serial_str[24];

static const char *s_str_desc[IO_USB_STR_COUNT] = {
    s_langid_bytes,          /* 0 */
    "IO_Agent",              /* 1 Manufacturer */
    "IO_Agent Composite",    /* 2 Product */
    s_serial_str,            /* 3 Serial */
    "IO_Agent HID",          /* 4 HID interface */
    "IO_Agent Disk",         /* 5 MSC interface */
};

static const tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* 复合设备:类码交给各接口自己声明(没有 IAD,所以用 0) */
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = IO_USB_VID,
    .idProduct = IO_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = IO_USB_STR_MANUF,
    .iProduct = IO_USB_STR_PRODUCT,
    .iSerialNumber = IO_USB_STR_SERIAL,
    .bNumConfigurations = 0x01,
};

/* ------------------------------------------------------------------ */
/* HID 报告描述符:键盘(Report ID 1)+ 鼠标(Report ID 2)                */
/*                                                                      */
/* 载荷长度:                                                            */
/*   键盘 ID1 = modifier(1) + reserved(1) + keycode[6] = 8 B           */
/*   鼠标 ID2 = buttons(1) + X(1) + Y(1) + Wheel(1)      = 4 B         */
/* TinyUSB 的 tud_hid_report() 会自动在载荷前补 1 字节 Report ID,      */
/* 所以总长分别是 9 B 和 5 B,与下面的描述符长度严格一致。               */
/* ------------------------------------------------------------------ */
static const uint8_t s_hid_report_desc[] = {
    /* ---------------- Keyboard (Report ID 1) ---------------- */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x01,        /*   Report ID (1) */
    0x05, 0x07,        /*   Usage Page (Keyboard) */
    0x19, 0xE0,        /*   Usage Minimum (224) */
    0x29, 0xE7,        /*   Usage Maximum (231) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) - 修饰键 */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x08,        /*   Report Size (8) */
    0x81, 0x03,        /*   Input (Constant, Variable) - 保留字节 */
    0x95, 0x06,        /*   Report Count (6) */
    0x75, 0x08,        /*   Report Size (8) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0xE7,        /*   Logical Maximum (231) —— 必须覆盖 0xE1 左 Shift */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0x00,        /*   Usage Minimum (0) */
    0x29, 0xE7,        /*   Usage Maximum (231) */
    0x81, 0x00,        /*   Input (Data, Array) - 6 个键位 */
    /*
     * ★ 这里**故意没有** Output Report(键盘 LED 回写)★★
     *
     * 加上它之后,Windows 的 HidUsb 启动失败("USB Input Device" 节点
     * ProblemCode=10),整个 HID 接口不可用 —— 而 M1 时代(没有 Output Report)
     * 枚举是正常的。加之前必须先把 SET_REPORT 的应答路径在 Windows 上验通,
     * 否则加它等于把整条 HID 链路搞挂。
     * (它也是"设备插到 U 盘上会被复制"的同一根因:Windows 走 HID 栈配这个
     *  接口失败。)
     * 规范上 Boot Keyboard 应该有 Output Report(LED 回写),所以这是**待补**项,
     * 不是永久放弃。
     */
    0xC0,              /* End Collection */

    /* ---------------- Mouse (Report ID 2) ---------------- */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x02,        /*   Report ID (2) */
    0x09, 0x01,        /*   Usage (Pointer) */
    0xA1, 0x00,        /*   Collection (Physical) */
    0x05, 0x09,        /*     Usage Page (Button) */
    0x19, 0x01,        /*     Usage Minimum (1) */
    0x29, 0x05,        /*     Usage Maximum (5) */
    0x15, 0x00,        /*     Logical Minimum (0) */
    0x25, 0x01,        /*     Logical Maximum (1) */
    0x95, 0x05,        /*     Report Count (5) */
    0x75, 0x01,        /*     Report Size (1) */
    0x81, 0x02,        /*     Input (Data, Variable, Absolute) - 5 个按键 */
    0x95, 0x01,        /*     Report Count (1) */
    0x75, 0x03,        /*     Report Size (3) */
    0x81, 0x03,        /*     Input (Constant, Variable) - 填充位 */
    0x05, 0x01,        /*     Usage Page (Generic Desktop) */
    0x09, 0x30,        /*     Usage (X) */
    0x09, 0x31,        /*     Usage (Y) */
    0x09, 0x38,        /*     Usage (Wheel) */
    0x15, 0x81,        /*     Logical Minimum (-127) */
    0x25, 0x7F,        /*     Logical Maximum (127) */
    0x75, 0x08,        /*     Report Size (8) */
    0x95, 0x03,        /*     Report Count (3) */
    0x81, 0x06,        /*     Input (Data, Variable, Relative) */
    0xC0,              /*   End Collection */
    0xC0,              /* End Collection */
};

#define IO_USB_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, IO_USB_ITF_COUNT, 0, IO_USB_CONFIG_LEN, 0, 250),
    TUD_HID_DESCRIPTOR(IO_USB_ITF_HID, IO_USB_STR_HID, 0, sizeof(s_hid_report_desc),
                       IO_USB_EP_HID_IN, IO_USB_EP_HID_SIZE, IO_USB_HID_INTERVAL_MS),
    TUD_MSC_DESCRIPTOR(IO_USB_ITF_MSC, IO_USB_STR_MSC, IO_USB_EP_MSC_OUT, IO_USB_EP_MSC_IN,
                       IO_USB_EP_MSC_SIZE),
};

/* wTotalLength 必须等于配置描述符的真实长度,否则 Windows 枚举会失败。
   TUD_*_DESC_LEN 宏来自 TinyUSB,一旦升级版本导致长度变化,这里会直接编译报错。 */
_Static_assert(sizeof(s_config_desc) == IO_USB_CONFIG_LEN,
               "配置描述符实际长度与 IO_USB_CONFIG_LEN 不一致");
_Static_assert(sizeof(s_hid_report_desc) <= 0xFFFF,
               "HID 报告描述符过长");

/* ------------------------------------------------------------------ */
/* 运行时状态                                                           */
/* ------------------------------------------------------------------ */
static volatile bool s_host_configured;

/* ------------------------------------------------------------------ */
/* HID 回调(TinyUSB 只调用不定义,必须由 App 提供)                     */
/* ------------------------------------------------------------------ */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
{
    /*
     * 主机枚举 HID 设备时会 GET_REPORT 读"当前输入状态"。
     *
     * **必须返回有效内容**:返回 0 字节 = TinyUSB STALL 这个控制请求,
     * 主机认为设备不响应,随后会 SET_FEATURE(ENDPOINT_HALT) 把 IN 端点
     * halt 掉 —— 端点状态变成 STALLED|BUSY,从此再也不能发报告,
     * 表现为"枚举出键盘设备但按键毫无反应"。
     * (实测踩过:探针显示 busy=1 claim_ok=0,launcher 永远卡在等端点就绪。)
     *
     * TinyUSB 会自动在响应前补上 Report ID 字节,所以这里只返回载荷本体。
     * 当前状态就是"什么都没有按"= 全 0。
     */
    (void)instance;
    if (report_type != HID_REPORT_TYPE_INPUT) {
        return 0;   /* 没有 Output 报告 */
    }
    if (report_id == IO_HID_RID_MOUSE) {
        if (reqlen < 4u) {
            return 0;
        }
        memset(buffer, 0, 4);    /* buttons + X + Y + Wheel */
        return 4;
    }
    /* 键盘(Report ID 1),以及 report_id == 0 的"默认报告"请求 */
    if (reqlen < 8u) {
        return 0;
    }
    memset(buffer, 0, 8);        /* modifier + reserved + 6 keycode */
    return 8;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
    /* 无输出报告项,主机不会真的发这个请求;真发也忽略 */
}

/* ------------------------------------------------------------------ */
/* MSC 回调(TinyUSB 强制的 5 个,esp_tinyusb 内置层已被排除出编译)      */
/* ------------------------------------------------------------------ */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    (void)lun;
    if (io_disk_read_at(lba, offset, buffer, bufsize) != ESP_OK) {
        ESP_LOGE(TAG, "read failed lba=%u off=%u len=%u", lba, offset, bufsize);
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    if (io_disk_write_at(lba, offset, buffer, bufsize) != ESP_OK) {
        ESP_LOGE(TAG, "write failed lba=%u off=%u len=%u", lba, offset, bufsize);
        return -1;
    }
    return (int32_t)bufsize;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = io_disk_sector_count();
    *block_size = IO_DISK_SECTOR_SIZE;
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return io_disk_sector_count() > 0;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    /* 只支持标准 SCSI 命令;厂商自定义命令一律回不支持 */
    (void)lun;
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    return -1;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4])
{
    (void)lun;
    memset(vendor_id, ' ', 8);
    memset(product_id, ' ', 16);
    memset(product_rev, ' ', 4);
    memcpy(vendor_id, "IOAGENT", 7);
    memcpy(product_id, "IO_Agent Disk", 13);
    memcpy(product_rev, "1.0", 3);
}

uint8_t tud_msc_get_maxlun_cb(void)
{
    return 1;   /* 1 个 LUN */
}

/* ------------------------------------------------------------------ */
/* 设备事件                                                             */
/* ------------------------------------------------------------------ */
static void io_usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_host_configured = true;
        ESP_LOGI(TAG, "host enumerated and configured (rhport=%d)", event->rhport);
        break;
    case TINYUSB_EVENT_DETACHED:
        s_host_configured = false;
        ESP_LOGI(TAG, "host disconnected (rhport=%d)", event->rhport);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 初始化                                                               */
/* ------------------------------------------------------------------ */
esp_err_t io_usb_init(void)
{
    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "读 MAC 失败");
    /*
     * 序列号带一个可改的后缀:Windows 会按 "VID/PID/序列号" 识别设备实例,
     * 序列号一变就会**新建**实例并重新解析配置描述符 —— 否则它会一直复用
     * 那个启动失败的旧实例(HidUsb ProblemCode=10),我们改了描述符也不生效。
     */
    snprintf(s_serial_str, sizeof(s_serial_str),
             "%02X%02X%02X%02X%02X%02X%s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             IO_USB_SERIAL_SUFFIX);

    const tinyusb_config_t cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup = false,      /* 由 esp_tinyusb 配置内部 FS PHY */
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = 4096,
            .priority = 5,
            .xCoreID = 0,
        },
        .descriptor = {
            .device = &s_dev_desc,
            .qualifier = NULL,
            .string = s_str_desc,
            .string_count = IO_USB_STR_COUNT,
            .full_speed_config = s_config_desc,
            .high_speed_config = NULL,
        },
        .pm_lock_enable = false,
        .event_cb = io_usb_event_cb,
        .event_arg = NULL,
    };

    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG, "tinyusb_driver_install failed");

    ESP_LOGI(TAG, "USB composite device started: VID=%04X PID=%04X, interfaces %d (HID+MSC), endpoints "
                  "HID_IN=0x%02X MSC_OUT=0x%02X MSC_IN=0x%02X",
             IO_USB_VID, IO_USB_PID, IO_USB_ITF_COUNT,
             IO_USB_EP_HID_IN, IO_USB_EP_MSC_OUT, IO_USB_EP_MSC_IN);
    ESP_LOGI(TAG, "HID report descriptor %u bytes, config descriptor %u bytes",
             (unsigned)sizeof(s_hid_report_desc), (unsigned)sizeof(s_config_desc));
    return ESP_OK;
}

bool io_usb_host_configured(void)
{
    return s_host_configured;
}

/* ------------------------------------------------------------------ */
/* 链路活性                                                            */
/* ------------------------------------------------------------------ */
/*
 * tud_ready() = tud_mounted() && !tud_suspended()
 *
 * ESP32-S3 的 USB 是 Synopsys DWC2 内核,TinyUSB 的 DWC2 device 模式没有实现
 * 设备断开检测(dcd_dwc2.c 里 "GINTSTS_DISCINT for disconnect detection" 仍是
 * TODO),所以物理拔线只会让 DWC2 上报 SUSPEND,永远不会派发
 * TINYUSB_EVENT_DETACHED。因此"主机还在不在"必须看总线状态,不能看事件。
 *
 * 详见 include/io_usb.h 的注释。
 */
bool io_usb_link_alive(void)
{
    return tud_ready();
}

bool io_usb_mounted(void)
{
    return tud_mounted();
}

bool io_usb_suspended(void)
{
    return tud_suspended();
}
