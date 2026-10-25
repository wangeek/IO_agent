# 实施计划 v1(2026-10-20)

从"设计定案、零行代码"到"可验收的一代机"。本文是施工顺序与契约冻结清单。
设计依据见 README 决策表 + output-channel-design.md + agent-interface-mcp.md +
connectivity-and-binding.md。

## 一、仓库骨架

```
Sophon/
├── README.md                     架构决策表(总纲)
├── docs/                         设计文档(已存在)
│   ├── glossary.md               术语表(先读这个)
│   ├── output-channel-design.md  Output 通道 v2(三层/延迟预算)
│   ├── agent-interface-mcp.md    Agent 接口层:手眼模型
│   ├── hardware-selection.md     硬件选型与形态
│   ├── connectivity-and-binding.md 配网/绑定/回传(本文新增)
│   ├── msc-watcher-protocol.md   v1 存档(仅 CMD/RESP 约定仍生效)
│   ├── design-review.md          2026-10-20 评审结论与定案
│   └── implementation-plan.md    本文
├── firmware/                     ESP-IDF 工程(idf.py create-project 生成,勿手写骨架)
│   ├── main/                     应用入口
│   ├── io_usb/                   TinyUSB 描述符:复合设备(HID 键盘/鼠标 + MSC)
│   ├── io_disk/                  PSRAM FAT16 镜像 + MSC 后端
│   ├── io_payload/               T2 flash payload 仓(script0/script1 A/B + CRC32 + 信箱 ingest)
│   ├── io_hid/                   注入序列执行器(本地定时,<10ms/步)
│   ├── io_launcher/              注入链路(拉起目标机 watcher;默认**关**)
│   ├── io_cfg/                  NVS 配置(WiFi 凭据/PSK/设备名)+ UART0 配网控制台
│   ├── io_net/                  WiFi STA + 退避重连 + mDNS responder(TCP/IP 线程内装配)
│   ├── io_mcp/                  MCP server(HTTP + JSON-RPC 2.0,5 个工具)
│   ├── capability/               能力注册表(编译期开关 + 运行时查询)
│   └── components/               vendored 第三方组件(esp_tinyusb / tinyusb)
│                                 + README-vendor-patches.md(补丁记录)
├── watcher/                      目标机侧(零安装,驻 RAM disk)
│   ├── watch.ps1                 驻守主脚本
│   ├── capture/                  采集抽象层(当前 GDI,后续可插拔 DXGI)
│   └── assets/                   预置脚本与工具(打进固件分区或 RAM disk 镜像)
├── tools/                        构建与验收脚本(纯 ASCII .ps1 / .py)
│   ├── idf.ps1                   固化构建环境(idf.py 的薄封装)
│   ├── setup_idf.ps1             IDF 主树 + 工具链安装
│   ├── fetch_idf_submodules.py   IDF 子模块离线抓取 + 逐文件 SHA 校验
│   ├── mkfatimg.py               FAT16 镜像生成(设备端零 FAT,正确性在此保证)
│   ├── verify_m1.ps1             M1 主机侧验收(枚举 / 卷定位 / 读写往返 / 端点证据)
│   ├── verify_t2.py              T2 端到端验收(下发 → 落 flash A/B → 复位 → 槽位内容一致)
│   ├── verify_unplug.ps1        M1 拔插健壮性验收(串口日志 + PnP 状态同步采样)
│   └── check_proxy_integrity.py 本机代理损坏诊断
├── tests/                        确定性自检(不依赖人工操作)
│   └── test_watcher_diff.ps1     watcher diff 采样与合并逻辑
└── host/                         宿主侧(你的 Mac/服务器)
    ├── client/                   MCP 客户端与联调工具(发现 / 配对 / 取帧)
    └── adapter/                  选做:设备端 MCP 做不到时的薄协议转换器
```

约定:

- 固件端**只搬运,不解析内容**(哑设备原则,N2 延续)。
- watcher 端**有像素,所以 diff/ROI 在这里算**(N1/N2 延续)。
- 所有跨端契约(MCP 协议、盘上文件名、错误码)以本文 §四 为准,改一处必须同步四处。

## 二、里程碑

### M0 环境与骨架
- 建仓、目录落位、ESP-IDF 工程生成、模块边界定死(不写业务)。
- 验收:`idf.py build` 通过;目录结构与本文一致。

#### M0 实测结果(2026-10-22 ~ 10-24)

| 事项 | 结论 |
|---|---|
| ESP-IDF | v5.5.6,装在 `D:\Espressif\frameworks\esp-idf`(commit `880a203a`);`IDF_TOOLS_PATH=D:\Espressif` |
| 工具链 | xtensa-esp-elf gcc 14.2.0 / cmake 3.30.2 / ninja 1.12.1;IDF venv Python 3.11.14 |
| 板子确认 | esptool 实测:ESP32-S3 rev v0.2 / Flash 16MB / Octal PSRAM 8MB / MAC `30:ed:a0:15:b1:dc` → **N16R8** |
| 固件尺寸 | `sophon.bin` **884,176 B**(0xD7DD0);app 分区 **2MB**,实占 **884,176 B**,**剩余 1,212,976 B(57.8%) free**;131 个编译目标。**2026-10-22 瘦身**:内嵌 RAM disk 镜像改稀疏(4MB → 49,664 B)+ 槽位化(簇内容不进镜像,槽位容量零成本)后从 4,420,880 B 降下来 |
| 组件策略 | **vendored 组件 + 关闭组件管理器**(`IDF_COMPONENT_MANAGER=0`)。代价:必须补齐**全部** IDF 内置组件的子模块 —— 早展开阶段 include 每个已注册组件的 CMakeLists,缺一个即 FATAL_ERROR |
| 子模块 | 23/23 落盘,**双通道校验全通过**:9 个用 GitHub git-tree API 逐文件 blob SHA-1 比对,14 个用两次独立下载 sha256 比对 |
| vendor patch | 3 处,全部记在 `firmware/components/README-vendor-patches.md`:①MSC 解耦 ②tinyusb 组件名解析 + script-mode 保护 ③`tusb_config.h` 归位 tinyusb 组件 |
| 构建环境封装 | `tools/idf.ps1` 固化 `IDF_PATH` / `IDF_TOOLS_PATH` / venv / 工具链 PATH / `ESP_ROM_ELF_DIR` / `IDF_SKIP_CHECK_SUBMODULES=1` |
| 编码坑 | `idf.py` 把 cmake 输出经 Windows 控制台 codec 再编码,中文触发 `UnicodeEncodeError: 'gbk'` → 构建在成功的 configure 之后崩掉。修法:Python/控制台双端 UTF-8(`PYTHONIOENCODING=utf-8` + `PYTHONUTF8=1` + `[Console]::OutputEncoding=UTF8`)+ CMake `message()` 文案改 ASCII。**固件日志一律 ASCII**:串口中文在 Windows 控制台链路中被破坏成 `????` |
| 网络策略 | 本机代理 `127.0.0.1:7897` **间歇性重置连接**:pip/wheel 绕过代理走清华直连;**git clone 走代理不可靠**(`early EOF` / `schannel: server closed abruptly`);改用 **codeload tarball + curl 断点续传**,完整性靠「git-tree API 逐文件 blob SHA」或「两次独立下载 sha256」,不靠 git 自身哈希 |
| 下载器坑 | `curl -C -` 对失败残留的半截文件会拿到 200 而非 206 → curl 33 死循环。修法:无进展即丢弃残file重来,`returncode==33` 时清盘 |

