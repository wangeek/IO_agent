# 硬件选型与形态(2026-10-20)

芯片已定 ESP32-S3,本文回答:**什么配置、什么板形、分几步走**。

## 一、从协议倒推的硬件需求

| 需求项 | 结论 | 依据 |
|---|---|---|
| PSRAM | **≥8MB,必须 Octal** | RAM disk(FAT16 卷,设计值 **4-6MB**,非 32MB)+ WiFi/TinyUSB 栈。N8R2(2MB)不够 → 锁定 **N16R8** 配置 |
| Flash | ≥16MB | 固件 + OTA 双槽(各 **1MB**,实占 **884,176 B**)+ 运行时 payload 仓(`script0`/`script1` 各 512KB)+ 预置资源(薄引导进 RAM disk 镜像,完整内容走 payload 仓)。出厂镜像稀疏 → 固件只占约 **863KB** |
| RAM disk 卷 | 4-6MB,固化尺寸 | 内容:SNAP_A/B 双缓冲 ~600KB + CMD/RESP + 工具 ~50KB,覆盖写不累积;32MB 是 FAT16 上限不是设计值。**出厂镜像只内嵌到最后一个已用扇区(4MB 卷 → **97 扇区 / 49,664 B**)**,卷容量由 BPB 声明,固件在 PSRAM 开满卷并补零尾部。**槽位化(2026-10-22)**:3 个设备管理槽(`WATCH.PS1` 32K / `AGENT.SH` 32K / `AGENT.EXE` 64K)+ 1 个宿主信箱(`PAYLOAD.BIN` 128K),簇从卷末尾往前分配,**簇内容不进镜像 → 槽位容量在固件里占 0 字节** |
| USB | OTG(HID+MSC 复合) | FS 12Mbps,已有决策 |
| WiFi | 2.4GHz 板载 | 带外回传干道;插机箱后面板时板载天线衰减,产品级换外置(u.FL) |
| 供电 | 目标机 USB 500mA 预算 | S3+WiFi TX 峰值 ~400mA,留余量可行;不外接电源 |
| 调试口 | 开发期必须独立于 OTG | README 硬约束:启用 HID 后 OTG 口不再有日志 → 走 UART0,形态上要第二通道 |
| ESD | 产品板加 USB TVS | dongle 直插静电环境 |

## 二、三阶段形态

### 阶段 1:原型(立即)——官方 DevKitC-1-N16R8

- 双 Type-C 是决定性优势:一 **直连 GPIO19/20(OTG,插目标机)**,一 **经 CH343
  接 UART0(接开发机看日志)**——HID/MSC 与日志调试互不干扰,README 里
  "串口调试口消失"的痛点在开发期被板子本身化解。
- ~50-70 元,随处可买,今天就能开工固件。
- 形态是"大板+排针",只进实验室,不出门。

### 阶段 2:形态验证——现成 dongle 板(可跳过/并行)

