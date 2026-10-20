# 实施计划 v1(2026-10-20)

从"设计定案、零行代码"到"可验收的一代机"。本文是施工顺序与契约冻结清单。
设计依据见 README 决策表 + output-channel-design.md + agent-interface-mcp.md +
connectivity-and-binding.md。

## 一、仓库骨架

```
IO_agent/
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
│   ├── usb/                      TinyUSB 描述符:复合设备(HID 键盘/鼠标 + MSC)
│   ├── disk/                     PSRAM FAT16 镜像 + MSC 后端
│   ├── hid/                      注入序列执行器(本地定时,<10ms/步)
│   ├── net/                      配置来源(A/B/C)、TLS 回传、帧编解码
│   ├── cfg/                      NVS 配置与设备凭据
│   └── capability/               能力注册表(编译期开关 + 运行时查询)
├── watcher/                      目标机侧(零安装,驻 RAM disk)
│   ├── watch.ps1                 驻守主脚本
│   ├── capture/                  采集抽象层(当前 GDI,后续可插拔 DXGI)
│   └── assets/                   预置脚本与工具(打进固件分区或 RAM disk 镜像)
└── agent/                        agent 侧
    ├── mcp/                      MCP server(act / observe)
    ├── uplink/                   设备绑定、鉴权、帧解码
    └── storage/                  截图与响应入库
```

约定:

- 固件端**只搬运,不解析内容**(哑设备原则,N2 延续)。
- watcher 端**有像素,所以 diff/ROI 在这里算**(N1/N2 延续)。
- 所有跨端契约(帧格式、盘上文件名、错误码)以本文 §四 为准,改一处必须同步四处。

## 二、里程碑

### M0 环境与骨架
- 建仓、目录落位、ESP-IDF 工程生成、模块边界定死(不写业务)。
- 验收:`idf.py build` 通过;目录结构与本文一致。

### M1 固件最小可枚举
- TinyUSB 复合设备:**HID 键盘 + MSC**;PSRAM 内构建 FAT16 镜像(4-6MB 设计值)。
- 验收:Windows 设备管理器同时出现"键盘"与"可移动磁盘";盘可写可读;拔插不崩。
- 风险点:FAT16 BPB 参数必须正确,否则主机不认卷(会提示"需要格式化")。

### M2 注入链路
- HID 键盘/鼠标注入;**动作序列本地定时执行**(<10ms/步,禁止逐条 WiFi 往返)。
- `focus_reset` 宏:**IME 归零(Shift 点切 EN)→ Win+M → Win+R**(N4,中文输入法是头号杀手)。
- 验收:自动化脚本注入含 Shift/特殊字符的文本,主机侧逐字节一致;时序抖动 <10ms/步。

### M3 watcher 通路(目标机侧)
- HID 敲 `Win+R` → `powershell -ExecutionPolicy Bypass -File X:\watch.ps1` 拉起驻守。
- 盘符自扫(不假设 X:);`SNAP_P`/`SNAP_A` + `SNAP.IDX` 双缓冲防撕裂;
  `CMD\` / `RESP\` 命令与响应;幂等键沿用 v1 约定。
- **进程自检退出**:写盘失败(盘消失)即判定载体丢失 → 主动退出,不残留。
- 验收:拔盘 5s 内进程自行退出;连续 5fps 取帧 10 分钟,无花屏帧入库。

### M4 回传链路
- 配置来源三选一:A 目标机代取(目标机有外网,主路径)/ B 人工投送配置文件 / C SoftAP+手机(异地分发才做)。
- TLS(PSK)回传、帧编解码、agent 端绑定与鉴权。
- 验收:插入 → 无人工 → **10s 内 agent 侧收到第一张桌面截图**;
  稳定态随机 20 次全屏截图全部 ≤1s、成功率 100%。

### M5 MCP 面
- `act(sequence)` / `observe(opts)`;ROI 与 diff 清单透传;`file`/`exec` 留接口。
- 状态诚实:observe 三态(正常帧/无信号/黑帧)+ `secure_desktop`,不误报。
- 验收:`act → observe(diff) → 决策` 闭环全链无人工、无 agent 侧对目标机的软件依赖。

### M6 形态与边界
- 形态验证板(USB-A 直插)完成三合一枚举、机箱后面板供电、天线遮挡实测。
- UAC 盲过阶梯落地(默认盲过;不默认改注册表)。
- 边界诚实标注:锁屏/安全桌面无画面、SAS 不可注入、EDR 环境一代不覆盖。

## 三、定案结论的落地条款(2026-10-20 评审)

| # | 定案 | 落地要求 |
|---|---|---|
| 1 | **采集走 GDI(CopyFromScreen),游戏/独占全屏后置** | watcher 的采集封装为 `capture` 抽象层,当前实现 GDI;DXGI Desktop Duplication 后续插入同一接口,不动协议与固件。文档如实标注"真独占全屏可能黑帧"为已知边界 |
| 2 | **分层快照,带宽按变化量收敛** | 见 §四.2 文件与帧率约定;USB 写入稳态 <300KB/s,CMD/RESP 不被饿死 |
| 3 | **配置来源三选一 + 运行时分级回传** | 见 connectivity-and-binding.md §三;主路径 A(目标机代取配置),B/C 为退路 |
| 4 | **LED 通道退出主路径** | Stage 0 不以 LED 成功为前提;LED 仅作可选调试/心跳;v1 协议文档中 LED 段落标记作废 |
| 5 | **拔盘无残留靠显式自检,不靠"自然退出"** | watch.ps1 写盘失败即退出;验收清单加入"无残留进程" |
| 6 | **安全基线平移进实施清单** | op 白名单 / type ≤256B / 鼠标 clamp ±127 / 键盘基线恢复 / 检测到人动键盘即中止 —— 写进 M3 验收条目 |
| 7 | **文档与仓库卫生** | README 目录不指向不存在的文件;仓库纳入版本控制 |

## 四、契约冻结清单(改一处同步四处)

### 4.1 盘上文件与目录(虚拟盘根,短名 8.3)

| 路径 | 写方 | 读方 | 内容 |
|---|---|---|---|
| `SNAP_P.JPG` | watcher | ESP32 | 滚动预览帧(低分辨率,默认 960×540,quality 70) |
| `SNAP_A.JPG` | watcher | ESP32 | 全分辨率帧(按需单帧) |
| `SNAP.IDX` | watcher | ESP32 | 当前帧名 + 长度 + 序号(双缓冲,序号两次一致才取帧) |
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

### 4.3 帧格式(见 connectivity-and-binding.md §六)

`magic(2) | ver(1) | type(1) | seq(4) | device_uuid(16) | len(4) | payload | tag(16)`

### 4.4 错误码与状态

- 命令状态:`OK | PARSE_ERR | BAD_OP | TIMEOUT | BUSY | EXEC_ERR | HW_ERR`(沿用 v1)。
- observe 三态:`FRAME | NO_SIGNAL | BLACK`,另加 `SECURE_DESKTOP` 标记。
- 幂等键:沿用 v1 的 6 hex id 语义,ESP32 侧保留最近 8 个已执行 id,重复直接跳过。

## 五、硬件到货后的第一动作

1. DevKitC-1-N16R8(主力开发板,已下单):插电脑 → esptool 烧录 blink → 确认烧录通路。
2. 形态验证 dongle(已下单,USB-A 直插):插电脑枚举出键盘 + U 盘 + 串口三合一 → 证明 OTG 通路。
3. 两块板都到位后,直接进 M1(固件最小可枚举),不要再在设计层加新决策。
