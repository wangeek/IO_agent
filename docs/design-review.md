# 设计评审结论(2026-10-20)

对 v2 设计文档做的一次交叉评审,记录 7 条发现与定案结论。
本文是**结论索引**;具体方案见各自指向的文档,施工顺序见 `implementation-plan.md`。

## 背景

评审对象:`output-channel-design.md`(v2)、`agent-interface-mcp.md`(v1)、
`hardware-selection.md`、`msc-watcher-protocol.md`(v1 存档)与 README 决策表。
项目当时状态:设计定案、零行代码、硬件已下单待到货。

## 结论一览

| # | 主题 | 结论 | 落点 |
|---|---|---|---|
| 1 | 采集 API 冲突 | **定案:走 GDI(CopyFromScreen),游戏/真独占全屏后置**;采集封装留接口缝,后续可插拔 DXGI Desktop Duplication | agent-interface-mcp.md §四.2 / implementation-plan.md §三 |
| 2 | USB 带宽 | **定案:分层快照**——滚动低分辨率预览 + 变化 ROI + 按需全帧;禁 1080p×5fps 连续全帧 | output-channel-design.md §四 / implementation-plan.md §四.2 |
| 3 | 配网与回传 | **定案:两级配网(出厂预置 / SoftAP 手机配置)+ 运行时分级回传(带外 ↔ L2 自动降级)** | connectivity-and-binding.md |
| 4 | LED 通道 | **定案:退出主路径**,降为可选调试/心跳;Stage 0 不以 LED 成功为前提 | README 决策表 / msc-watcher-protocol.md(标注作废) |
| 5 | 拔盘无残留 | **定案:靠显式自检退出,不靠"载体消失自然退出"**;验收加"无残留进程" | output-channel-design.md §四 / §八 |
| 6 | 安全基线 | **定案:平移进实施清单**,不随 v1 文档一起作废 | implementation-plan.md §三 |
| 7 | 文档与仓库卫生 | **定案:README 目录不指向不存在的文件;仓库纳入版本控制** | README / 仓库 |

## 逐条说明

### 1. 采集 API 冲突(已定案:传统用途优先)

矛盾:output-channel-design.md 写 watch.ps1 用 `CopyFromScreen`(GDI),
agent-interface-mcp.md 却判"GDI 弃用、必须用 DXGI Desktop Duplication",
并指出 DirectDraw 老游戏正是 GDI 黑屏重灾区。按前者实现,主场景必然黑屏。

定案:**一代先保传统桌面用途**(GDI,Windows 自带、零安装、最快),
DXGI 后续插入同一采集接口,协议与固件不受影响。
代价如实记录:真独占全屏(D3D9 FSE / DirectDraw)可能黑帧,属已知边界而非缺陷。
副作用(正向):传统办公场景下画面变化小,ROI 截图价值反而更高。

### 2. USB 带宽(已定案:分层快照)

算术冲突:1080p JPEG 约 300KB × 5fps = **1.5MB/s 持续写**,
而 MSC 实测上限 0.8-1.2MB/s → CMD/RESP 被饿死,双缓冲撕裂风险上升。
延迟预算表里"截屏+编码 ≤200ms"其实是靠 5fps 轮询周期凑出来的;
改成按需抓帧则单帧冷启动约 1s,又违反 ≤1s 指标。这是一个真实的两难,不是笔误。

定案:预览 2fps 降采样(960×540,q70)常驻 + 只写变化 ROI + 全帧按需。
变化量驱动带宽,静态桌面写入趋近 0,滚动页面约 120KB/s,峰值 <300KB/s。
附带要求:diff 走 16×16 分块哈希,**不要在 PowerShell 里逐像素比对 1080p**。

### 3. 配网与回传(已定案:见 connectivity-and-binding.md)

缺口:WiFi 配网流程、agent 端点地址来源、设备鉴权与绑定,全无设计;
且"与目标机网络无关"的带外宣称在硬件与现实面前都站不住。

补全时确认的三条硬约束(不可绕):

1. ESP32 不能向主机发起任何数据 → 配网只能"主机推送到盘上",不能"ESP32 拉取";
2. USB CDC 兜底硬件上不存在(S2/S3 = EP0 + 6 可配置,HID+MSC 占 4,CDC 需 3)→ 回传只有 WiFi 一条硬件路;
3. 无痕基线禁止碰目标机 → 配网步骤必须不碰目标机完成 → 出厂预置或用手机。

定案与最大修正:**L2 不是"加速",而是现实中最可能成功的主路径**。
真实运维场景目标机通常有网,而 ESP32 常连不上外网(企业网/隔离网/需认证 WiFi);
ESP32 带外的真正价值是"目标机完全隔离/无网"这一唯一场景。
故表述改为:**ESP32 带外 + L2 直连,两条都要有,运行时哪条通走哪条。**

### 4. LED 通道(已定案:退出主路径)

3-9 bit/s、macOS 不下发 LED 报告、一次 ACK 最多 1.5s;
而 v1 状态机把**每次状态迁移**都绑在 LED 序列上,v2 已有 RESP 文件回执,LED 沦为纯冗余,
却仍占据 bootstrap Stage 0 的必经位——在 macOS 上会永远过不去。
定案:降为可选调试信号;Stage 0 改为"USB 枚举 + HID 通路 + agent 链路连通性"三件套。
原计划中的 `stage0-led-handshake.md` 随之取消,不单独成文。

### 5. 拔盘无残留(已定案:显式自检)

"拔掉 ESP32 = U 盘消失 = 驻守脚本失去载体自然退出"不成立:PowerShell 进程不因可移动盘
消失而自动退出。原验收项只查磁盘文件与服务残留,漏了**进程残留**。
定案:watch.ps1 写盘失败即判定载体丢失 → 主动退出;验收增"无残留进程"。

### 6. 安全基线(已定案:平移)

v1 的安全基线(op 白名单、type ≤256B、鼠标 clamp ±127、键盘基线恢复、
检测到人动键盘即中止)质量高,但 v2 文档的可平移清单没点名它,实施时容易丢。
定案:写进 implementation-plan.md §三 与 M3 验收条目。

### 7. 文档与仓库卫生(已定案)

README 目录列了不存在的 `docs/stage0-led-handshake.md`;项目无版本控制。
定案:目录与实际文件对齐;仓库纳入 git,文档与固件同仓。

## 附带发现(非阻塞,但影响实施)

- **产线约束**:Espressif 文档明确启用 flash encryption / secure boot 会禁用 ROM 里的
  USB-OTG 栈。本项目 USB 口即产品主接口 → 量产固件**必须走 WiFi OTA**,
  开发期不开加密(否则每次更新都要全量擦除)。
- **端点核对结论**:README"4/6 端点,剩 2 富余"与官方数据一致,无需改;
  但由此可知 CDC 不可用,也就没有"USB 串口当数据回传"这条退路。
- **FAT16 BPB 正确性**:RAM disk 若 BPB 参数不对,Windows 会提示"需要格式化"甚至不认卷,
  是 M1 的头号坑。
- **diff 性能**:PowerShell 逐像素比对 1080p 是性能黑洞,必须分块哈希。