- **LILYGO T-Dongle S3**(USB-A 公头直连 OTG + 0.96" 小屏,~60-90 元):
  插上目标机即完成真实场景验证——机箱后面板供电、天线遮挡、Windows 枚举
  键鼠+U盘三合一。小屏可当状态指示(连接/执行/告警),对运维场景有产品价值。
- 备选:Waveshare ESP32-S3-Zero(~30 元,超小,但母口需转接,不推荐做 dongle)。
- 注意:T-Dongle 无第二 USB 口 → 日志走 **WiFi**(固件发 UDP 到开发机),
  不引 UART0(要额外买 USB-TTL),也不切 USB-CDC(会占满 OTG,枚举不出键鼠)。

### 阶段 3:产品——自研板(远期可选,当前不启动)

- 核心:ESP32-S3-WROOM-1-**N16R8** 模组 + USB-A 公头直插 PCB + USB TVS +
  u.FL 外置天线座 + 外壳(3D 打印起步)。
- **动机澄清(2026-10-20)**:自研**不是成本优势**——小批量下打样+器件+焊接
  综合成本高于直接买 T-Dongle(上文 BOM 35-60 元未含摊销与工时)。真实动机
  是**产品自主性与形态定制**:①LILYGO 是第三方小厂消费板,无长期供货承诺,
  随时停版/改版,产品线不能建在它上面;②外置天线(插机箱后面板的金属遮挡,
  板载天线衰减)、外壳丝印卷标、ESD 防护只有自研能做。
- 启动条件:**当且仅当**项目要从"自用工具"转为"批量部署的产品"时再打样;
  在此之前 T-Dongle S3 封装已足够,甚至可长期作为最终形态。
- 日志策略:量产板**不焊 USB 转串口芯片**(没有 CH343 那样的桥),因此 UART 不能即插即用;
  但 **ESP32-S3-WROOM-1 模组的 UART0 引脚 GPIO43(TX)/GPIO44(RX) 本来就存在**,
  PCB 上不引出而已——那是"没出线",不是"没有"。
  → 做法:GPIO43 / GPIO44 / GND 三个焊盘引出为测试点(零元件成本),
  配一个 USB-TTL 转换器即可即插即用地看日志、并用 esptool 自动复位救砖。
  → 运行期日志主通道仍是 **WiFi**(见下)。
- **救砖通道(2026-10-20 定,量产必须项)**:两条,按顺序用:
  1. **UART 首选**:GPIO43/44 + GND 测试点 + USB-TTL → esptool 走 DTR/RTS 自动复位,
     干净、可脚本化(需采购清单里那个 USB-TTL)。
  2. **BOOT+EN 短接保底**:PCB 同时留 **BOOT(GPIO0) 与 EN 两个测试点**,
     探针短接 + 复位强制进 ROM 下载模式。这条不需要任何外购件,必须留。
  → 前提:**不能开 flash encryption / secure boot**(二者会禁用 ROM 的 USB-OTG 栈,
  连同 UART 下载一起废掉,只剩 OTA)。量产顺序必须是"先验证 OTA 可用,再开加密"。
  → 形态验证 dongle 板自带 BOOT 按钮(到货验收第 ① 条即依赖它),烧录没问题。

## 四、采购清单(2026-10-20 定稿)

**必买(仅 1 项,~50-70 元)**:

| 物品 | 数量 | 说明 |
|---|---|---|
| ESP32-S3 N16R8 双 Type-C 板 | 1 | 首选官方 DevKitC-1-N16R8;**平价替代 YD-ESP32-S3(VCC-GND) N16R8 版,~40-60 元**,布局相同(COM 口 CH340 + 原生 OTG 口)。不带排针版即可 |

**采购进展(2026-10-20)**:DevKitC-1-N16R8 已下单(~30 元),主力开发板。

**已评估排除(2026-10-20)**:

- Waveshare **ESP32-S3-GEEK**(~100 元):USB-A 公 + 外壳 + LCD 形态很好,
  但 **PSRAM 仅 2MB(R2)**,放不下 4-6MB RAM disk,硬门槛不达标。搬 TF 卡的
  改造路(磁盘落卡)被否:设计偏离 + TF 回购 + 断电 FAT 损坏面。无第二 USB 口,
  日志还要另配 USB-TTL。
- LILYGO **T-Dongle S3**(加运费 ~100 元):形态标杆,但同类通用 dongle
  (N16R8 + USB-A 公 + TF 槽 + 外壳)约 60 元可达,不值溢价。

**形态验证板(~60 元,2026-10-20 已通过甄别,下单)**:

卖家已确认:USB-A 直连 ESP32 引脚、无串口芯片("虚拟USB串口"= CDC/
USB-Serial-JTAG = GPIO19/20 原生),OTG 可切,HID+MSC 可行。

- 到货验收(3 分钟):① 插电脑 → 枚举出 USB 串行设备(按住 BOOT 再插应出现
  下载模式设备);② esptool 烧录 blink 确认烧录通路;③ 烧 TinyUSB HID 键盘
  示例 → 电脑出现键盘设备,即证明 OTG 通路。
- 分工:开发调试在 DevKitC;此板只做形态实况(机箱后面板供电/天线/三合一枚举),
  验证阶段日志走 WiFi。
- **零配件**:USB-A 公头直插即用(目标机/开发机同口),无需线材;TF 不买
  (PSRAM RAM disk 链路)、无串口需求(日志走 WiFi)、总线供电。仅纯 C 口
  笔记本调试时需 A母→C公 转接头。
- TF 槽用不上但无害。

**线材(用现有手机线,2026-10-20 确认不采购)**:两根 USB-A 公→Type-C 公数据线
(一根 OTG 口插目标机、一根第二口插开发机看日志)——主流手机线即此形态,
家中现成;要求:确认能传数据(连电脑拷过文件的),纯充电线不可用。

**暂缓买(开发阶段不需要,进入形态验证/量产再买 1 个)**:

| 物品 | 数量 | 何时要 | 说明 |
|---|---|---|---|
| **USB-TTL 转换器**(CH340 / CP2102 / FT232) | 1 | 形态验证板或量产板要"即插即用 UART"时 | **必须是 USB-TTL,不是 USB-RS232**(RS232 是 ±12V,会烧 3.3V IO);逻辑电平 3.3V;**别指望它给板子供电**(WiFi TX 峰值 ~400mA,转换器的 3.3V 输出带不动)→ 板子照旧 USB 供电,转换器只接 **TX / RX / GND 三线**;一个可复用于多块板 |

> 为什么不列入"必买":开发阶段 DevKitC 自带 CH343 桥,第二个 Type-C 就是串口,插上开发机
> 就能看日志,还能用 esptool 自动复位烧录。量产自研板把 GPIO43/44/GND 引到测试点后,
> 同一个转换器即可复用。

**不买(明确排除)**:

- **TF 卡 + 卡槽**:数据通道是 PSRAM RAM disk;TF 只是 ESP32 侧存储,
  解决不了"主机数据出来"的方向问题,对 Output 无贡献。工具仓库 16MB Flash 足够。
- **ESP-Prog JTAG**:UART 日志不够用、需要断点级调试时再议。
- **独立电源/电池**:全链路 USB 总线供电(峰值 ~400mA < 500mA 预算),
  产品形态同样保持无电池无外接;仅注意目标机前面板口供电弱时换后面板。

## 三、端点分配(2026-10-20 补)

S3 USB-OTG 预算:**共 7 个端点(含 EP0),6 个可配置,IN 方向最多 5 个(含 EP0)**
(Espressif esp-usb device guide,
https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_device.html)。
每类功能的端点开销是固定的:

| 功能 | IN | OUT |
|---|---|---|
| HID | 1(interrupt) | 0 或 1(可选 interrupt-OUT,LED 回读用) |
| MSC | 1(bulk) | 1(bulk) |
| CDC-ACM | 2(1 interrupt-IN 通知 + 1 bulk-IN) | 1(bulk-OUT) |
| vendor / WebUSB | 1(bulk) | 1(bulk) |

**拓扑反转(设备跑 MCP server)不占 USB 端点**——MCP 走 WiFi/TCP,USB 预算不变。

| 方案 | 占用 | 富余 | 取舍 |
|---|---|---|---|
| **A** 单 HID 接口 + 复合报告描述符(Report ID 区分键鼠) | EP0 + HID(IN+OUT) + MSC(IN+OUT) = 5 | 2 | 兼容性略逊,但留出余量 |
| **A′** 同 A 但砍掉 HID OUT(LED 已退出主路径) | 4 | 3 | **本项目采用** |
| **B** 键鼠拆成两个独立 HID 接口 | EP0 + 2×HID(IN+OUT) + MSC(IN+OUT) = 7 | 0 | Windows 干净枚举为两个设备,但无任何余量 |

**A′ 实测占用(2026-10-24,固件运行时日志)**:EP0 + 0x81(HID IN)+ 0x02(MSC OUT)+ 0x82(MSC IN)
= **含 EP0 共 3 个,IN 侧 2 个**。比方案表预估的 4 还少 1 —— 因为 A′ 本就不含 HID OUT,
方案表按"EP0 计入总数"记 4(含 HID OUT 的旧算式),实际落地砍掉 OUT 后为 3。
**富余:IN 2 个 / OUT 3 个**,与"富余端点留给 vendor bulk 调试口"一致。

- **CDC 不可加**:CDC 需 2 IN + 1 OUT,与 HID+MSC 相加后 IN 侧到硬上限、端点归零,不可取。
- **富余端点的正确用法**:塞一个 **vendor bulk 接口(1 IN + 1 OUT)**——Windows 绑 WinUSB、
  浏览器可 WebUSB 免驱。量产板不焊 USB 转串口芯片(日志走 WiFi),这条正好当**产线测试/设备调试口**。
- **MCP 的 HTTP 端点**:只需 1 个(Streamable HTTP 单端点),不占 USB 端点、无预算问题。
- **键鼠合并(A′)正是本项目的选择**:键盘与鼠标都要,放进同一个 HID 接口用 Report ID 区分,
  端点消耗与只做键盘完全相同 → 鼠标是"免费"的。详见 `agent-interface-mcp.md` §四.5。

### 调试通道:用哪个(不占原生 USB 端点)

先分清两种"串口":

- **UART0 物理引脚**(GPIO43/44)→ 开发板自带 CH343 芯片 → 板上的**第二个 Type-C 口**。
  这条链路与 S3 的原生 USB 控制器**完全无关**,不占任何 USB 端点。
- **USB CDC 虚拟串口**:跑在原生 USB 口上,电脑里出现 COM 口 → **要占 2 IN + 1 OUT**。

> **实测坑(2026-10-24)**:用 pyserial / .NET `SerialPort` 打开 COM3 时,
> **DTR/RTS 默认被置为有效**,开发板的自动复位电路(RTS → EN)会把 EN 拉低 → **一开串口板子就复位**。
> 后果:每次看日志都变成"重启 + 重跑一遍初始化",看起来像设备不稳定。
> 正确做法:**在 Open() 之前**就设 `DtrEnable = false; RtsEnable = false`,打开后再设一次。
> `tools/verify_unplug.ps1` 已按此实现(且复位判据只在拔插**之后**的复位才算失败——
> 拔插之前由开串口引起的复位是预期的、无害的)。

**结论:不做 USB CDC。**

| 阶段 | 板子 | 烧录 | 运行时日志 |
|---|---|---|---|
| 开发 | DevKitC-1-N16R8(双 Type-C) | 原生 USB(按住 BOOT 走 ROM DFU)或 UART 口 | **UART0 物理口**(第二 Type-C,CH343) |
| 形态验证 | ~60 元 dongle(单 USB-A 口) | 原生 USB ROM DFU | **WiFi 日志**(固件把 log 发到开发机 UDP 端口);次选:临时切 USB-CDC 单独看日志(会占满 OTG,此时枚举不出键鼠) |
| 量产 | 自研板(无 USB 转串口芯片) | **WiFi OTA**;救砖:①UART 测试点 + USB-TTL 自动复位(首选) ②BOOT+EN 短接(保底,无需外购件) | **WiFi**;可选 vendor bulk 端点作产线测试口 |

不要 CDC 的三条理由:

1. 开发期日志本来就有 UART0 物理通道,**零端点成本**;
2. 量产板与形态验证板都不焊 USB 转串口芯片,日志走 WiFi(已定),不需要 USB 通道
   (UART 引脚一直在,只是没出线,且日志不依赖它);
3. 做完 CDC 端点归零,换来的却是一个**端口号不稳定的 COM 口**(Windows 对无 INF
   绑定的 CDC 设备按可用编号分配,换个 USB 口插 COM 号就变,自动化脚本的经典坑)
   ——省下 3 个端点反而能换来更稳的 vendor bulk 调试口。

> **易混点澄清**:README 硬约束里"启用 HID 后原生串口调试口消失"指的是
> **芯片 ROM 的 USB-Serial-JTAG**(与 TinyUSB 抢同一个原生 USB PHY),
> 与开发板的 UART0 物理口无关——那条链路始终可用。

## 四、决策摘要

1. **芯片配置锁定 N16R8**,所有阶段统一,固件无缝迁移。
2. RAM disk 设计值 4-6MB(PSRAM 预算决定),协议文档中 32MB 仅为 FAT16 上限。
   **出厂镜像用稀疏格式**(只内嵌到最后一个已用扇区),省下的 flash 空间给 OTA 双槽与 payload 仓。
3. 原型开发用 DevKitC;形态实况验证用 ~60 元通用 dongle(N16R8),替代 T-Dongle。
4. 自研板等 L1 链路跑通后再启动打样。
5. **端点分配走 A′**(单 HID 接口 + 复合报告描述符,不要 HID OUT),富余 2 个端点
   留给 vendor 调试口。**实测已定案(2026-10-24)**:Windows 会按报告描述符把单 HID 接口
   拆成 `MI_00&COL01`(HID Keyboard Device)+ `MI_00&COL02`(HID-compliant mouse)两个集合,
   两个都独立可用 → **无需切 B**,A′ 成立。
6. **不做 USB CDC 虚拟串口**:开发日志走 UART0 物理口,量产/形态验证走 WiFi 日志。
   省下 3 个端点,避开 COM 号漂移的坑。
7. **USB 链路活性判据 = `tud_ready()`(`mounted && !suspended`),不能用 `TINYUSB_EVENT_DETACHED`**。
   **实测定案(2026-10-24)**:ESP32-S3 的 USB 外设是 **Synopsys DWC2 内核**,TinyUSB 的
   DWC2 device 模式**没有实现设备断开检测**——`dcd_dwc2.c` 里
   `"TODO check GINTSTS_DISCINT for disconnect detection"` 至今未实现,而 device 模式
   唯一能发 `DCD_EVENT_UNPLUGGED` 的分支是 `GOTGINT_SEDET`(Session End Detect),
   需要 OTG 模式 + VBUS 感知,devkit 上没接。
   实测物理拔线时:固件看到的是 `mounted=1 suspended=1` → `TINYUSB_EVENT_DETACHED`
   **永不触发**,`tud_mounted()` 也**不会翻转**(没有 UNPLUGGED 就没有 `cfg_num` 清零)。
   → "主机还在不在"只能看总线 suspend 状态。拔线瞬间 D+/D- 变 SE0、总线空闲 3ms
   即满足 suspend 条件,于是 `suspended=1` → `tud_ready()=false`。
   恢复可靠:重新插上后主机发 SOF,SOF 处理器清掉 `suspended`(`usbd.c` 的
   `DCD_EVENT_SOF` 分支),实测 140 ms 链路即恢复。
   **连带影响**:M2 注入链路的中止条件、agent 侧的"设备是否还在线"判断,都必须挂在
   `tud_ready()` 上,不能挂在 DETACHED 事件上。

## 五、开发环境(2026-10-22 实测)

| 事项 | 结论 |
|---|---|
| ESP-IDF | **v5.5.6**,装在 `D:\Espressif\frameworks\esp-idf`(C 盘只剩 24GB,放 D 盘);`install.bat esp32s3` |
| esptool | pip 装 **v5.6.0**(烧录/查芯片用) |
| **网络坑** | 本机 `HTTP_PROXY/HTTPS_PROXY=127.0.0.1:7897`(Clash)+ 系统代理同址;该代理**间歇性重置连接**("Cannot connect to proxy" / schannel 握手失败)。git 大克隆易被打断 → 保留重试循环;**下载类**(pip wheel、IDF 工具链)走代理更稳(工具自带重试/续传) |
| 烧录口 | DevKitC 的 **COM 口**(CH343→UART0)烧录最省事:`idf.py -p COMx flash` 自动复位,不用手按 BOOT;原生 **USB 口**按 BOOT 进 ROM DFU 亦可。注意:HID 起来后原生 USB-Serial-JTAG 消失,但 **COM 口是 UART0 物理链路,始终可用** |
| 端口分工 | 目标机 = **USB** 口(原生 OTG,产品主接口);开发机 = **COM** 口(日志) |