### M1 固件最小可枚举
- TinyUSB 复合设备:**HID 键盘 + MSC**;PSRAM 内构建 FAT16 镜像(4-6MB 设计值)。
- **镜像用稀疏格式(2026-10-22)**:卷容量由 BPB `total32` 声明(4MB),镜像文件只写到
  最后一个已用扇区(97 扇区 / 49,664 B),固件 `io_disk_init()` 按 `total32` 在 PSRAM
  开满卷并把尾部补零。宿主看到的仍是完整 4MB 卷,设备端依然零 FAT 逻辑。
  → 内嵌镜像 4MB → 48KB,`sophon.bin` 4.42MB → **863KB**,OTA 槽随之 6MB → 1MB。
- **槽位化(2026-10-22)**:出厂镜像不再内嵌 `watcher/watch.ps1`,只留 3 个设备管理槽
  (`WATCH.PS1` 32K / `AGENT.SH` 32K / `AGENT.EXE` 64K)+ 1 个宿主信箱(`PAYLOAD.BIN` 128K)。
  槽位簇**从卷末尾往前分配**,簇链与目录项写进镜像、**簇内容不写进镜像** → 镜像停在
  元数据区,固件按 `total32` 补零尾部 ⇒ **槽位容量在固件里占 0 字节**。
- 验收:Windows 设备管理器同时出现"键盘"与"可移动磁盘";盘可写可读;拔插不崩;
  **端点占用核对**(见 `hardware-selection.md` §三):IN ≤ 4(不含 EP0)、总占用 ≤ 6,
  富余端点数与方案表一致。
- 风险点:FAT16 BPB 参数必须正确,否则主机不认卷(会提示"需要格式化")。

#### M1 实测结果(2026-10-24,N16R8 实板)

| 验收项 | 结果 | 证据 |
|---|---|---|
| 复合设备枚举 | 通过 | `USB Composite Device USB\VID_303A&PID_8001\30EDA015B1DC` |
| 键盘 | 通过 | `HID\VID_303A&PID_8001&MI_00&COL01` → Class=`Keyboard`,FriendlyName=`HID Keyboard Device` |
| 鼠标 | 通过 | `HID\VID_303A&PID_8001&MI_00&COL02` → Class=`Mouse`,FriendlyName=`HID-compliant mouse` |
| MSC | 通过 | `USB\VID_303A&PID_8001&MI_01` → Class=`USB`,`USB Mass Storage Device` |
| 单 HID 接口被主机拆成键盘+鼠标 | **通过(原顾虑推翻)** | 同一个 `MI_00` 接口下 Windows 按报告描述符开出 COL01/COL02 两个集合,说明 Report ID 1=键盘 / 2=鼠标 的复合报告描述符被正确解析 |
| 卷挂载 | 通过 | `E:` 卷标 `SOPHON`,FileSystem=`FAT`,DriveType=`Removable`,可见容量 4,144,640 B,空闲 4,123,648 B |
| 磁盘节点 | 通过 | `Get-Disk` #1 `SOPHON Sophon Disk`,BusType=`USB`,Size=4 MB,IsReadOnly=False,位置 `Integrated : Bus 0 : Device 0 : Function 2 : Adapter 0 : Port 0` |
| 盘可写可读 | 通过 | `tools/verify_m1.ps1` 第 3 步:写 27 B → 读回 27 B → **逐字节一致** → 删除成功 |
| 主机挂载时写盘 | 观察到的正常行为 | 固件侧统计 `written=25 blocks / 12800 bytes`(Windows 挂载 FAT 卷时的写入) |
| 端点占用核对 | 通过 | 见下表 |
| 拔插不崩 | 通过 | `tools/verify_unplug.ps1`:实测 `PRESENT → ABSENT → PRESENT` 完整循环,固件**无复位**(`rst:0x` 未出现,uptime 连续),重新枚举成功 |
| 链路掉线/恢复可观测 | 通过(修 bug 后) | 固件状态行 `link=0 mounted=1 suspended=1` → 重新插上后 `link=1 mounted=1 suspended=0`,跃迁日志 `host link lost` / `host link restored` 均有 |

**拔插实测原始日志(2026-10-24,第二次跑,判据改用链路活性)**:

```
[4.42s]   device PRESENT          <- 主机侧 PnP
[24.33s]  W sophon: host link lost (mounted=1 suspended=1) -- cable removed or host suspended
[26.33s]  I sophon: status: link=0 mounted=1 suspended=1 enumerated=1 hid_ready=0
[27.93s]  device ABSENT           <- 主机侧 PnP
[39.78s]  I io_usb: host enumerated and configured (rhport=0)
[39.92s]  I sophon: host link restored (mounted=1 suspended=0)
[41.88s]  device PRESENT          <- 主机侧 PnP
```

判据:`tools/verify_unplug.ps1` 五项全绿(ABSENT 出现 / 链路掉线被记录 / 链路恢复被记录 /
重新枚举被报告 / 全程无 MCU 复位)。

> **本轮修掉的真 bug**:之前固件用 `TINYUSB_EVENT_DETACHED` 判断"主机还在不在",
> 但 ESP32-S3 的 USB 是 Synopsys DWC2 内核,TinyUSB 的 DWC2 device 模式**不实现设备断开检测**
> (`firmware/components/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c` 里
> `GINTSTS_DISCINT for disconnect detection` 仍是 TODO;唯一的 `DCD_EVENT_UNPLUGGED`
> 来源 `GOTGINT_SEDET` 需要 OTG + VBUS 感知,devkit 没接)。
> 结果:物理拔线只上报 **SUSPEND**,`TINYUSB_EVENT_DETACHED` **永不触发** →
> 固件的 `host_configured` 标志在拔线后是**过期的**(撒谎说还连着)。
> 实测同时证明 `tud_mounted()` 也不能用(拔线时 `mounted` 仍为 1,因为没有 UNPLUGGED
> 就没有 `cfg_num` 清零)。**唯一可靠判据是 `tud_ready()` = `mounted && !suspended`**;
> 恢复靠重新插上后主机发 SOF,SOF 处理器会清掉 `suspended`(实测 140 ms 恢复)。
> 判据与影响见 `docs/hardware-selection.md` 决策摘要第 7 条。

**附带的端到端验证(watcher → ESP32,2026-10-24)**:watcher 在本机直接对真实 E: 盘
(ESP32 的 PSRAM RAM disk)跑通,盘上落盘:

| 文件 | 实测大小 | 说明 |
|---|---|---|
| `CAP.TXT` | 97 B | 能力自陈 `{"os":"Win32NT","arch":"AMD64","admin":false,"net":true,"screen":"800x600","watcher_ver":"0.1.0"}` |
| `SNAP_P.JPG` | 95,232 B | 预览帧,实测像素 **960×540** |
| `SNAP_R.JPG` | 53,794 B | 变化 ROI 裁剪帧,实测像素 **427×285**(与 DIFF.IDX 包围盒精确吻合) |
| `SNAP.IDX` | 220 B | `{"name":"SNAP_P.JPG","len":95232,"seq":5,"cursor":[400,300],"roi":{...}}` |
| `DIFF.IDX` | 146 B | `{"seq":5,"grid":[60,34],"rects":[...]}` |

