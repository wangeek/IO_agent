# IO_Agent — ESP32 双向 I/O Agent

> 术语不熟先看 **[`docs/glossary.md`](docs/glossary.md)**(NVS / CDC / L0-L2 / SoftAP 等一律在那儿解释)。

目标:一块 ESP32-S3,插在"不能装软件的机器"上,形成 输入注入 + 回读验证 的闭环 agent。

定位(2026-10-20):ESP32 外设给 AI agent 提供 **物理硬件级输入**(HID 键鼠注入)
与**多形态回读输出**(驻守程序或硬件接口的截图、网络与文件读取),最大程度模拟
人工与机器交互,支撑自动化测试/运维;并以 **MCP、skill** 形式接入 agent 工作流。

## 架构决策记录(已定,不重开)

| 决策 | 结论 |
|---|---|
| 芯片 | ESP32-S3(唯一同时有 USB Device HID + USB Host/OTG 的主流型号;C3/C6 无 USB Device HID) |
| USB 组合 | **HID + MSC 复合设备**,4/6 端点,剩 2 富余 |
| ESP32 → 机器(执行) | HID IN(键盘/鼠标注入) |
| 机器 → ESP32(数据) | **v2:主机写虚拟 U 盘(标准 MSC 主方向)+ ESP32 本地读 RAM disk**;ESP32 主动向主机塞数据/主机反向读盘已被证伪(MSC 缓存 + 设备无法发起传输),见 `docs/output-channel-design.md` |
| 回传干道 | **双路,运行时择一**:ESP32 自带 WiFi 带外(与目标机网络无关)↔ L2 目标机有网直连;WiFi 通走带外,不通自动降级 L2。**L2 是现实中更可能成功的主路径**(目标机通常有网,ESP32 常连不上外网),详见 `docs/connectivity-and-binding.md` |
| 确认/ACK | U 盘文件即回执(RESP);**LED 通道退出主路径**,降为可选调试/心跳信号(2026-10-20 定案,见 `docs/design-review.md` 结论 4) |
| Output 分层 | L1 U 盘驻守脚本(默认,免安装) / L0 硬件直采=P4 二代(远期,产品内唯一硬件路线) / L2 目标机有网直连(加速)。**一体化形态约束:外置采集棒等配件堆方案不入实施计划** |
| 指标 | 稳定形态全屏截图 ≤1s(L1 预算:预览帧 ~350ms / 全帧按需 ~570ms);bootstrap ≤10s 无人工;稳态 USB 写入 <300KB/s |
| 采集方式 | **GDI `CopyFromScreen`**(一代保传统桌面用途,Windows 自带、零安装、最快);DXGI Desktop Duplication 后续可插拔(采集接口缝已留)。真独占全屏可能黑帧 = 已知边界 |
| 配网/绑定 | 配置来源三选一:**A 目标机代取**(目标机有外网时由它取配置写盘,推荐)/ B 人工投送配置文件 / C SoftAP + 手机(异地分发才做);一次性绑定码换长期 token,设备 UUID 进回传包头 |
| bootstrap | Stage -1:配网与绑定(出厂已预置则跳过) → Stage 0:USB 枚举 + HID 通路 + agent 链路连通性(LED 往返降为可选加分项) → Stage 1:HID 敲命令拉起常驻服务 → Stage 2:服务外连+能力自陈 → 之后按目标机器能力降级选通道 |
| 固件组织 | 每级 = 编译期开关 + 能力注册表;状态机只查"我现在有什么",不假设某级存在 |
| 描述符 | 自写(TinyUSB 层),**不用 Arduino 便利库** — 后者常不实现 HID OUT 端点、且复合设备端点分配不受控(LED 已退出主路径,端点分配仍是硬理由) |

## 已知硬约束

- S3 的 USB-OTG 与 USB-Serial-JTAG 共享一个 PHY:启用 HID 后原生串口调试口消失,日志走 UART0。
- **端点预算**:S2/S3 控制器共 7 个端点(含 EP0)、最多 5 个 IN(含 EP0)→ 6 个可配置。
  HID+MSC 占 4,剩 2;**USB CDC 塞不下**(需 3 个)→ "USB 串口当数据回传"没有退路。
- **flash encryption / secure boot 会禁用 ROM 里的 USB-OTG 栈** → 本项目 USB 口就是产品主接口,
  量产固件**必须走 WiFi OTA**;开发期不开加密(否则每次更新都要全量擦除)。
- LED 通道约 3–9 bit/s,只做 ACK/心跳,不做数据。
- macOS 多数版本不同步下发 LED 报告 → LED 通道探测失败时必须有降级分支。
- FAT 无 FIFO 语义,`echo > file` 是 fire-and-forget → 回执走 RESP 文件(LED 已退出主路径)。
- ESP32 在 FAT 上主动建文件易损坏 → ESP32 只读文件,写回交给主机。
- ESP32 不能向主机发起任何数据(USB 主从 + FAT 缓存)→ 配网/绑定只能"主机推送到盘上"。

## 目录

- `docs/glossary.md` — **术语表**(读其它文档前先扫一眼)
- `docs/agent-interface-mcp.md` — **Agent 接口层:手眼模型**(act/observe 原语、N1-N7 裁定、UAC 补充、采集方式定案)
- `docs/output-channel-design.md` — Output 通道设计 v2(三层架构、延迟预算、屏幕即接口模式)
- `docs/connectivity-and-binding.md` — **配网、绑定与回传**(两级配网、运行时分级回传、传输安全、产线约束)
- `docs/hardware-selection.md` — 硬件选型与形态(N16R8 锁定、三阶段板形、供电/天线/日志策略)
- `docs/implementation-plan.md` — **实施计划**(仓库骨架、里程碑、契约冻结清单)
- `docs/design-review.md` — 2026-10-20 设计评审:7 条发现与定案结论
- `docs/msc-watcher-protocol.md` — v1 文件协议,回读部分已被 v2 取代;CMD/RESP 约定平移至 v2,**LED 段落已作废**
