# IO_Agent — ESP32 双向 I/O Agent

> 术语不熟先看 **[`docs/glossary.md`](docs/glossary.md)**(NVS / CDC / L0-L2 / SoftAP 等一律在那儿解释)。

目标:一块 ESP32-S3,插在"不能装软件的机器"上,形成 输入注入 + 回读验证 的闭环 agent。

定位(2026-10-20):ESP32 外设给 AI agent 提供 **物理硬件级输入**(HID 键鼠注入)
与**多形态回读输出**(驻守程序或硬件接口的截图、网络与文件读取),最大程度模拟
人工与机器交互,支撑自动化测试/运维;并以 **MCP 服务端** 形式直接接入 agent 工作流
(设备自带 MCP server,agent 主动连设备)。

## 当前进度(2026-10-24)

**M0 / M1 已完成并实板验证通过**(详见 `docs/implementation-plan.md` 的 M0 / M1 实测结果):

| 里程碑 | 状态 | 关键结论 |
|---|---|---|
| M0 环境与骨架 | ✅ | ESP-IDF v5.5.6 + 23/23 子模块双通道校验 + 固件编译/烧录通过(稀疏镜像 + 槽位化后 `io_agent.bin` 884,176 B) |
| M1 固件最小可枚举 | ✅ | 复合设备在 Windows 上同时枚举出 **键盘 + 鼠标 + 可移动磁盘**;E: 盘读写字节级一致;端点占用 3 个(IN 侧 2) |
| M1 拔插不崩 | ✅ | 实测 ABSENT→PRESENT 完整循环:主机侧消失/重现,**固件侧无复位**(uptime 连续)、链路掉线/恢复均被观测到、重新枚举成功 |
| T2 运行时下发(2026-10-22) | ✅ | 出厂镜像**不再内嵌** `watch.ps1`,只留 3 个设备管理槽 + 1 个宿主信箱(**槽位容量在固件里占 0 字节**);内容落 flash `script0`/`script1` A/B 快照(断电不丢),经 `PAYLOAD.BIN` 信箱下发(M5 后 MCP `update` 直接调同一 API)。**真源/副本契约:flash 是唯一真源(宿主够不到),RAM disk 槽位只在开机时铺开;运行期下发只写 flash** → 与宿主零竞争。实板双槽 A/B 验收 **17 项全绿** |
| M2 注入链路(2026-10-23 复测) | ✅ | 设备端 HID 注入**端到端打通并客观验收**:远程 `input macro=selftest` → 设备走完注入序列 → 目标机执行命令把 `INJECT_OK` 写回盘 → 设备在盘上看到 `INJ.TXT`(`marker_seen:true`);再 `input macro=launch_watcher` → `CAP.TXT` 出现、watcher 起帧(~2.2 fps)、设备经 `/files/<name>` 回传**真实桌面帧**。US 布局表(95 字符)带自检 + 仓库内独立交叉校验脚本 `tools/check_us_layout.py`。**注入默认关**,只走远程显式触发(见决策表) |
| M5 远程链路(2026-10-22) | ✅ | 设备侧 MCP server 落地(HTTP + JSON-RPC 2.0):`/mcp` + `/ping` + `/files/<name>`,6 个工具(`status`/`ls`/`read`/`capture`/`input`/`update`)。实测闭环:远程下发 → flash → 开机 overlay → 设备只读解析 → HTTP 回传 **sha256 逐字节一致**;`input(hotkey win+r)` 目标机窗口列表新增「运行」。凭据走 NVS + UART0 控制台配网(**不进仓库/不进固件**)。TLS-PSK 待做 |
| M6 mDNS 发现(2026-10-23) | ✅ | 设备用 **lwIP 原生 mdns responder**(不引 esp-mdns,组件管理器关着也能离线复现)广播 `_ioagent._tcp.local`。宿主侧 `tools/discover.py` 自带解析器(`--json` 直接给 MCP 端点),实测从 `ioagent-lab.local` 解出 `172.17.21.225:8080`。**踩了两坑**:① mDNS 装配**必须在 TCP/IP 线程里做**(`sys_timeout` 定时器链表数据竞争 ⇒ 状态机卡在 PROBING ⇒ 注册成功但线上一个包都不发、也不应答);② 宿主侧**必须 `IP_ADD_MEMBERSHIP` 加入 224.0.0.251**,绑端口 ≠ 入组,AP 开着 IGMP snooping 时不入组就收不到组播应答 |
| M8 对外接口改名(2026-10-24) | ✅ | 旧名**彻底替换**(不做兼容):工具集 5→6,全改成「一个动词 = 一件事」、全小写单词、零分隔符:`device/info`→`status`、`observe`→`read`(参数 `target`→`name`)、`act`→`input`、`deploy_payload`→`update`,新增 `ls`/`capture`;弃用 `output`/`write`/`type`;`watcher/status` 取消并进 `status.watcher.running`。端点统一:`GET /blob/b0..b3`→**`GET /files/<name>`**(按文件名取),新增 `GET /mcp`→**405**(规范要求)。MCP 原生 **ImageContent** 取代 blob URL;每个结果带 `structuredContent`/`outputSchema`/`title`;**`isError` 补齐到所有工具**,未知工具回 **-32602**。改名过程中撞出并修掉两个真机 bug(错误被塞进 `result`、通配 URI 默认不展开)。验收:`tools/contract_probe.py` **24 项全绿**。⚠ 随后又发现**第三个 bug**:本轮新增的 FAT16 mtime 解析把偏移量写成了 `0x24/0x26`(已越过目录项 32 字节,读到的是**下一个条目**),读出来是 1995 年的**合法**日期 —— 而"挑最新一帧"正是按 mtime 排的 ⇒ 挑帧逻辑一直错。已修为 `0x16/0x18`(FAT16 的最后写入时间/日期),回归 `tools/mtime_probe.py` **4 项全绿**(跨文件偏移恒定/变更可见/先后可排/`capture` 挑中最后写的帧) |
| M7 MCP 接口硬化(2026-10-23) | ✅ | 请求路径硬化(P2)+ 协议细节(P4)一并落地:body 上限 128 KB → 413、`Content-Type` 非 JSON → 415、`Host`/`Origin` 绑定设备自己 → 403(防 DNS rebinding)、协议版本协商(支持 2024-11-05/2025-03-26/2025-06-18)、去掉假 session id(无状态,不发 `Mcp-Session-Id`)、通知回 202。**拒绝前先抽干 body**(带未读请求体关连接会发 RST,客户端只看到"连接被重置"而不是状态码)。验收:`tools/mcp_hardening_probe.py` **17 项全绿**,MCP 五工具与 mDNS 发现回归正常 |

