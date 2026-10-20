# firmware/ — ESP32-S3 固件

ESP-IDF 工程(用 `idf.py create-project` 生成,**不要手写骨架**)。

```bash
idf.py create-project -p ../.. firmware   # 在 IO_agent 根目录执行
```

## 模块划分(来自 `docs/implementation-plan.md` §一)

| 目录 | 职责 |
|---|---|
| `main/` | 应用入口、状态机、bootstrap 流程(Stage -1/0/1/2) |
| `usb/` | TinyUSB 描述符:复合设备 **HID 键盘/鼠标 + MSC**,自写不用 Arduino 便利库 |
| `disk/` | PSRAM 内 FAT16 镜像(4-6MB 设计值)+ MSC 后端;预置脚本镜像 |
| `hid/` | 注入序列执行器:本地定时执行,<10ms/步;`focus_reset` 宏(IME 归零→Win+M→Win+R) |
| `net/` | WiFi、mDNS 广播、TLS-PSK 服务端、HTTP 服务 |
| `cfg/` | NVS 配置与设备凭据(WiFi 凭据 / PSK / watcher 拉起策略) |
| `capability/` | 能力注册表:编译期开关 + 运行时查询,状态机只查"我现在有什么" |

## 不可绕的硬件约束(踩坑前先读)

- **端点预算**:S2/S3 共 7 个端点(含 EP0)、最多 5 个 IN(含 EP0)→ 6 个可配置。
  HID(2)+MSC(2)=4,剩 2。**CDC-ACM 需要 3 个,塞不下**。
- **PHY 共享**:USB-OTG 与 USB-Serial-JTAG 共用 PHY;启用 HID 后原生串口调试口消失 → 日志走 UART0。
- **flash encryption / secure boot 会禁用 ROM 里的 USB-OTG 栈** → 本项目 USB 口即产品主接口,
  量产固件必须支持 **WiFi OTA**;开发期不开加密。
- **FAT16 BPB 正确性**:RAM disk 若 BPB 参数不对,Windows 会提示"需要格式化"甚至不认卷(M1 头号坑)。
- **ESP32 不能向主机发起数据**:USB 主从 + FAT 缓存;所有输入必须由主机主动写盘。

## 端点与双核(别踩)

- **端点预算**:7 个(含 EP0)、6 可配置、IN 最多 5(含 EP0)。本项目走 **A′ 方案**:
  单 HID 接口 + 复合报告描述符(Report ID 区分键鼠)+ 不实现 HID OUT(LED 已退出主路径)
  → 占用 4 个,富余 2 个,留给 **vendor bulk 接口**当产线/调试口。
  详见 `docs/hardware-selection.md` §三。
- **双核分工**:**core 0 专职 USB/TinyUSB + HID 注入**,**core 1 跑 WiFi + TLS + HTTP/MCP**。
  注入时序要求 <10ms/步,不能让 TLS 传输(300KB ≈ 20-60ms CPU)抢到同一核。
- **帧零拷贝**:从 RAM disk 分块读直接写 socket,不在 PSRAM 另存副本。
- **PSRAM 预算**:8MB 中 RAM disk 占 4-6MB,应用侧(TLS/HTTP/JSON/MCP/任务栈)约 0.3-0.5MB,
  不要在设备端做帧仓库或大缓冲。

## 哑设备原则

固件只**执行与搬运**,不做解码、比对、识别(见 `docs/agent-interface-mcp.md` §一)。
裁剪与 diff 在 watcher 侧算,固件只搬 `DIFF.IDX` 清单与图块。

## 国际化

若后续采用带屏 dongle(如 T-Dongle 0.96")做状态指示,屏上文案走
zh-Hans + en 双语表,禁止硬编码;协议层字段与文件名一律保持 ASCII 固定名。
