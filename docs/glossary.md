# 术语表

读其它文档前先扫一眼这里。**本文档存在的原因**:项目内部大量使用 L0/L1/L2、N1-N7 这类编号,
以及 NVS、CDC、SoftAP 这类缩写,新读者容易在"术语"上卡住而不是在"设计"上卡住。

## 一、硬件与固件

| 术语 | 人话 |
|---|---|
| **ESP32-S3 / S3** | 乐信的 MCU 芯片。自带 WiFi、PSRAM、原生 USB。本项目的主角 |
| **原生 USB / USB-OTG** | 芯片内部直接实现的 USB 控制器(不靠外接转换芯片),能**同时**扮演多种 USB 设备(键盘 + U 盘) |
| **PHY** | 把 USB 电气信号送进芯片的那层模拟电路。S3 只有一组 USB PHY,OTG 与调试串口共用 → 开了 HID 就没法用同一个口看串口日志 |
| **PSRAM** | 外挂的"运行内存",**断电即失**。本项目的虚拟 U 盘(RAM disk)就放在这里 |
| **Flash** | 芯片内的"硬盘",断电不丢。存固件、存 RAM disk 的初始镜像、存 NVS 分区 |
| **NVS** | Non-Volatile Storage = Flash 里划出的一小块**键值对**存储区(像一张很小的配置表)。断电不丢,容量几百 KB 级,专放配置和密钥。本项目用它存:WiFi 凭据、PSK/配对信息、watcher 拉起策略、设备 UUID(拓扑反转后**不再存 agent 端点地址**) |
| **RAM disk** | 用内存假装出来的磁盘(= 本项目的虚拟 U 盘)。**内容随断电消失**,每次上电从 Flash 里的镜像重建 → 所以"要留存的东西"不能放这儿,必须放 NVS |
| **FAT16** | U 盘用的文件系统。**BPB** 是它的"身份证参数"(每扇区字节数、每簇扇区数、卷序列号等),写错 Windows 会提示"需要格式化"甚至根本不认卷 |
| **槽位 slot** | RAM disk 上**预留的固定位置**的文件位(`WATCH.PS1` 32K / `AGENT.SH` 32K / `AGENT.EXE` 64K)+ 一个宿主信箱(`PAYLOAD.BIN` 128K)。簇链与目录项在出厂镜像里就分配好,设备端只需 `memcpy` + 定点改 4 字节 size 字段 |
| **真源 / 副本** | **flash 里的 payload 仓是"真源"**(宿主够不到);RAM disk 槽位是"每次开机从真源铺开的副本"。运行期下发只写真源、不碰副本 —— 因为 RAM disk 是与宿主共享的可写存储,运行期改它 = 和宿主抢同一块簇 |
| **端点 endpoint** | USB 上一条可独立收发的"通道"。端点数量是**硬件上限**,直接决定一块芯片能同时扮演几种 USB 设备 |
| **MSC** | Mass Storage Class = USB 存储设备类。芯片在电脑上表现成一个 **U 盘** |
| **HID** | Human Interface Device = 人机接口设备类。键盘/鼠标/手柄都走这个类。**HID IN** = 芯片→电脑(注入按键);**HID OUT** = 电脑→芯片(键盘灯状态回读) |
| **CDC** | Communications Device Class = 通信设备类,表现为一个**虚拟串口**(Windows 里多出一个 COM3)。手机上"充电线转接头"就是它。本项目**不做**它(占 2 IN + 1 OUT 端点,且 COM 号会漂移) |
| **TinyUSB** | 乐信维护的开源 USB 协议栈。本项目的描述符自写在这一层 |
| **USB-Serial-JTAG** | 芯片 ROM 自带的 USB 串口(烧录用)。与 TinyUSB 抢同一个原生 USB PHY → 应用一开 TinyUSB 它就没了(按住 BOOT 进下载模式时 ROM 才接管)。**与开发板的 UART0 物理口是两回事** |
| **描述符 descriptor** | USB 设备的"身份证",电脑靠它认出你是键盘还是 U 盘 |
| **P4** | 乐信的 ESP32-P4:没有射频(不能连 WiFi)但 USB/显示/编解码更强。项目规划的**二代**硬件 |
| **LT6911UXC / TC358743** | HDMI → MIPI-CSI 桥接芯片(P4 直采路线用,树莓派 HDMI-in 同款) |

## 二、本项目的通道分层(自定编号,最容易被误解的部分)

| 编号 | 是什么 | 通俗说 |
|---|---|---|
| **L0** | 硬件直采 | 二代路线:目标机 HDMI 输出 → 桥接芯片 → P4 采像素 → WiFi 发回。**目标机完全不参与**。远期 |
| **L1** | U 盘驻守脚本 | **默认路线**:目标机上临时跑一个 PowerShell 脚本(藏在 U 盘里,不安装不落盘),脚本截图写进 U 盘,ESP32 读走再经 WiFi 上传 |
| **L2** | 目标机有网直连 | 原设计:目标机有外网时脚本跳过 U 盘与 ESP32 直接推给 agent。**已作废**——拓扑反转为 agent 主动连设备,不再有"设备推数据"这条路 |