- **固件侧写盘统计同步上涨**:`written=25 blocks` → **`written=2355 blocks / 1,205,760 bytes`**
  (每块 512 B 精确对齐)→ 帧确实经 MSC 落进了 ESP32 的 PSRAM,不只是主机侧缓存。
- **ROI 坐标链路核过**:网格 60×34 → 预览像素矩形 → 按 `scaleX/scaleY` 映射回**全分辨率** →
  从全分辨率抓屏图裁剪。`SNAP.IDX` 里的 rects 是**全分辨率空间**(与 `docs/implementation-plan.md` §4.1
  "全分辨率 ROI" 一致),不是预览空间。
- **diff 边界钳制正确**:`Get-BlockHashes` 用 `yMax = Min(y0+Block, Height)-1` 夹住最后一行,
  34 行 × 16 = 544 > 540 不会越界读;`Get-RoiBytes` 对包围盒做 `Min` 钳制,不会裁出界。

**端点占用核对(冻结)**

| 端点 | 方向 | 用途 | 来源 |
|---|---|---|---|
| EP0 | 双向 | 控制传输 | USB 规范固定 |
| 0x81 | IN | HID 报告(键盘/鼠标) | 固件运行时日志 |
| 0x02 | OUT | MSC 数据出(主机→设备) | 固件运行时日志 |
| 0x82 | IN | MSC 数据入(设备→主机) | 固件运行时日志 |

占用统计:**IN = 2**(0x81 / 0x82)、**OUT = 1**(0x02)、含 EP0 **总占用 = 3**。
限额为 IN ≤ 4 / 总占用 ≤ 6 → **富余 IN 端点 2 个、OUT 端点 3 个**,与"富余端点留 vendor bulk(M2/M3)"的方案一致。

### M2 注入链路
- HID 键盘/鼠标注入;**动作序列本地定时执行**(整条序列在**一次** HTTP 请求内跑完,
  禁止逐条 WiFi 往返;实测标定的单步时长:组合键 600ms、单键/逐字符 60ms)。
- `focus_reset` 宏(2026-10-23 实测重定案):**`Win+M` → `Win+R` → `Ctrl+Space` → `Shift` → `Ctrl+A`**,每步之间 500ms。
  - **keycode 是 HID 用法 ID,不是 ASCII**:传 `'R'`(0x52)会按成小键盘的键 —— 组合键从来没生效过
    (`io_hid_keycode_for_letter()` 负责字母 → keycode)。
  - **组合键按住 ≥600ms**:30ms 会被 Windows 整段丢弃(实测 `Win+R` 打不开运行框)。
  - **单键按住 ≥60ms**:8ms 时微软拼音处理不过来,输入被打成乱码(`ni hao ` → 49 个"啊")。
  - **IME 必须强制且状态无关**:`Shift` 是状态相关 toggle(中→英 或 英→中);
    `Ctrl+Space`(开/关输入法)+ `Shift`(中/英切换)两步走,三种初始态(开+中/开+英/关)实测终点都是纯 ASCII。
  - **运行框会预填历史命令**(且不是选中态)→ 必须先 `Ctrl+A` 全选,否则命令被追加成废命令。