- **实板**:DevKitC-1-N16R8(Flash 16MB + Octal PSRAM 8MB,esptool 实测确认)。
- **主机侧枚举**:`HID\VID_303A&PID_8001&MI_00&COL01`(HID Keyboard Device)、
  `&MI_00&COL02`(HID-compliant mouse)、`&MI_01`(USB Mass Storage Device)、
  卷 `IOAGENT`(FAT / Removable / 4MB)。
- **下一步**:M2 注入链路(键鼠动作序列本地定时执行)。

## 架构决策记录(已定,不重开)

| 决策 | 结论 |
|---|---|
| 芯片 | ESP32-S3(唯一同时有 USB Device HID + USB Host/OTG 的主流型号;C3/C6 无 USB Device HID) |
| USB 组合 | **HID + MSC 复合设备**,4/6 端点,剩 2 富余 |
| HID 接口数 | **单 HID 接口 + 复合报告描述符**(Report ID 1=键盘 / 2=鼠标),不实现 HID OUT。**M1 实测已确认(2026-10-24)**:Windows **会**按报告描述符把单 HID 接口拆成两个集合 —— `MI_00&COL01`=HID Keyboard Device、`MI_00&COL02`=HID-compliant mouse,两个都独立可用。**原顾虑(以为只会枚举成一个 HID 设备)已推翻**,描述符方案成立 |
| ESP32 → 机器(执行) | HID IN(键盘/鼠标注入) |
| 机器 → ESP32(数据) | **v2:主机写虚拟 U 盘(标准 MSC 主方向)+ ESP32 本地读 RAM disk**;ESP32 主动向主机塞数据/主机反向读盘已被证伪(MSC 缓存 + 设备无法发起传输),见 `docs/output-channel-design.md` |
| 回传干道 | **方向反转(2026-10-20 定案)**:agent **主动连设备**,设备端跑 MCP server(WiFi + mDNS 自动发现)。设备不外推数据,"帧回给 agent" = 设备读 RAM disk 后应请求直传。**带外的确切含义**:设备连的是你的网络,与目标机网络无关 → 目标机断网/隔离网照样可控 |
| 确认/ACK | U 盘文件即回执(RESP);**LED 通道退出主路径**,降为可选调试/心跳信号(2026-10-20 定案,见 `docs/design-review.md` 结论 4) |
| Output 分层 | L1 U 盘驻守脚本(默认,免安装)/ L0 硬件直采=P4 二代(远期,产品内唯一硬件路线)/ ~~L2 目标机有网直连~~(**已作废**:拓扑反转为 agent 主动连设备)。**一体化形态约束:外置采集棒等配件堆方案不入实施计划** |
| MCP 宿主 | **设备端(ESP32)自跑 MCP server**:Streamable HTTP + JSON-RPC 2.0,工具集 **`status`/`ls`/`read`/`capture`/`input`/`update`**(全是动词、零分隔符;`update` = T2 运行时下发,2026-10-22 增);帧用 MCP 原生 image 内容块返回(超过 64 KB 才给 `/files/<name>` URL)。兜底:极简 HTTP API + host 侧薄适配器 |
| 指标 | 稳定形态全屏截图 ≤1s(端到端:等新帧 ≤500ms + 读盘 ~10ms + WiFi 传输 50-100ms ≈ 0.6-0.7s);bootstrap ≤10s 无人工;稳态 USB 写入 <300KB/s |
| 采集方式 | **GDI `CopyFromScreen`**(一代保传统桌面用途,Windows 自带、零安装、最快);DXGI Desktop Duplication 后续可插拔(采集接口缝已留)。真独占全屏可能黑帧 = 已知边界 |
| 配网/绑定 | 设备只需 **WiFi 凭据 + 一把 PSK**。**已实现(2026-10-23)**:① **U 盘配网文件 `DEV.CFG`** —— 宿主把 `ssid=` / `pass=` / `name=` 写到虚拟盘的 `DEV.CFG`,设备**开机取一次、之后每 5 秒再看一眼**,内容变了就写进 NVS 并重连。这是**唯一方向天然对**的通道(设备还没联网时 agent 连不上它,串口也未必接着)。`pass` 键可选:不写就沿用 NVS 里已有的密码,写 `pass=`(空)表示开放网络。文件内容记 CRC,**同一份文件不重复应用**(运行时改完网,盘上旧文件不会把它改回去)。② 串口控制台 `wifi set` ③ 设备 PSK 走 NVS(**不进仓库、不进固件二进制**)。**mDNS 自动发现已实现(2026-10-23)**:设备广播 `_ioagent._tcp.local`,宿主用 `tools/discover.py` 解出端点。**TLS-PSK 双向认证**待做 |
| bootstrap | Stage 0:上电自检 → 连 WiFi → mDNS 广播(agent 可发现)→ Stage 1:按策略用 HID 拉起目标机驻守脚本 → Stage 2:agent 连上并配对 + 能力自陈 → 之后按目标机器能力降级选通道 |
| 固件组织 | 每级 = 编译期开关 + 能力注册表;状态机只查"我现在有什么",不假设某级存在 |
| USB 链路活性判据 | **`tud_ready()` = `mounted && !suspended`**,不靠 `TINYUSB_EVENT_DETACHED`。**M1 实测确认(2026-10-24)**:ESP32-S3 的 USB 是 Synopsys DWC2 内核,TinyUSB 的 DWC2 device 模式**不实现设备断开检测**(`dcd_dwc2.c` 里 `GINTSTS_DISCINT` 仍是 TODO,唯一的 UNPLUGGED 来源 `GOTGINT_SEDET` 需要 OTG+VBUS 感知,devkit 没接)→ 物理拔线实际只上报 **SUSPEND**,`TINYUSB_EVENT_DETACHED` **永不触发**。实测拔线时 `mounted=1 suspended=1`,`tud_mounted()` 不会翻转,所以也不能用它。重新插上后主机发 SOF,SOF 处理器清掉 `suspended` → 链路自动恢复(实测 140 ms) |
| 描述符 | 自写(TinyUSB 层),**不用 Arduino 便利库** — 后者常不实现 HID OUT 端点、且复合设备端点分配不受控(LED 已退出主路径,端点分配仍是硬理由) |
| 构建/依赖 | **离线可复现**:esp_tinyusb 2.4.0 + tinyusb 0.21.0~2 **vendored** 到 `firmware/components/`,禁用 IDF 组件管理器(`set(ENV{IDF_COMPONENT_MANAGER} 0)`)。IDF 子模块用 `tools/fetch_idf_submodules.py` 走 **GitHub codeload tarball** 离线抓取并做逐文件 SHA-1 校验 —— 本机代理 `127.0.0.1:7897` 会随机截断 git smart-HTTP 与大文件,`git clone` 不可靠(vendored 组件的 3 处补丁见 `firmware/components/README-vendor-patches.md`) |
| RAM disk 体积 | **出厂镜像用稀疏格式 + 槽位化(2026-10-22)**：卷容量由 BPB 声明(4MB),镜像文件只写到最后一个已用扇区(**97 扇区 / 49,664 B**),固件启动时在 PSRAM 开满 4MB 卷并把尾部补零 → 内嵌镜像 **4MB → 48KB**,`io_agent.bin` **约 4.42MB → 884,176 B**。宿主看到的仍是完整 4MB 可移动卷(实测 `size=4,144,640`),设备端依然零 FAT 逻辑。**"AI 现场就能生成的启动文件"不占固件位置** —— 出厂镜像只留薄引导槽:3 个设备管理槽(`WATCH.PS1` 32K / `AGENT.SH` 32K / `AGENT.EXE` 64K)+ 1 个宿主信箱(`PAYLOAD.BIN` 128K),**槽位簇从卷末尾往前分配**,簇链与目录项进镜像、**簇内容不进镜像** → 槽位容量在固件里占 **0 字节** |
| Flash 分区 | `nvs`24K / `phy_init`4K / `otadata`8K / `ota_0` **2MB** / `ota_1` **2MB** / `script0`512K / `script1`512K / 余 10.9MB。OTA 双槽从 6MB 缩到 2MB(app 实占 **884,176 B**,余 1,212,976 B(57.8%);`script0/1` 是 T2 的运行时 payload 仓(A/B),断电不丢 |
| watcher 启动 | **按卷标解析,不写死盘符(2026-10-22)**:dongle 拔线即断电,重入后盘符可能变。设备注入 `cmd /c for %d in (I O A G E N T) do @if exist %d:\WATCH.PS1 start "" powershell -ExecutionPolicy Bypass -WindowStyle Hidden %d:\WATCH.PS1`,注入后轮询 RAM disk 里的 `CAP.TXT` 判定拉起成功,未出现则 2s 间隔重试(最多 5 次)。**注入前必做 `focus_reset`(见下行「注入序列」)** |
| 注入序列(2026-10-23 实测定案) | `Win+M` → `Win+R` → `Ctrl+A` → 打字 → `Enter`,每步之间 500ms。五个关键点:**① keycode 是 HID 用法 ID 不是 ASCII** —— 传 `'R'`(0x52)会按成小键盘的键,`Win+R` 根本不触发(这个 bug 让组合键从来没生效过);字母要用 `io_hid_keycode_for_letter()`。**② 组合键必须按住 ≥600ms** —— 30ms 会被 Windows 整段丢弃。**③ 单键必须按住 ≥60ms** —— 8ms 时微软拼音把输入打成乱码(`ni hao ` 变成 49 个"啊")。**④ 输入态由 IME 实例记忆,新开的运行框继承上一次的态**(不是回到注册表默认)。⇒ **没法盲发把态拨准**:Shift 是 toggle,盲按一次的结果取决于上一轮 ⇒ 历史上实测 OK/FAIL 交替;`Ctrl+Space` 在微软拼音里和 Shift 是**同一个开关**,按两次等于没切(同报告/先报修饰键两种发法都实测无效)。⇒ **改成"先测后打"**:`macro=selftest` 就是探针(它打的是自检命令,`INJ.TXT` 出现 = 当时是纯 ASCII ⇒ 当前是英文态;不出现 = 中文态);探针不过 → `macro=ime_toggle`(单按 Shift,只切态不输入字符)→ 再探针;然后 `macro=launch_watcher` 按探针结论选档:英文态用默认,**中文态加 `ime:"toggle"`**(新框继承中文 → Shift 一下 → 英文)。**设备端不自动重试**(注入是破坏性操作),重试由 agent 决定。**⑤ 运行框会预填历史命令**(且不是选中态) → 必须先 `Ctrl+A` 全选,否则我们的命令被追加在它后面变成废命令 |
| 注入触发方式 | **默认关,只走远程显式触发(2026-10-22 事故后定案)**。往用户机器注入按键是**破坏性操作**:目标机若在锁屏,按键会被当成 PIN 输入 → 持续失败 → 触发账户防暴力锁定(实测事故)。因此 `IO_LAUNCHER_AUTOSTART` 默认 **0**(开机不自动拉起),注入只能由远程 `input` **单次触发、零重试**(失败不自动重发 —— 对"打系统快捷键"这类动作,重试=持续破坏)。异步模型下**动作不再自动回传前后两帧**(请求早就返回了),由 agent 自己用 `capture` 取前后帧来比对。验收放虚拟机/靶机;主力机上只能"人在场 + 知情 + 手动单次触发"。**远程另给 `macro=selftest`**:注入一条把标记写回盘的命令,设备以盘上是否出现 `INJ.TXT` 判定"HID 真的到达了目标机"(`marker_seen`)——设备自己看不到屏幕,这是唯一客观判据 |
| MCP 工具的边界(2026-10-23 定调) | **不做会切断自己这条连接的动作**。改 WiFi 会重连 ⇒ 断的正是这次调用所在的 HTTP 连接,调用方拿到的是"连接掉了",分不清"配网成功"还是"链路断了" ⇒ **不提供 `wifi set` 这类同步工具**。改网走**别的通道**(U 盘 `DEV.CFG` ✅,发起方是宿主、不依赖那条连接)。将来若要在线改网,形态只能是"**先应答 accepted,再断开重连**"的异步态。通用原则:**MCP 工具只做不改变链路的事**(读 / 注入 / 观察);会改变链路的(改网、重启、OTA)要么走别的通道,要么做成先应答后执行 |
| LED 语义(2026-10-23 定调) | **一个 int 状态码**,`status` 里就是 `"led": 0` —— 不带颜色、不带文案、不带子状态;颜色↔数值的对应只写在文档里,人和 agent 各自查同一张表。**0 就绪**(已联网 + MCP 在服务,🟢 常亮)/ **1 启动中**(刚上电还没开始连网,🔵 快闪)/ **2 网络未就绪**(在连/没配网/掉线重连,🟡 慢闪)/ **3 动作执行中**(收到 `input`,执行期间**持续亮**,🔵 常亮)/ **4 故障**(上次注入自检失败,"手不灵",🔴 常亮)。优先级 3 > 4 > 1 > 2 > 0。**三条约束**:① 只放**设备自己测得到**的状态(dongle 插着才有电,不存在"未插"这种态)② 只放**现场的人会据此行动**的状态 ③ **JSON 里只给一个 int**。以后加一档要同时满足这三条 |
| 设备端 HTTP 的定位(2026-10-23 定调) | **MCP over HTTP+JSON 是被 agent 生态逼出来的兼容层,不是产品目标** —— agent 只支持 MCP,快速实现比"做一个完整 HTTP 服务器"重要。所以设备端**刻意保持最小**:不追求完整 HTTP 服务器实现,**不追求并发**。**单例模式**:设备端**一次只执行一个动作**;**有动作在执行时,新的动作请求直接返回 `busy`**(不排队执行、不排队等结果),`BUSY` 沿用既有命令状态词表。**已实现(P1-B,2026-10-23)**:长操作(`macro` / 长文本)移到 **worker task**,httpd task 一直能应答 ⇒ busy **物理上立刻可达**(实测 84–412ms);发起方**不等结果**,结果由盘上标记文件回读(不需要 job 状态机/轮询端点);**读类工具在注入期间照常服务**(注入时"眼"是睁着的) |

## 后期实现项(2026-10-23 记下,以后实现)

| # | 项目 | 现状 / 要点 |
|---|---|---|
| P1 | ✅ **已做**:单例 + `busy` 语义落地(2026-10-23) | 长操作(`input` 的 `macro` / 超过 16 字符的 `type`、以及 `update` 写 flash)走**单例槽**;忙时新的动作请求**立刻回 `busy`**。长操作移出 httpd task(worker task),httpd task 一直能应答 ⇒ busy 物理上可达;发起方不等结果,结果由盘上标记(`INJ.TXT`/`CAP.TXT`)回读。**没有** job 状态机/轮询端点。实测:受理 119–300ms、busy 应答 84–412ms、动作期间读请求 53–225ms、槽 10.7–14.9s 释放。**顺带修掉一个真 bug**:读 body 的 `continue` 没上限 → 客户端发一半 body 就静默会把 httpd 唯一的 task 永久卡死(症状:设备还活着但新连接被拒) |
| P2 | ✅ **已做**:请求路径硬化(2026-10-23) | ① body 上限 **128 KB**(超了回 413)——最大的合法请求是 `update`(64 KB 原字节 → base64 约 87 KB),上限的意义是**不让 `content_len` 变成无界的 malloc**(httpd 只有一个 task)② `Content-Type` 不是 `application/json` 回 **415** ③ `Origin` + `Host` 绑定设备自己(当前 IP / 设备名 / 设备名.local)——**DNS rebinding** 防护,调用方是 native agent 永不发 `Origin`,所以带 `Origin` 且不是我们就拒。`/files/*`(`/ping` 也一样(前者装的是桌面帧)。**拒绝前先抽干 body**:服务端带着未读请求体关连接会发 RST,客户端就只看到"连接被重置"而不是状态码。验收:`tools/mcp_hardening_probe.py` **17 项全绿** |
| P3 | 设备侧鉴权 | 明文 HTTP 下**局域网任意主机当前都能调 `input`**(注入是破坏性操作)。用 PSK token 头;与 TLS-PSK 双向鉴权一起做 |
| P4 | ✅ **已做**:协议细节(2026-10-23) | ① **协议版本协商**:支持 `2024-11-05` / `2025-03-26` / `2025-06-18`;客户端 `initialize` 报什么我们支持就**原样回**,不支持就回最新的 `2025-06-18`(由客户端决定要不要继续)。initialize 之后的请求带 `MCP-Protocol-Version` 头,值不认识直接回 **400** ② **去掉假的 session id**:以前硬编码 `Mcp-Session-Id: ioagent-1`,我们是**无状态**服务,回一个假 session id 会让客户端以为有会话要维持 —— 现在**根本不发这个头**,通知按规范回 **202 + 空 body** |
| P5 | TLS-PSK 双向鉴权(M5 收尾) | **mDNS 发现已于 2026-10-23 落地**(见 M6 行),本项只剩 TLS-PSK。明文 HTTP 下局域网任意主机当前都能调 `input` |
| P6 | 不依赖 IME 的 bootstrap 通道 | IME 强制序列(`Ctrl+Space` + `Shift`)**只对微软拼音成立**,其它输入法未验证;且 **agent 不在目标机上时无法回读 IME 状态**(回读靠 agent 或 watcher,watcher 又要靠注入拉起 → 循环)。需要一条"不经过 IME 的引导命令"(如只含符号的 Alt+numpad 序列,或换一条不走文本框的通道) |
| P7 | 注入命令的落地方式 | 当前命令走 **Win+R 文本框**(要处理 IME、历史预填、运行框行为)。长期更稳的做法:命令先由设备落到盘上,目标机侧用**一次性启动器**执行,输入面尽量小 |
| P8 | 主机侧 watcher 的并发写 | 设备读盘(`read`/`capture`)与宿主写帧撞车会让写失败 → watcher 曾"跑两帧就死"。已加**重试 + 只有卷目录消失才退出**,并发是否彻底消除待观察 |
| P8b | watcher 存活信号的假阴性 | `status.watcher.running` 靠盘上的 `CAP.TXT` 判定 watcher 在不在,但 **CAP.TXT 是 watcher 启动时写一次的** —— 设备重启后 RAM disk 重建 → CAP.TXT 没了 → `status.watcher.running` 报"没在跑",**而 watcher 其实还在跑**(它只在启动时写 CAP.TXT)。需要一个不依赖一次性文件的存活判据(如帧序号是否在推进)。**注**:`CAP.TXT` 保持"存在即算活"是有意的 —— watcher 只在启动时写它,若改成"必须新鲜"反而会把"watcher 本来就在跑"误判成失败。但**注入自检的 `INJ.TXT` 是逐次回执**,判据必须是"这次写的"(见 P13) |
| P9 | Windows 旧设备实例 | `pnputil /remove-device` 报 Access denied(需管理员),旧的 Error 实例仍在(不影响新实例) |
| P10 | 协议层主机侧单测 | 目前没有本地 unit test,每次验证都得烧板或跑实板脚本 |
| P11 | ✅ **已修并真机验证(2026-10-24)**:读已存在文件曾让设备 panic 重启 | 2026-10-23 由 skill eval 的真实 agent 撞出来:读一个**存在**的文件(哪怕是宿主刚写的 21 字节文件)→ 设备 `Guru Meditation / LoadProhibited`(EXCVADDR=0x45,回溯被冲垮 ⇒ httpd 任务栈被写坏)→ **软件复位重启** → MCP 端点整个消失;读**不存在**的文件则 0.2 秒正常返回。httpd 任务栈是 IDF 默认的 **4096 字节**,而 handler 链(加固缓冲 + cJSON + 读盘)都在这一个栈上。**修法**:栈提到 8192、加固缓冲挪到 static、读盘缓冲也挪到 static、慢读打点。**验证**:回归脚本 `tools/read_hang_probe.py` —— 宿主现写的 21 B 文件、槽位里的 134 B 文件都是 0.2 s 内正常返回,设备存活、串口无崩溃日志。⚠ 期间本机的 Windows Code Integrity 拦了 Espressif 工具链的 `objdump.exe`(IDF 生成链接脚本那一步要调它),**关掉 Smart App Control / UAC 后已解封**,不需要改 IDF 工具链 |
| P12 | ❌ **否决(2026-10-24)**:设备端 `wait` 工具(阻塞等动作跑完) | 设计做完了、评估后**不做**。它要解决的问题是"agent 拿长动作终态要轮询 9~31 次 `status`"。**否决理由(三条,都是架构级的)**:① **它要独占设备唯一的 httpd task 最长 20 秒** —— P1-B 刚为了"注入时眼是睁着的"把长动作挪到 worker task,`wait` 等于把那个洞挖回来;而且两个 task 优先级相同(都=5),等待循环**必须 `vTaskDelay` 让出 CPU**,忙等会饿死 worker → 动作永远跑不完 → 自己把自己锁死。② **给已稳定的 6 工具契约加第 7 个**,每个新工具对未来的 MCP 客户端/skill 都是一份兼容面。③ **它省的事不该由设备负责** —— "少写几次轮询"是客户端的事。**替代路径(已落地,零固件改动)**:轮询留在 agent 侧,设备端契约一个字都不动 —— skill 的 `mcp_call.py` 加了 `--wait`(等 `busy` 转 false)和 `--wait-file <名>`(等某个文件出现),打印 `waited_s` / `timed_out`,**对 agent 仍是 1 步**,而设备全程照常服务别的请求(这正是设备端 wait 给不了的)。**注**:不引入 job 状态机/轮询端点仍是硬约定(见 `docs/implementation-plan.md` 第 17 行),`status.busy` 就是给 agent 轮询用的 |
| P13 | ✅ **已修并真机验证(2026-10-24)**:注入自检曾会"假 PASS" | `INJ.TXT` 同一次开机内一直留在盘上,而 `inject_selftest()` 只判**存在性** ⇒ **第二次自检会在 1 秒后直接报 PASSED,哪怕这次注入根本没到达目标机**。这正好打在"IME 态探针"上:探针的职责是判定当前是中文态还是英文态,判错 ⇒ 后面打的全是乱码,而且**看起来一切正常**。**修法**:注入前记下 `INJ.TXT` 的最后写入时间,要求**变了**才算数(一次自检 ~7 秒,天然避开 FAT 的 2 秒时间戳粒度)。**前置依赖**:这一步要靠 mtime,所以顺带修了一个更隐蔽的 bug —— mtime 的目录项偏移写错了(读成 `0x24/0x26`,越过 32 字节读到**下一个条目**),解出来是 1995 年的合法日期,而"挑最新一帧"正是按 mtime 排的 ⇒ **挑帧逻辑一直是错的**;已修为 FAT16 的 `0x16/0x18`。回归 `tools/mtime_probe.py` **4 项全绿** |

## 已知硬约束

- S3 的 USB-OTG 与 USB-Serial-JTAG 共享一个 PHY:启用 HID 后原生串口调试口消失,日志走 UART0。
- **USB 拔线检测不可用**:ESP32-S3 用的是 Synopsys DWC2 内核,TinyUSB 的 DWC2 device 模式
  **没有实现设备断开检测**,`TINYUSB_EVENT_DETACHED` 永远不触发;拔线只表现为总线 SUSPEND。
  → "主机还在不在"只能看 `tud_ready()`(mounted && !suspended),详见决策表「USB 链路活性判据」。
  连带影响:M2 注入链路的中止条件不能挂在 DETACHED 事件上。
- **端点预算**:S2/S3 控制器共 7 个端点(含 EP0)、最多 5 个 IN(含 EP0)→ 6 个可配置。
  **实测占用 3 个**(EP0 + `0x81` HID IN + `0x02` MSC OUT + `0x82` MSC IN,IN 侧 2 个),
  富余 IN 2 / OUT 3;**USB CDC 塞不下**(需 3 个)→ "USB 串口当数据回传"没有退路。
- **flash encryption / secure boot 会禁用 ROM 里的 USB-OTG 栈** → 本项目 USB 口就是产品主接口,
  量产固件**必须走 WiFi OTA**;开发期不开加密(否则每次更新都要全量擦除)。
- LED 通道约 3–9 bit/s,只做 ACK/心跳,不做数据。
- macOS 多数版本不同步下发 LED 报告 → LED 通道探测失败时必须有降级分支。
- FAT 无 FIFO 语义,`echo > file` 是 fire-and-forget → 回执走 RESP 文件(LED 已退出主路径)。
- ESP32 在 FAT 上主动建文件易损坏 → ESP32 只读文件,写回交给主机。
- ESP32 不能向主机发起任何数据(USB 主从 + FAT 缓存)→ 配网/绑定只能"主机推送到盘上"。
- **RAM disk 每次上电恢复出厂镜像**:内容随断电消失。dongle 形态拔线即断电 →
  任何运行时下发到 RAM 的内容都会丢,**需要留存的东西必须落 flash**(`script0`/`script1` 仓)。
- **重入后盘符不保证复用**:dongle 重入是冷启动,Windows 可能分配到别的盘符 →
  宿主侧按**卷标**定位(`IOAGENT`),设备侧注入的启动命令也按卷标解析(见决策表「watcher 启动」)。

## 目录

- `docs/glossary.md` — **术语表**(读其它文档前先扫一眼)
- `docs/agent-interface-mcp.md` — **Agent 接口层:手眼模型**(act/observe 原语、N1-N7 裁定、UAC 补充、采集方式定案)
- `docs/output-channel-design.md` — Output 通道设计 v2(三层架构、延迟预算、屏幕即接口模式)
- `docs/connectivity-and-binding.md` — **连接与绑定 v3:设备即 MCP 服务端**(拓扑、鉴权方向、配网、MCP on-device 可行性与风险、连带作废清单)
- `docs/hardware-selection.md` — 硬件选型与形态(N16R8 锁定、三阶段板形、供电/天线/日志策略)
- `docs/implementation-plan.md` — **实施计划**(仓库骨架、里程碑、契约冻结清单)
- `docs/design-review.md` — 2026-10-20 设计评审:8 条发现与定案结论(含当日拓扑反转)
- `docs/msc-watcher-protocol.md` — v1 文件协议,回读部分已被 v2 取代;CMD/RESP 约定平移至 v2,**LED 段落已作废**