三者关系:L0 远期硬件路线;L1 是默认且必有的;L2 已作废。
**现行通路只有一条**:agent ──WiFi/MCP──▶ 设备 ──HID / RAM disk──▶ 目标机。

## 三、Agent 侧概念

| 术语 | 人话 |
|---|---|
| **input** | MCP 原语:下发一个**动作序列**(按键/鼠标 + 内嵌延时),ESP32 本地按序执行,不逐条往返 |
| **capture** | MCP 原语:回读**最新一帧**画面(MCP 原生 image 内容块)。差分区域由 agent 侧算 |
| **ROI** | Region of Interest = 感兴趣区域。截屏只要那一小块(传统桌面场景里这块价值很高) |
| **diff** | 相邻两帧的**变化区域清单**。由 watcher 算(它手里有像素),ESP32 只搬运清单 |
| **哑设备** | 固件只**执行与搬运**,不做解码、比对、识别。OCR/判断/决策全在 agent 侧 |
| **能力注册表** | 固件知道自己当前有哪些通道(编译期开关 + 运行时查询)。状态机只查"我现在有什么",不假设某级存在 |
| **Delta T / ΔT** | 点按之间的停顿间隔,用来等 UI 响应 |
| **屏幕即接口** | 让目标机把数据显示在屏幕上,agent 用视觉读屏。不回读通道也能拿到状态 |

## 四、系统与安全

| 术语 | 人话 |
|---|---|
| **UAC** | Windows 管理员提权弹窗 |
| **安全桌面** | Windows 提权/锁屏时切到的**隔离桌面**,普通程序看不见也点不到 |
| **SAS** | Ctrl+Alt+Del。Windows 拦截非特权程序注入,无法模拟 |
| **EDR / AMSI** | 企业终端防护 / 脚本扫描。会拦截从可移动盘运行的 ps1,强管控环境 L1 会失效 |
| **Secure Boot / flash encryption** | ESP32 的启动校验与 Flash 加密。**开启后 ROM 里的 USB-OTG 栈被禁用** → 本项目 USB 口就是产品主接口,故量产必须走 WiFi OTA |
| **SoftAP** | ESP32 自己开的 WiFi 热点(手机能连上去) |
| **TLS / PSK / mTLS** | 加密通道。**PSK** = 预共享密钥(免证书链管理,成本最低);mTLS = 双向证书认证 |
| **mDNS** | 局域网内自动发现设备(不用手填 IP) |
| **DXGI Desktop Duplication** | Windows 官方的桌面复制 API,**能拍到独占全屏**游戏画面(OBS 显示捕获同款) |
| **GDI / CopyFromScreen** | 老一代 Windows 绘图接口及其"截当前屏幕"函数。**拍不到真独占全屏**(DirectDraw 老游戏黑屏) |
| **WMIC / Get-Volume** | Windows 命令行里读磁盘信息(尤其卷序列号)的工具 |

## 五、MCP 与协议

| 术语 | 人话 |
|---|---|
| **MCP** | Model Context Protocol,让 AI agent 调用外部工具的开放协议。本项目由**设备端**实现 MCP server |
| **JSON-RPC 2.0** | MCP 底层用的消息格式:一个 JSON 对象表示一次请求/响应 |
| **Streamable HTTP** | MCP 当前的推荐传输:单个 HTTP 端点,POST 进请求、可回 SSE 流。MCU 上比老式 SSE 双通道省 |
| **SSE** | Server-Sent Events,服务端单向推流的老传输方式 |
| **blob** | MCP 里表示二进制资源的方式(裸字节)。传 300KB 帧用 blob,不要 base64 塞进 JSON(会膨胀 33%) |
| **mDNS** | 局域网自动发现:设备广播 `_ioagent._tcp.local`,客户端扫到即知地址,不用配 IP |
| **PSK** | Pre-Shared Key,预共享密钥。TLS-PSK 双方持同一把密钥 → 天然双向认证,免证书链 |
| **配对码** | 一次性短码,用来把"这台设备"和"这个 agent"绑在一起,换取长期 PSK |
| **A/B 快照** | flash 里两份交替写的 payload 仓(`script0`/`script1`)。`seq` 大者胜,但 magic + 目录 crc + 每条 payload crc **全过**才算有效;写入永远写非生效的那份 → 写到一半掉电不影响当前生效的那份,**无需额外提交标记** |
| **信箱 inbox** | 宿主往 RAM disk 的 `PAYLOAD.BIN` 里写「64 字节头 + payload」的通道。设备 200ms 轮询,发现变化就落 flash。必须**原位覆写**(`r+b`),截断会让 Windows 释放簇链、设备按构建期偏移就读不到 |

## 六、编号约定

- **N1–N7**:agent 接口层的需求裁定编号,见 `agent-interface-mcp.md` §三。
- **M0–M6**:实施里程碑编号,见 `implementation-plan.md` §二。
- **Stage -1/0/1/2**:bootstrap 阶段编号,见 `implementation-plan.md` 与 `connectivity-and-binding.md` §八。
- **结论 1–7**:2026-10-20 设计评审的结论编号,见 `design-review.md`。