- **注入的启动命令按卷标解析,禁止写死盘符**(2026-10-22 定案,见
  `output-channel-design.md` §四 L1)。dongle 拔线即整机断电,重入是冷启动,
  Windows 可能分配到别的盘符 → 写死 `X:\` 的命令在重入后必然失效。
  命令形态:`cmd /c for %d in (I O A G E N T) do @if exist %d:\WATCH.PS1 start "" powershell -ExecutionPolicy Bypass -WindowStyle Hidden %d:\WATCH.PS1`
  → 纯 cmd 内建语法,无 `Get-Volume` 之类的可选模块依赖。
- 验收(2026-10-23 已过):① 注入含 Shift/特殊字符(`% \ > @ ( ) _ .`)的整条命令,
  在目标机**执行成功**(设备在盘上看到 `INJ.TXT` = `INJECT_OK`)② 探针文本在运行框里
  **逐字节回读一致**(纯 ASCII)③ 单步时长按实测标定(组合键 600ms / 逐字符 60ms),
  **不是 <10ms**——30ms 的组合键会被 Windows 整段丢弃,8ms 的逐字符会被 IME 打成乱码。
- **远程自检宏 `macro=selftest`(2026-10-23 新增)**:注入一条把标记写回盘的命令,
  设备以盘上是否出现 `INJ.TXT` 判定"HID 真的到达了目标机"(`marker_seen`)。
  设备自己看不到目标机屏幕,这是唯一能区分"命令发出去了"和"命令执行了"的客观判据。

### M3 watcher 通路(目标机侧)
- HID 敲 `Win+R` → 注入**按卷标解析**的启动命令(见 M2 / `output-channel-design.md` §四 L1)拉起驻守。
- **拉起成功判定走闭环**:设备注入后轮询 RAM disk 里的 `CAP.TXT`,出现即判定成功;
  未出现则 2s 间隔重试注入,最多 5 次。这条同时给 agent 提供 watcher 存活信号。
- 盘符自扫(不假设 X:);`SNAP_P`/`SNAP_A` + `SNAP.IDX` 双缓冲防撕裂;
  `CMD\` / `RESP\` 命令与响应;幂等键沿用 v1 约定。
- **进程自检退出**:载体丢失(**卷目录消失**)即判定失联 → 主动退出,不残留。
  写盘**失败**但卷还在(设备正在读盘导致的撞车)→ **先重试**(最多 5 轮),
  不再一失败就退(2026-10-23 实测:曾出现"跑两帧就没了")。
  (代价:dongle 重入后 watcher 已死,必须靠设备重新拉起 —— 上面的闭环就是为此。)
- 验收:拔盘 5s 内进程自行退出;连续 2fps 取帧 10 分钟,无花屏帧入库。

### M4 连接链路(设备即服务端)
- 设备起 WiFi + mDNS 广播(`_ioagent._tcp.local`);凭据预置(NVS)或 SoftAP 配置。
- 设备端 TLS-PSK 服务端 + 配对流程(一次性配对码 → PSK)。
- host 侧 MCP 客户端:发现设备 → 配对 → 调 `capture` 取帧。
- **双核分工**:core 0 专职 USB/TinyUSB + HID 注入(保 <10ms/步 时序),
  core 1 跑 WiFi + TLS + HTTP/MCP(TLS 传 300KB 帧约 20-60ms CPU,单核会抖注入时序)。
- **帧零拷贝**:直接从 RAM disk 分块读 → 直接写 socket,不在 PSRAM 另存 300KB 副本。
- 验收:插入 → 无人工 → **10s 内 host 侧拿到第一张桌面截图**;
  稳定态随机 20 次全屏截图全部 ≤1s、成功率 100%。

### M5 MCP 面(跑在设备上)
- 设备端 MCP server(Streamable HTTP + JSON-RPC 2.0),工具共 **7 个**:
  `status` / `ls` / `read` / `capture` / `input` / `update` / `flash`。
  命名规则:**全是动词,全小写单词,零分隔符**(名称绑定层的 `ioagent` 不改,只改人读的对外契约)。
  `update` 是 **2026-10-22 增的 T2 工具**(运行时下发引导内容,见
  `agent-interface-mcp.md` §二之二)。
  `flash` 是 **2026-10-25 增的整机固件 OTA 工具**(单 Type-C dongle 只有一个口,
  插在目标机上做 HID+MSC ⇒ 开发机烧不进去了,固件更新必须走网络;见 §四之四)。
- 新鲜度:取帧时可按 `mtime` 判新旧(盘上目录项带时间戳),当前实现是取**最新一帧**。
- 帧用 MCP 原生 **image 内容块**返回(小于 64 KB 内联 base64);超过的只给 `GET /files/<name>` 的 URL。ROI 与 diff 清单在 agent 侧做。
  **内联阈值 64 KB 不要下调** —— 帧几乎都超过 8 KB,砍到 8 KB 会让 image 块直接消失(契约降级)。
- `GET /files/<name>` 用 **chunked 分块发**(读一块发一块):几百 KB 的桌面帧一次 send
  塞不进 lwIP 的发送缓冲,会等 `SO_SNDTIMEO` 等满 5 秒然后失败。
- 状态诚实:`capture` 无帧时返回 `isError:true` + 明确原因,不误报。
- 验收:`input → capture → 决策` 闭环全链无人工、无 host 侧对目标机的软件依赖。

### M6 形态与边界
- 形态验证板(USB-A 直插)完成三合一枚举、机箱后面板供电、天线遮挡实测。
- UAC 盲过阶梯落地(默认盲过;不默认改注册表)。
- 边界诚实标注:锁屏/安全桌面无画面、SAS 不可注入、EDR 环境一代不覆盖。

## 三、定案结论的落地条款(2026-10-20 评审)

| # | 定案 | 落地要求 |
|---|---|---|
| 1 | **采集走 GDI(CopyFromScreen),游戏/独占全屏后置** | watcher 的采集封装为 `capture` 抽象层,当前实现 GDI;DXGI Desktop Duplication 后续插入同一接口,不动协议与固件。文档如实标注"真独占全屏可能黑帧"为已知边界 |
| 2 | **分层快照,带宽按变化量收敛** | 见 §四.2 文件与帧率约定;USB 写入稳态 <300KB/s,CMD/RESP 不被饿死 |
| 3 | **拓扑反转:设备即 MCP 服务端** | 见 connectivity-and-binding.md v3;agent 主动连设备、mDNS 发现、TLS-PSK 双向鉴权;原"双路回传"与"目标机代取配置"作废 |
| 4 | **LED 通道退出主路径** | Stage 0 不以 LED 成功为前提;LED 仅作可选调试/心跳;v1 协议文档中 LED 段落标记作废 |
| 5 | **拔盘无残留靠显式自检,不靠"自然退出"** | watch.ps1 写盘失败即退出;验收清单加入"无残留进程" |
| 6 | **安全基线平移进实施清单** | op 白名单 / type ≤256B / 鼠标 clamp ±127 / 键盘基线恢复 / 检测到人动键盘即中止 —— 写进 M3 验收条目 |
| 7 | **文档与仓库卫生** | README 目录不指向不存在的文件;仓库纳入版本控制 |
| 8 | **RAM disk 出厂镜像用稀疏格式(2026-10-22)** | 卷容量由 BPB `total32` 声明(4MB),镜像文件只写到最后一个已用扇区;固件按 `total32` 在 PSRAM 开满卷并补零尾部。宿主看到的仍是完整 4MB 卷,设备端零 FAT 逻辑不变。**理由:AI 现场就能生成的启动文件不该占固件位置** |
| 9 | **Flash 分区重排(2026-10-22)** | `ota_0`/`ota_1` 6MB → **2MB**(app 实占 884,176 B,余 1,212,976 B(57.8%));新增 `script0`/`script1` 各 512KB 作 T2 运行时 payload 仓(A/B);余 10.9MB 留白 |
| 10 | **注入的启动命令按卷标解析(2026-10-22)** | 禁止写死盘符(`X:\`)。dongle 拔线即整机断电,重入是冷启动,Windows 可能分配到别的盘符 → 写死盘符的命令在重入后必然失效。命令按卷标枚举候选盘符,`if exist` 命中即启动 |
| 11 | **出厂镜像槽位化 + T2 flash 仓落地(2026-10-22)** | 出厂镜像不再内嵌 `watcher/watch.ps1`,只留 3 个设备管理槽(`WATCH.PS1` 32K / `AGENT.SH` 32K / `AGENT.EXE` 64K)+ 1 个宿主信箱(`PAYLOAD.BIN` 128K)。**槽位簇从卷末尾往前分配,簇链与目录项进镜像、簇内容不进镜像** ⇒ 槽位容量在固件里占 **0 字节**,镜像 49,664 B。设备端只做 `memcpy` + 定点写目录项 size 字段 4 字节,**FAT 表一个字节不动**。内容落 flash `script0`/`script1` A/B 快照(`seq` 大者胜 + 全 CRC 通过才有效,写非生效分区 → 断电安全)。**真源/副本契约:flash 是唯一真源,RAM disk 槽位只在开机时铺开;运行期下发只写 flash,不碰副本**(RAM disk 是与宿主共享的可写存储,运行期改它 = 和宿主抢同一块簇)。"部署完立刻生效"列为后期优化项,届时须先补"写槽前校验 FAT1"。详见 `agent-interface-mcp.md` §二之二 |
| 12 | **注入默认关,只走远程显式触发(2026-10-22)** | 往用户机器注入按键是**对用户环境的破坏性操作**:目标机若处于锁屏,那些按键会被锁屏界面当成 PIN 输入,持续失败会触发账户防暴力锁定(实测事故)。因此 `IO_LAUNCHER_AUTOSTART` **默认 0**(编译期开关),开机不再自动拉起 watcher;注入只能由远程 `input` 工具**单次触发、零重试**。失败不自动重发 —— 重试对"打系统快捷键"这类动作是持续破坏。**验收纪律:能控制用户机器的能力,验收放虚拟机/专用靶机;在主力机上做只能"人在场 + 知情 + 手动单次触发"。** 详见 `agent-interface-mcp.md` §二之三 |
| 13 | **M5 远程链路落地(2026-10-22)** | 设备侧先起 **HTTP + JSON-RPC 2.0**(`POST /mcp`、`GET /mcp`→405、`GET /files/<name>`、`GET /ping`),凭据走 NVS、**用 UART0 控制台配网**(凭据不进仓库、不进固件二进制)。工具在 2026-10-24 改名为 6 个:`status` / `ls` / `read` / `capture` / `input` / `update`。**当前是明文 HTTP,TLS-PSK 双向鉴权待做**(mDNS 发现见第 16 行)。验收方式用**裸 HTTP 构造**(不依赖 MCP 客户端),`tools/mcp_probe.py` |
| 14 | **HID 描述符约束(2026-10-22)** | ① **不能加 Output Report(键盘 LED 回写)**:即便 Usage Page 写对(LED 属 0x08 LEDs Page),Windows 的 HidUsb 仍会启动失败(`USB Input Device` 节点 `ProblemCode=10`),整个 HID 接口不可用 —— 而 M1 时代没有它时枚举正常。规范上 Boot Keyboard 该有 Output Report,这是**待补项**,加之前必须先在 Windows 上把 SET_REPORT 应答路径验通。② **改 HID 报告描述符必须 bump 序列号后缀**(`IO_USB_SERIAL_SUFFIX`):Windows 按 VID/PID/序列号识别设备实例,序列号不变就**复用旧的失败实例、从不重新解析**,表现为"改了没生效"。③ **"枚举出键盘节点" ≠ "输入能到达主机"** —— HID 可用性的验收项必须是"按键真的到达了目标应用" |
| 15 | **mDNS 发现落地(2026-10-23)** | 用 **lwIP 原生 mdns responder**(直接编 IDF 树里 `apps/mdns/{mdns,mdns_domain,mdns_out}.c`,不引 esp-mdns ⇒ 组件管理器关着也能离线复现),广播 `_ioagent._tcp.local` + 端口 8080;`sdkconfig` 需 `CONFIG_LWIP_NUM_NETIF_CLIENT_DATA≥1`(mdns 要占 netif 的 client_data 槽)。**两条硬事实**:① **mDNS 装配必须在 TCP/IP 线程里做** —— `mdns_resp_add_netif()` → `mdns_resp_restart()` → `mdns_set_timeout()` → `sys_timeout()` 是往 lwIP 定时器链表插条目,而本项目 `CONFIG_LWIP_TCPIP_CORE_LOCKING` **没开**,从别的任务插是数据竞争。症状极具迷惑性:**所有 mdns API 都返回成功、日志照打,但线上一个包都不发、查询也不应答**(定时器不触发 ⇒ 状态机卡在 `MDNS_STATE_PROBING` ⇒ 不发探测;`mdns_handle_question()` 在 state≠COMPLETE 时直接 return)。解法:`tcpip_callback()` 把装配投递进 TCP/IP 线程,再等回调置位。② 宿主侧**必须 `IP_ADD_MEMBERSHIP` 加入 224.0.0.251** —— 绑 5353 端口 ≠ 加入组,AP 开着 IGMP snooping 时不入组就收不到组播应答。**lwIP 的 responder 不实现 PTR**(源码里没有 `DNS_PTR`),通用 zeroconf 浏览器扫不到服务类型;它会用实例 SRV 应答服务类型查询,所以本项目**自带解析器** `tools/discover.py`(无三方依赖,`--json` 直接给 MCP 端点)。实测:`sophon-lab.local` → `172.17.21.225:8080` |
| 16 | **注入序列与时序定案(2026-10-23)** | 实测标定,五条硬事实:① **HID keycode 是用法 ID 不是 ASCII** —— 传字符 `'R'`(0x52)会按成小键盘上的键,`Win+R`/`Ctrl+A` 从来没触发过(**这才是"运行框不打开"的真因**,不是 USB 枚举也不是 IME);字母一律走 `io_hid_keycode_for_letter()`。② **组合键按住 ≥600ms**:30ms 会被 Windows 整段丢弃。③ **单键/逐字符按住 ≥60ms**:8ms 时微软拼音把输入打成乱码(`ni hao ` → 49 个"啊")。④ **输入态由 IME 实例记忆,新开的输入框继承上一次的态** ⇒ 盲按 Shift 的结果取决于上一轮(实测 5 次 OK/FAIL 交替);`Ctrl+Space` 与 `Shift` 在微软拼音里是**同一个开关**(按两次等于没切,同报告/先报修饰键两种发法都无效)⇒ **只能"先测后打"**:`macro=selftest` 当探针(`INJ.TXT` 出现 = 当时纯 ASCII ⇒ 英文态;不出现 = 中文态),中文态时 launch 传 `ime:"toggle"`(新框继承中文 → Shift 一下 → 英文)。**设备端不自动重试**(注入是破坏性操作),重试由 agent 决定。⑤ **运行框会预填历史命令**(非选中态)→ 必须先 `Ctrl+A` 全选,否则命令被追加成废命令。配套:`input` 支持**命名键**(SHIFT/ENTER/ESC/…)、`macro=selftest`/`ime_toggle`、以盘上 `INJ.TXT`/`CAP.TXT` 作为客观判据(`marker_seen`) |
| 17 | **设备端 HTTP 保持最小:单例模式,不排队(2026-10-23 定调,P1-B 已实现)** | **MCP over HTTP+JSON 是被 agent 生态逼出来的兼容层,不是产品目标** —— agent 只支持 MCP,快速实现优先于"做完整 HTTP 服务器"。设备端**一次只执行一个动作**;**有动作在执行时,新的动作请求直接返回 `busy`**(不排队执行、不排队等结果),`BUSY` 沿用既有命令状态词表。**实现方式(查证 esp_http_server 源码后定的)**:它是**单 task**,长操作在 handler 里跑时第二个请求**连 handler 都进不去** → 物理上拿不到 busy。所以 **长操作移出 httpd task(worker task)** + **发起方不等结果**(结果由盘上标记文件回读)。**不引入 job 状态机/轮询端点** —— 标记文件本来就是状态。附带:注入期间读类工具照常服务;`status` 带 `busy` 供 agent 轮询 |
| 18 | **HTTP 服务的鲁棒性基线(2026-10-23,由 skill eval 的真实 agent 撞出来)** | ① **读"已存在"的文件会让设备 panic 重启**:`Guru Meditation / LoadProhibited`(EXCVADDR=0x45,回溯被冲垮 ⇒ httpd 任务栈被写坏)→ 软件复位 → **MCP 端点整个消失**;读"不存在"的文件则正常返回。两条路径只差"把内容搬进应答"这一步。httpd 任务栈是 IDF 默认的 **4096 字节**(单 task,handler 链全在这一个栈上)。**已修并真机验证(2026-10-24)**:栈提到 8192、加固的 host/origin/ct 缓冲挪到 static(单 task 无并发问题)、读盘缓冲也挪到 static。回归脚本 `tools/read_hang_probe.py`:宿主现写的 21 B 文件、槽位里的 134 B 文件都是 0.2 s 内正常返回,设备存活。② **单 task 会被慢客户端堵死**:一个"Content-Length 说很大、body 却不发完"的请求会让 httpd 卡在 `httpd_req_recv()` 里等满 3×5=15 秒,期间**所有**其它 agent 请求排队(实测把正常请求拖到 20 秒);已把 `recv_wait_timeout` 降到 2 秒、重试 3→2,最坏约 4 秒。③ 工具链自带的 `tools/mcp_probe.py` 原来**不绕代理**,局域网地址被 Clash 代理走会回 **502 Bad Gateway**(看起来像设备死了)——已修(空 ProxyHandler + 清环境变量)。复现/验收脚本:`tools/http_robustness_probe.py`、`tools/read_hang_probe.py` |
| 19 | **MCP 接口硬化落地(2026-10-23,P2+P4)** | ① **body 上限 128 KB → 413**:上限按"最大的合法请求"定(`update` 把 64 KB 原字节 base64 装进 AGENT.EXE 槽 ≈ 87 KB);意义是**不让 `content_len` 变成无界的 malloc**(httpd 只有一个 task,超大 body 会把服务打死)。② **`Content-Type` 非 `application/json` → 415**。③ **`Host` + `Origin` 绑定设备自己**(当前 IP / 设备名 / 设备名.local)→ 403:调用方是 native agent、**永不发 `Origin`**,所以带 `Origin` 且不是我们就拒 —— 这是 DNS rebinding 防护;`/files/*`(`/ping` 也一样,前者装的是桌面帧)。④ **协议版本协商**:支持 `2024-11-05`/`2025-03-26`/`2025-06-18`,客户端 `initialize` 报什么我们支持就**原样回**,不支持就回最新的;initialize 之后的 `MCP-Protocol-Version` 头不认识就回 400。⑤ **去掉假的 session id**:以前硬编码 `Mcp-Session-Id: ioagent-1`,我们是**无状态**服务,回假 session id 会让客户端以为有会话要维持 —— 现在根本不发这个头;通知按规范回 **202 + 空 body**。**一条容易漏的细节:拒绝之前先把 body 抽干**(有上限)—— 服务端带着未读请求体关连接会发 RST,客户端还没读到的响应就被丢弃,客户端只看到"连接被重置"而不是 403/413/415。验收:`tools/mcp_hardening_probe.py` **17 项全绿** |
| 20 | **对外接口全面改名 + 端点统一(2026-10-24)** | **旧名彻底替换,不做兼容**。① **工具集 5 → 6,全部改成"一个动词 = 一件事"**:`device/info`→`status`、`observe`→`read`(`target` 参数改名 `name`)、`act`→`input`、`deploy_payload`→`update`,新增 `ls`(列盘)与 `capture`(取最新一帧);`watcher/status` **取消**,并进 `status.watcher.running`(设备只有一个状态入口)。**弃用**:`output`(截图不是 dongle 产出)、`write`(设备不提供写盘能力,写盘会覆盖 watcher 的帧文件)、`type`(只覆盖 7 类 HID 动作之一,应是 `input` 的参数值)。② **端点统一**:`GET /blob/b0..b3` → **`GET /files/<name>`**(按文件名取,不再暴露内部缓存槽位,也不用先 observe 再取);新增 `GET /mcp` → **405**(规范要求:MCP 端点要么开 SSE 流,要么明确回 405,不能是 404)。③ **MCP 原生 ImageContent 取代 blob URL**:≤64 KB 的 JPEG 直接内联成 image 内容块,大的才给 `/files/<name>` URL;每个工具结果都带 `structuredContent` + `outputSchema` + `title`。④ **`isError` 补齐到所有工具**(以前只有注入类设,读类工具失败会被合规客户端当成功);未知工具回 JSON-RPC **-32602**。⑤ **两个真机 bug 在改名过程中被撞出来并修掉**:(a) `dispatch_rpc` 对普通调用返回"结果体"、对未知工具返回"完整错误信封",调用方一律包一层 `rpc_ok()` ⇒ **错误被塞进 `result` 里**,客户端按规范只认顶层 `error` ⇒ 未知工具看起来像调用成功了。改成 `dispatch_rpc` **只产出完整信封**。(b) `"/files/*"` 通配 URI **默认不展开** —— IDF 的 `httpd_config_t.uri_match_fn` 默认是 NULL(精确比较),`httpd_uri_match_wildcard()` 是**公开 API** 但要自己设进去(旧代码注释"实测 IDF 5.5 对通配模板匹配不上"诊断反了)。**验收**:`tools/contract_probe.py` **24 项全绿**(6 工具齐全/无旧名残留/title+outputSchema/未知工具 -32602/isError 齐/GET /mcp→405+/files/<name> 200 与 404),`tools/mcp_hardening_probe.py` 17 项全绿,`tools/http_robustness_probe.py` 三阶段全过,`tools/read_hang_probe.py` 崩溃回归通过 |
| 22 | **产品名定案:IO_Agent → Sophon(2026-10-24)** | 名字: **Sophon** —— *AI Agent 的智子:从潜空间抵达实体机器的眼与手。*(*The Sophon of AI Agents: eyes and hands reaching from latent space into physical machines.*)★ **分两层,只有人读层改** ★**人读层(全改)**:README 标题+slogan、4 份设计文档、skill(SKILL.md/2 references/2 scripts,目录名 `io-agent-mcp`→`sophon-mcp`)、USB 字符串描述符(Manufacturer/Product/HID/MSC)、**USB MSD 的 vendor/product 串**(`SOPHON` / `Sophon Disk`,Windows 设备节点显示名)、**FAT 卷标**(`IOAGENT`→`SOPHON`,宿主工具按卷标找盘)、串口控制台提示符(`ioagent>`→`sophon>`)、MCP `serverInfo.name`、日志标签(`io_agent`→`sophon`)、设备默认名、固件产物名(`project(io_agent)`→`project(sophon)`,`io_agent.bin`→`sophon.bin`)。**绑定层(一律不动)**:① **NVS 命名空间 `"ioagent"`** —— 改了读不到已存的 WiFi 配置,设备会掉线;② **mDNS 服务类型 `_ioagent._tcp.local`** —— 线缆层标识,已部署的发现脚本依赖它;③ **USB VID/PID/序列号** —— 序列号按**芯片 MAC** 填(不是设备名),Windows 靠 VID/PID/序列号认设备实例,换了就是"新设备";④ **`io_*` 模块名** —— 内部代码标识(与 ESP-IDF 内置组件避让),跟产品名无关,改它是纯风险无收益。**顺带查出一个真问题**:改设备名后 **mDNS 实例名要重启才更新** —— mDNS responder 只在开机装配一次(`mdns_resp_add_service` 用的是当时的设备名),改名走 `DEV.CFG` 的 `name=` → 写 NVS → 重连 WiFi,而重连只**重新 announce**、**不重新注册服务名** ⇒ NVS 与 `/ping` 都是新名、mDNS 还在广播旧实例名。已写进 README「已知硬约束」。**验收**:重编(`sophon.bin` 908,496 B)+ 重刷 + 改名 + 重启后,宿主看到卷标 `SOPHON`、mDNS 实例 `sophon-lab.local`(服务类型仍是 `_ioagent._tcp.local`)、`/ping` 报 `device:sophon-lab`;`contract_probe` 24/24、`hardening_probe` 17/17、`mtime_probe` 4/4、`read_hang_probe` 崩溃回归全过 |
| 21 | **FAT16 mtime 偏移修正 + 注入自检改判"这次写的"(2026-10-24)** | ① **mtime 偏移写错了**:`io_disk.c` 把目录项的"最后写入时间/日期"读成 `0x24/0x26` —— 那已经越过目录项的 32 字节,读到的是**下一个条目**的字节。症状极其隐蔽:解出来是 **1995 年的合法日期**(看着挺合理,没有任何报错),于是 `ls` 的 mtime 全错,而"挑最新一帧"是按 mtime 排的 ⇒ **挑帧逻辑一直是错的**。正确偏移是 FAT16 目录项的 `0x16`(时间)/`0x18`(日期);已把整张 32 字节布局表写进代码注释,防止再犯。**教训:任何"读出来挺合理"的值都要拿独立实现交叉核对**(本项目的 `tools/dump_rootdir.py` 就是干这个的)。② **两条 mtime 语义要写清楚**:FAT16 存的是**本地墙钟时间**(无时区),设备按 UTC 解释 ⇒ 绝对值与真实 UTC 差"宿主机时区偏移"(实测 +8h),**只能用来比较先后/判断有没有被重写,不能当绝对时刻**;FAT 时间戳只有 **2 秒**粒度。③ **注入自检的判据从"存在"升级为"这次写的"**:`INJ.TXT` 同一次开机内一直在盘上,只判存在性 ⇒ **第二次自检会在 1 秒后直接报 PASSED,哪怕这次注入根本没到达目标机** —— 这正好打在"IME 态探针"上(判错 ⇒ 后面打的全是乱码,而且看起来一切正常)。改成:注入前记下它的 mtime,要求**变了**才算数(一次自检要 ~7 秒,天然避开 2 秒粒度)。**注**:`CAP.TXT` 保持"存在即算活"是有意的 —— watcher 只在启动时写它,改成"必须新鲜"会把"watcher 本来就在跑"误判成失败。④ **否掉了"每次用唯一标记名"的替代方案**:根目录只有 **512** 项,几百次自检就会撑爆(而且设备端不写 FAT ⇒ 删不掉旧文件)。**验收**:新增 `tools/mtime_probe.py` **4 项全绿**(跨文件偏移恒定 / 宿主重写后设备看得见变更 / 先后可排 / `capture` 挑中最后写的帧),`contract_probe` 24/24、`hardening_probe` 17/17、`read_hang_probe` 崩溃回归全过 |

## 四、契约冻结清单(改一处同步四处)

### 4.1 盘上文件与目录(虚拟盘根,短名 8.3)

| 路径 | 写方 | 读方 | 内容 |
|---|---|---|---|
| `SNAP_P.JPG` | watcher | ESP32 | 滚动预览帧(低分辨率,默认 960×540,quality 70) |
| `SNAP_A.JPG` | watcher | ESP32 | 全分辨率帧(按需单帧) |
| `SNAP_R.JPG` | watcher | ESP32 | 变化区域裁剪帧(全分辨率 ROI,只写变化区;按 N1 裁剪在 watcher 侧做) |
| `SNAP.IDX` | watcher | ESP32 | 当前帧名 + 长度 + 序号 + 光标坐标 + ROI 清单(双缓冲,序号两次一致才取帧) |
| `DIFF.IDX` | watcher | ESP32 | 变化区域清单(ROI 列表,JSON 单行) |
| `CMD\*.cmd` | watcher | ESP32 | 待执行命令(JSON 单行) |
| `RESP\*` | watcher | ESP32 | 执行结果 |
| `CAP.TXT` | watcher | ESP32 | 能力自陈(os/arch/admin/net/screen/watcher_ver) |

### 4.2 帧率与带宽约定(定案 2 落地)

- 预览帧常驻 **2fps**,降采样 960×540,quality 70(约 40-60KB/帧)。
- 变化 ROI:watcher 分块(16×16)哈希比较定位变化区,只写变化区 JPEG(按 N1,裁剪在 watcher 做)。
- 静态桌面(传统办公常态):变化 ≈ 0 → USB 写入 ≈ 0。
- 滚动页面:ROI 面积通常 <20% 屏 → 约 60KB/次 × 2fps = 120KB/s,远低于 MSC 实测 1MB/s 上限。
- 全分辨率单帧:仅当 agent 下发 `CAP_FULL` 时抓一次(约 300KB),不常驻。
- **禁止** 1080p × 5fps 连续全帧写盘(1.5MB/s > 1.2MB/s 实际上限,会饿死 CMD/RESP)。
- diff 计算必须走分块哈希,**不要在 PowerShell 里逐像素比对 1080p**(性能黑洞)。
- **diff 采样有下限约束(2026-10-24 实测后补)**:16×16 块内按步长采样,步长 4 会让
  1px 细描边(光标环、UI 细线)完全落在采样点之间 → 块哈希不变 → 明明变了却报"无变化"。
  约束:①光标标记必须是**实心圆点**(不能是空心环);②块内采样步长 ≤3。

### 4.3 帧格式(2026-10-21 修订:随拓扑反转作废)

原自定义帧头 `magic|ver|type|seq|device_uuid|len|payload|tag` 已作废
(connectivity-and-binding.md §十):现行帧走 MCP(JSON-RPC/HTTP),
`resources/read` 返回裸二进制 blob,无自定义包头;设备 UUID 由 mDNS 实例名
与 MCP 会话承载。自定义帧头仅在后续"设备↔中继 relay"启用时另行冻结,
不得按旧格式施工。

### 4.4 错误码与状态

- 命令状态:`OK | PARSE_ERR | BAD_OP | TIMEOUT | BUSY | EXEC_ERR | HW_ERR`(沿用 v1)。
- observe 三态:`FRAME | NO_SIGNAL | BLACK`,另加 `SECURE_DESKTOP` 标记。
- 幂等键:沿用 v1 的 6 hex id 语义,ESP32 侧保留最近 8 个已执行 id,重复直接跳过。

### 4.5 USB 描述符与端点分配(2026-10-24 实测冻结)

| 项 | 冻结值 |
|---|---|
| VID / PID | `0x303A` / `0x8001`(量产需申请独立 VID/PID) |
| 配置数 / 接口数 | 1 个配置、**2 个接口**(MI_00 = HID,MI_01 = MSC) |
| HID 报告描述符 | 101 字节;**Report ID 1 = 键盘,Report ID 2 = 鼠标**;单接口 + 复合描述符(A′),**不做 HID OUT** |
| HID 集合 | 主机侧拆成 `MI_00&COL01`(HID Keyboard Device)+ `MI_00&COL02`(HID-compliant mouse)——**已实测,不是设计意图而已** |
| MSC | 单 LUN,介质为 PSRAM RAM disk(8192 扇区 × 512 B = 4 MB),`is_removable = 1` → Windows 按可移动设备处理。**出厂镜像稀疏**:固件内嵌 49,664 B(97 扇区),卷容量由 BPB 声明,尾部在 PSRAM 补零 |
| 端点 | EP0(控制)+ `0x81` HID IN + `0x02` MSC OUT + `0x82` MSC IN → **含 EP0 共 3 个,IN 侧 2 个** |
| 富余 | IN 2 个 / OUT 3 个 → 留 vendor bulk(产线测试口 / WebUSB),M2/M3 再启用 |
| 描述符字节数 | 配置描述符 57 字节(固件运行时自检) |
| 链路活性判据 | **`tud_ready()` = `mounted && !suspended`**。**不用** `TINYUSB_EVENT_DETACHED`(DWC2 device 模式不实现断开检测,永不触发)。实测拔线 `mounted=1 suspended=1`;重新插上后主机 SOF 清 `suspended`,140 ms 恢复 |

> **不做 USB CDC**:会占 2 IN + 1 OUT 且端口号漂移,详见 `hardware-selection.md` §三。

### 4.6 Flash 分区与存储落点(2026-10-22 冻结)

| 分区 | 偏移 / 大小 | 用途 |
|---|---|---|
| `nvs` | 0x9000 / 24 KB | WiFi 凭据、PSK、设备 UUID、watcher 拉起策略 |
| `phy_init` | 0xf000 / 4 KB | RF 校准 |
| `otadata` | 0x10000 / 8 KB | OTA 状态 |
| `ota_0` | 0x20000 / **2 MB** | 固件 A 槽(实占约 863 KB,余 57.8%) |
| `ota_1` | 0x220000 / **2 MB** | 固件 B 槽 |
| `script0` | 0x420000 / 512 KB | **T2 运行时 payload 仓 A**(断电不丢) |
| `script1` | 0x4A0000 / 512 KB | **T2 运行时 payload 仓 B**(A/B 切换) |
| 留白 | 0x520000 起,10.9 MB | 以后给日志仓/数据仓/资产仓 |

> **ota 槽为什么是 2MB**:接上 WiFi + HTTP(MCP server)后,app 从 283 KB 涨到 **863 KB**
> (WiFi 协议栈 + lwIP + esp_http_server + cJSON 进来)。1 MB 槽只剩 16% 余量,后面还要加
> TLS-PSK 必然爆 → 扩到 2 MB,留白仍有 10.9 MB。
> **代价**:`script0/1` 偏移随之移动,flash 里已有的 payload 快照读不到(新位置是空白区,
> magic 不匹配 → 干净出厂态),需要重新下发一次。

**存储落点的两条规则**(T2 实现时必须遵守):

1. **要留存的东西不能放 RAM disk** —— RAM disk 每次上电从出厂镜像重建,dongle 拔线即断电,
   运行时写进去的内容全丢。需要跨断电留存的 → `script0/script1`。
2. **RAM disk 里的槽位必须簇链定长且位置预知** —— 簇链定长(簇数由容量决定,出厂镜像里
   已分配)⇒ 设备**不用分配也不用释放簇**;位置预知 ⇒ 设备端只要
   `(data_offset, size_field_offset)` 就能覆写。设备**只碰簇数据 + 根目录项 size 字段 4 字节**,
   **FAT 表一个字节不动**,根目录项的其余字段(名字/首字节/属性/时间戳/簇链)也不动。
   槽位偏移在构建期由 `tools/mkfatimg.py` 连同镜像一起产出(单一真源,不会漂移)。
   代价:新 payload 超过槽容量要报错,不能就地扩容(扩容就要设备端写 FAT,违背零 FAT 原则)。
3. **引导内容的真源必须在宿主够不到的地方** —— flash `script0/1` 是真源,RAM disk 槽位
   是"每次开机铺开的副本"。**运行期下发只写 flash,不碰副本**。RAM disk 是与宿主共享的
   可写存储,运行期改它 = 和宿主抢同一块簇(宿主可能已删槽文件、簇被释放,或已建了
   占用同簇的新文件)。真源放到宿主够不到的地方,竞争就不存在。

### 4.7 WiFi 日志与 OTA 通道(2026-10-25 冻结)

单 Type-C dongle 只有一个口(插在目标机上做 HID+MSC)⇒ **开发机烧不进去**,
固件更新与日志都必须走网络。两条通道都定成**「host 主动连设备」**。

| 项 | 冻结值 |
|---|---|
| 日志服务 | **UDP server,端口 8090**。开发机发一个包来"要"日志,设备把日志回给**请求的源地址**(回程流量 ⇒ 开发机**不用加防火墙规则**) |
| 日志授权 | 设备端预置**源 IP 白名单**(`DEV.CFG` 的 `logips=`,与 `ssid` 一并给用户)。不在表里的请求**直接丢,连回包都不给**;**没配就不开**(socket 不建、钩子不装)⇒ 量产固件日志面为零 |
| 日志热切换 | UART0 控制台 `log ips <ip>[,<ip>]` / `log off` / `log status`(开发机 DHCP 换 IP 后不用重贴 U 盘) |
| 日志回执 | `status.log{active, open, allowed, rx, dropped, tx, bytes, hook}` —— `dropped` 涨 = "有人在问但没授权" |
| OTA 工具 | `flash { url }`。**pull 模型**(设备去开发机上拉镜像),写进另一个 OTA 槽,`esp_ota_end` 校验(描述符 + SHA256)通过才切启动分区 ⇒ **刷坏了不变砖** |
| OTA 凭据 | `status.ota.boot` / `status.ota.next` = 当前启动槽 / 下次写入槽。**刷同一份构建时版本号不变**,所以"新固件真的在跑"要看 `boot` 翻转,不能只看版本号 |

> **为什么 OTA 不用 push**:push 900 KB 会占住设备**唯一的 httpd task** 整个传输时长
> ⇒ MCP 整个服务卡死。pull 边下边写另一个槽,校验不过当前分区不动。
> **为什么日志不用 HTTP**:试过"第二个 httpd + chunked 流" —— 代价太高(多一个 httpd task
> + 一套 socket 管理 + 每条日志一次 vprintf 钩子),而且实测会外溢到 MCP 通道。

### 4.8 状态 LED(2026-10-25 落地实灯)

`status.led` 是一个 int(0~4)。JSON 里**只有这个 int**,不带颜色/文案/子状态;
颜色与节奏的对应只在这张表里,人和 agent 各自查。

| 码 | 状态 | 灯 | 节奏 |
|---|---|---|---|
| 0 | 就绪(已联网 + MCP 在服务) | 🟢 绿 | 常亮 |
| 1 | 启动中(刚上电,还没开始连网) | 🔵 蓝 | **快闪**(亮 100 ms / 灭 100 ms) |
| 2 | 网络未就绪(在连/没配网/掉线重连) | 🟡 黄 | **慢闪**(亮 500 ms / 灭 500 ms) |
| 3 | 动作执行中 | 🔵 蓝 | 常亮 |
| 4 | 故障(上次注入自检失败,"手不灵") | 🔴 红 | 常亮 |

优先级 **3 > 4 > 1 > 2 > 0**。三条约束:① 只放**设备自己测得到**的状态 ② 只放
**现场的人会据此行动**的状态 ③ **JSON 里只给一个 int**。

**硬件(2026-10-25 实测)**:开发板上是**单颗大颗粒 WS2812,数据脚 GPIO48,颜色字节序 GRB**
(红→绿→蓝→白 自检实测通过)。板上另外 3 颗 RGB 小灯珠是**普通指示灯,不是可寻址外设**,不受我们控制。
RMT 发位流(WS2812 位周期 0.4/0.85 µs = 800 kHz,bit-bang 精度不够,只有硬件计时能发稳)。
亮度可配 `CONFIG_SOPHON_LED_BRIGHTNESS`:128 在大颗粒上"亮得像小手电筒"(还看不出颜色),
24 ≈ 9%,远看清晰近看不刺眼。编译期开关 `CONFIG_SOPHON_LED_WS2812`(量产板引脚不同或
没有可寻址灯就关掉,**状态码照算**,只是没有灯)。同一串灯的多个像素必须**在一次 RMT 事务里
连续发完**(WS2812 逐像素 latch,中间断一下就被当成复位)。

**"动作执行中"有三个来源**:注入(`input`)/ `update` 写槽 / `flash` OTA。
它们会重叠 ⇒ busy 用**计数器**,不是 bool(否则后一个把前一个的"忙"抹掉)。

## 五、硬件到货后的第一动作

1. DevKitC-1-N16R8(主力开发板,已下单):插电脑 → esptool 烧录 blink → 确认烧录通路。
2. 形态验证 dongle(已下单,USB-A 直插):插电脑枚举出键盘 + U 盘 + 串口三合一 → 证明 OTG 通路。
3. 两块板都到位后,直接进 M1(固件最小可枚举),不要再在设计层加新决策。
