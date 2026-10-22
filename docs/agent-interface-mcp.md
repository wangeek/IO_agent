# Agent 接口层:手眼模型(MCP 面)v1(2026-10-20)

来源:桌面图形/游戏项目 agent(sango 实测)提案,经评审采纳为主接口抽象,
修正 4 处后收录。修订权:本文与 output-channel-design.md 冲突时以后者为准
(通道层),本文只管 agent 可见接口。

## 一、定位

ESP32-S3 = agent 的**手**(HID 注入)与**眼**(截图/帧差回读)。
智能(OCR/判断/决策)全在 agent 侧,固件保持哑设备——只执行与搬运,
不做任何内容级工作(解码、比对、识别)。

视觉为主通道;U 盘 RESP 文件通道保留为大数据/二进制的次级通道
(传输细节,不进 MCP 首发面)。

场景依据(sango 实测,全盘采纳):前台焦点策略软拒(SetForegroundWindow +
ForegroundLockTimeout)、UIPI/窗口站隔离、提权窗与独占全屏的软件注入无解
→ HID 是 OS 级真输入,天然免疫;视觉验收闭环已全链跑通,外设把
"程序自转储"换成"外部拍屏",范式不变。

明确不覆盖:进程内部状态(崩溃 EIP/内部缓冲)、消息级语义注入(PostMessage)。

## 二、MCP 原语与工具集

设备端 MCP 工具集共 **5 个**(定案见 `connectivity-and-binding.md` §六):
`act` / `observe` / `watcher(start|stop|status)` / `device(info)` / `deploy_payload`。
本文详述前两个核心原语:

1. **act(sequence)**:动作序列(按键/鼠标 + 内嵌延时)一次下发,
   ESP32 本地按序执行。禁止逐条 WiFi 往返(时序必坏)。
   type/click/组合键都是 act 的宏(Alt+Tab = 按住→点按→松开)。
2. **observe(opts)**:`opts ⊆ {全屏, roi(x,y,w,h), quality, diff, max_age_ms}`。
   diff = 相邻帧变化区域清单。
3. **deploy_payload(slot, bytes)**:**2026-10-22 增**(T2)。agent 运行时把宿主要执行的
   引导脚本下发到设备,设备落 flash 仓并在 RAM disk 槽位上生效。详见下节。

(预留)**file/exec**:L1 RESP 通道落地后再暴露。

标准闭环:`act → observe(diff/roi) → 决策`,全链无人工。

## 二之二、deploy_payload:运行时下发,出厂不带业务文件(2026-10-22)

**动机**:生成一个启动脚本的成本极低(AI 现场就能写),把它预置进固件占 flash 是浪费;
而目标机不能落盘(无痕性),脚本又必须从设备来。解法是**出厂只带薄引导槽,内容运行时下发**。

| 环节 | 落点 | 谁写 | 寿命 |
|---|---|---|---|
| **唯一真源** | flash `script0`/`script1`(A/B 快照) | 设备(运行期下发);**宿主够不到**(不在 MSC 暴露范围) | **断电不丢** |
| 生效副本 | RAM disk 槽位(簇数据 + 目录项 size 字段 4 字节) | **只在开机时**由设备从 flash 铺开 | 断电即回出厂内容 |

### 真源 / 副本契约(2026-10-22 定案)

**flash payload 仓是引导内容的唯一真源;RAM disk 槽位是"每次开机从真源铺开的副本"。**

- **运行期下发只写 flash,不碰 RAM disk 副本** → 下次开机时 `io_payload_init()` 把
  真源铺进槽位,届时才生效。运行期**竞争窗口为零**。
- 为什么不运行期就写副本:RAM disk 是**宿主和设备共享**的可写存储。宿主可能已经
  删了槽文件(簇被释放)或新建了占用同簇的文件,设备再写就会写进孤儿簇、或覆盖
  宿主的新文件。**真源放在宿主够不到的地方,竞争就不存在了。**
- 拔插后恢复原状有**双保险**:RAM disk 每次上电从出厂 img 重建(宿主的一切改动
  随 PSRAM 消失)+ 引导内容从 flash 真源铺回。任一层单独成立都够。
- 宿主在运行期把盘上槽位改乱了也不影响真源 —— 下次开机照样铺回正确内容。
- **"部署完立刻生效"是后期优化项**:届时在 `io_payload_store()` 成功后显式调
  `io_payload_apply()` 即可(接口不用改),但**必须先实现"写槽前校验 FAT1 与出厂
  镜像一致"**,否则就是上面那个竞争窗口。

### 设备端零 FAT 的实现方式:槽位**簇链定长**(簇数由容量决定,出厂镜像里已分配好)
⇒ 设备**不需要分配也不需要释放簇**;槽位**位置预知**⇒ 设备只要
`(data_offset, size_field_offset)` 就能覆写。

设备端**只碰这两处字节**:簇数据(`memcpy`)+ 根目录项的 size 字段(4 字节定点写,小端)。
设备**不碰**的:FAT 表(不分配、不释放、不改簇链)、根目录项的其余字段
(名字、首字节、属性、时间戳、簇链字段)。槽位偏移与镜像同源
(构建期由 `tools/mkfatimg.py` 产出)。

### 槽位表(构建期产出,固件与宿主同源)

`tools/mkfatimg.py --reserve-slot NAME:CAP --reserve-inbox NAME:CAP --emit-slot-table`
产出 `ramdisk_slots.h/.c`,固件编译进只读表。**槽位簇从卷末尾往前分配**
(普通文件从卷开头往后分配,两个分配器不相遇),簇链与目录项写进镜像、
**簇内容不写进镜像** → 镜像停在元数据区(97 扇区 / 49,664 B),
固件按 BPB `total32` 开满卷并补零尾部 ⇒ **槽位容量在固件里占 0 字节**。

| 槽位 | 类别 | 容量 | 用途 |
|---|---|---|---|
| `WATCH.PS1` | 设备管理槽 | 32 KB | watcher 引导脚本(M2 注入拉起) |
| `AGENT.SH` | 设备管理槽 | 32 KB | 目标机侧 shell 引导 |
| `AGENT.EXE` | 设备管理槽 | 64 KB | 目标机侧可执行引导 |
| `PAYLOAD.BIN` | **宿主信箱** | 128 KB | 宿主写、设备轮询的下发通道 |

每条记录含 `name / kind / capacity / data_offset / size_field_offset / start_cluster`,
`size_field_offset` = 根目录项里 size 字段的字节偏移(设备端定点写这 4 字节,小端)。

### flash 快照格式(A/B)

`script0` / `script1` 各 512 KB,同一分区里放**一份全量快照**:

```
64B  头   magic "IOAGPAY1" / version / seq / count / dir_crc / 40B reserved
32B×N 记录  slot_index / len / crc32 / data_off / 16B reserved   (N ≤ 8)
512B 对齐   payload 数据块
```

**A/B 语义**:`seq` 大者胜,但 magic + `dir_crc` + 每条 payload 的 crc32 **全过**才算有效;
写入永远写**非当前生效**的那个分区 ⇒ 写到一半掉电不影响当前生效的那份,**无需额外提交标记**。
CRC32 自己实现(反射 0xEDB88320),已逐字节验证与宿主 `zlib.crc32` 一致。

### 信箱协议(MCP 未就绪时的等价下发通道)

宿主往 `PAYLOAD.BIN` **原位覆写**(必须 `r+b`,不能用 `wb` 截断——截断会让
Windows 释放原簇链另分配,设备按构建期算好的簇偏移就读不到了):

```
64B 头  magic "IOPB" / version / slot_index / len / crc32 / flags(bit0=commit) / 36B reserved
       payload 从偏移 64 起,后面补零到信箱容量(131072 B)
```

设备 200 ms 轮询信箱头,发现变化 → CRC 校验 → **落 flash**(下次开机铺到槽位)。
开机首轮会比对 flash 已有快照,内容相同则跳过擦写(**幂等**)。

**M5 落地后,MCP `deploy_payload` 直接调 `io_payload_store(slot, bytes)`**,
信箱只是它的等价前置通道,协议本身不用改。

## 二之三、注入的安全语义(2026-10-22 事故后定案)

**往用户机器注入按键是对用户环境的破坏性操作**,不是普通开发步骤:
目标机若处于**锁屏**,注入的按键会被锁屏界面当成 PIN 输入 → 持续失败 → 触发
Windows 账户防暴力锁定(实测事故:设备每 3 秒重试一轮,目标机被锁)。

因此:

| 约束 | 落地 |
|---|---|
| 开机**不自动**注入 | `IO_LAUNCHER_AUTOSTART` 编译期开关**默认 0** |
| 只能被**显式**触发 | 远程 `act` 工具(有人明确下令) |
| **单次执行、零重试** | 失败不自动重发 —— 对"打系统快捷键"这类动作,重试 = 持续破坏 |
| 自检失败**立即中止** | 不退避重试 |
| 键盘基线 | 每次注入结束回到基线(无残留按键);鼠标 clamp ±127 |
| 验收环境 | 虚拟机 / 专用靶机;在主力机上做只能"人在场 + 知情 + 手动单次触发" |

`act` 默认在注入**前后各观察一次**并把两个 URL 一起返回 ——
**由 agent 自己判断"指令有没有生效"**,而不是靠设备日志里的"已发送"。

## 二之四、HTTP 端点与工具(2026-10-22 落地)

设备侧 MCP server(`firmware/io_mcp/`),Streamable HTTP + JSON-RPC 2.0:

| 端点 | 方法 | 用途 |
|---|---|---|
| `/mcp` | POST | JSON-RPC 2.0(`initialize` / `tools/list` / `tools/call`) |
| `/ping` | GET | 最小存活探针(不构造 JSON 就能确认 HTTP 层活着) |
| `/blob/b0..b3` | GET | 观察帧原字节(4 个固定槽位;通配 URI 在 IDF 5.5 上不匹配,用精确路径) |

工具:`device/info` / `act` / `observe` / `watcher/status` / `deploy_payload`。

- **`observe`** 读目标机上 watcher 写的快照文件(`SNAP_P.JPG` / `SNAP_A.JPG` / `SNAP_R.JPG` /
  `DIFF.IDX`),**设备不解码、不比对,只搬运**(哑设备原则);`target` 传任意 8.3 文件名
  时直接读根目录里那个文件(也能用来验证 `find_file`/`read_file`)。
- **`act`** 动作:`type` / `tap` / `hotkey` / `move` / `click` / `scroll` /
  `macro(focus_reset | launch_watcher)`。链路不可用时直接报错,不盲发。
- **`deploy_payload`** 落 flash,**下次开机生效**(真源/副本契约)。

**当前实现状态**:明文 HTTP;凭据在 NVS,**用 UART0 控制台配网**
(`wifi scan` / `wifi set <ssid> [pass]` / `psk set <psk>` / `name set <n>` / `status`),
凭据**不进仓库、不进固件二进制**。**mDNS 发现与 TLS-PSK 双向鉴权待做**
(候选见 `implementation-plan.md` 决策 13)。

验收方式用**裸 HTTP 构造**(不依赖 MCP 客户端):`tools/mcp_probe.py`。
注意 **PowerShell 把 JSON 传给 `curl.exe` 会吃掉双引号**(报 `invalid JSON`),
所以用 Python 发或 `--data-binary @file`。

### 实测(2026-10-22,N16R8 实板)

`tools/verify_t2.py` 端到端 **17 项全绿**,关键三条证据:

```
下发前  WATCH.PS1 (20452, a7ffef…)   AGENT.SH (134, f0d29…)
下发 →  script0 seq=9 → script1 seq=10        (A/B 交替成立)
下发后  WATCH.PS1 (20452, a7ffef…)   AGENT.SH (134, f0d29…)   ← 逐字节未变
复位后  boot active: script1 seq=10 → 槽位内容 = 新下发内容(sha256 一致)
```

即:**运行期下发不碰副本**(第 3 行 == 第 1 行),**开机才铺开**(第 4 行)。
未动的 `AGENT.EXE` 始终保持出厂态 0 字节。M1 回归在同一固件上全绿。

**代价与边界(必须如实标注)**:

- 新 payload 超过槽容量 → **报错,不就地扩容**(扩容要设备端写 FAT,违背零 FAT 原则)。
- 宿主 FAT 缓存可能给旧内容 → 缓解:deploy → apply → inject 顺序 + **复位后重新枚举**(缓存清空)。
- 宿主若删除槽文件 → 簇会被宿主回收 → 设备端**当前只按固定偏移写,没有校验 FAT1
  与出厂镜像的差异**,被动过时写入的是孤儿簇、宿主看不到。**这是已知缺口**,
  补法是写槽前比对 FAT1(需先实现只读 FAT 解析),量产前必须补。
- payload 最终是目标机上执行的代码 → 量产前需加签名校验(sha256 只挡传输损坏,挡不了恶意替换)。
- `script0/1` 写入要考虑 flash 擦写寿命:实测**每次部署擦整个 512KB 分区,耗时约 2.3 s**,
  高频部署需要磨损均衡策略(当前靠 A/B 交替摊薄)。

## 三、需求裁定(N1-N7)

| # | 裁定 | 说明/修正 |
|---|---|---|
| N1 ROI 一等公民 | 采纳 | 裁剪在 **watcher** 做(有真像素),ESP32 不解码 JPEG |
| N2 帧差只回传变化区 | 采纳,**位置修正** | diff 在 **watcher** 计算。原提案"固件侧"与哑设备定位矛盾(解码+逐像素比对是内容级工作);ESP32 只搬运 diff 清单 |
| N3 act 后帧差验证闭环 | 采纳 | MCP 标准流;杜绝盲注入 |
| N4 注入前置宏 | 采纳,**扩充** | `focus_reset` = **IME 归零(Shift 点切 EN)** → Win+M → Win+R。中文输入法会把 HID 字母吃进组合窗(高危,提案未覆盖);宏参数化,调用方不自己拼 |
| N5 长命令 base64 -enc | **降级为兜底** | 架构已化解:长内容=盘上文件,命令行恒短(~60 字符);仅无盘异常阶段用打字投递 |
| N6 多设备编址 | 采纳 | 设备 UUID 走 **mDNS 实例名 + MCP 会话标识**,一机一名(MQTT 方案已随拓扑反转作废) |
| N7 JPEG 质量可调 | 采纳 | 验收场景 quality≥90 或无损;压缩边界曾被视觉模型误读为"缝隙"(实测教训) |

## 四、鼠标注入与光标可见性(2026-10-20 补)

### 端点:键鼠合并,不额外占端点

键盘与鼠标放进**同一个 HID 接口**,用 Report ID 区分(Report ID 1 = 键盘,
Report ID 2 = 鼠标)→ 接口仍只占 1 个 IN(+1 个可选 OUT),**不增加端点消耗**。
拆成两个独立 HID 接口要多花 2 个端点(总数顶到 7、富余归零),**不采纳**。

**实测已确认(2026-10-24,N16R8 实板 + Windows)**:主机**会**按报告描述符把单 HID 接口
拆成两个 HID 集合——`MI_00&COL01` = HID Keyboard Device、`MI_00&COL02` = HID-compliant mouse,
两个集合都独立可用。所以"合并键鼠"在主机侧**照样拿到两个独立设备的语义**,
而不必多花 2 个端点。方案 B 作废,A′ 落地。

### 注入前置条件:链路活性必须查 `tud_ready()`,不能查 DETACHED 事件

**实测定案(2026-10-24)**:ESP32-S3 的 USB 是 Synopsys DWC2 内核,TinyUSB 的 DWC2 device
模式**不实现设备断开检测**,物理拔线只会让总线进入 **SUSPEND**,
`TINYUSB_EVENT_DETACHED` **永不触发**(详见 `hardware-selection.md` 决策摘要第 7 条)。

对 `act` 注入链路的直接影响:

| 项 | 结论 |
|---|---|
| 注入前置检查 | 用 `tud_ready()`(`mounted && !suspended`),**不要**用 `TINYUSB_EVENT_DETACHED` 派生的标志 |
| 拔线时 | `suspended=1` → `tud_ready()=false` → 注入应返回 `HW_ERR`(链路不可用),而不是假装成功 |
| 重新插上 | 主机发 SOF → `suspended` 被清 → 链路自动恢复(实测 140 ms),设备无需复位 |
| `tud_hid_ready()` | **不能**直接当链路判据:它含 `usbd_edpt_busy` 项,注入瞬间端点忙也会为 false,会误报掉线 |

> **M2 实现坑(已定位,M1 阶段不改)**:`firmware/io_hid/io_hid.c` 现在的注入守卫是
> `if (!tud_hid_ready()) return ESP_ERR_INVALID_STATE;`。而 `tud_hid_ready()` 含
> `usbd_edpt_busy()` 项 —— 上一个 HID 报告还没被主机取走时它就是 false。
> Windows 的 HID 轮询间隔约 1 ms,一旦注入步长压到 10 ms 以内、且一步要连发
> press+release 两个报告,第二次发送就会撞上 busy → 直接返回 `ESP_ERR_INVALID_STATE`,
> **动作序列会在中途断掉**。M2 落地时必须改成"等 ready"而不是"报错":
> 以 `tud_ready()` 做链路/挂起检查(去掉 busy 项),再用有上限的短等待轮询
> `tud_hid_ready()` 直到端点空闲再发,并把超时映射成 `BUSY` 状态。
> 当前 M1 固件无注入流量,不影响本轮验收。

固件侧现状(`firmware/io_usb/io_usb.c` 提供 `io_usb_link_alive()` / `io_usb_mounted()` /
`io_usb_suspended()`,`main.c` 的状态行每 200 ms 采样、5 s 打一次完整状态):

```
status: link=1 mounted=1 suspended=0 enumerated=1 hid_ready=1 sectors=8192 written=66 blocks / 33792 bytes
```

### 坐标模式(默认相对,绝对为增强项)

| 模式 | 说明 | 代价 |
|---|---|---|
| **相对(默认)** | 标准鼠标报告,只有 dx/dy | "点中具体像素"要靠步进试错;但与本项目闭环哲学相容:`act → observe(diff) → 决策`,视觉反馈足以修正 |
| 绝对(后续增强) | Generic Desktop/Mouse + 绝对逻辑范围(**不用 Digitizer 页**,否则 Windows 当成触摸) | 需目标机主屏分辨率(`CAP.TXT` 的 `screen` 字段已有);多显示器只在主屏范围有效 |

### 光标不可见的坑(必须处理)

GDI `CopyFromScreen` **不包含鼠标指针**(光标是独立覆盖层,不在帧缓冲里)。
agent 看到的是一张"没有光标的屏幕",无法知道光标当前位置 → 容易误判成"点歪了"并反复瞎试。

**零成本解法(watcher 侧)**:`GetCursorPos` 取光标坐标,**在截图上合成光标标记**
(十字或圆环),或把光标坐标写进 `SNAP.IDX` 随帧返回。
后续 DXGI Desktop Duplication 路径可直接选"带光标",但一代不依赖它。

## 五、评审补充(提案未覆盖,决定成败)

1. **UAC 安全桌面:手通眼盲,但有降级阶梯(2026-10-20 修订:不是断点,是 1-3s 盲窗)**。
   物理 HID 与真实键盘同栈,输入能到达安全桌面;UAC 弹窗键盘可导航——管理员
   账户默认按钮即"是"。降级阶梯:
   - **盲过(默认)**:act = 触发提权 → 延时 ~2s → Enter/Alt+Y;期间 observe
     上报 `secure_desktop`,agent 明知盲行,弹窗后恢复观察。闭环只慢 ~3s。
   - **一次提权持久受益**:盲过拉起提权级驻守,后续提权免弹窗;同提权级下
     UIPI 解除,能力面扩大。
   - **策略级根治(可选,默认否)**:提权后设 ConsentPromptBehaviorAdmin=0
     (从不通知)——改注册表留痕,违反无痕基线,仅长期驻留环境经批准使用。
   - **标准用户凭据盲打(默认不支持)**:需在设备配置存管理员口令,安全面
     扩大,留作受控选项。
   - **远控为何"能过 UAC"(2026-10-20 补充)**:不是无视规则,是安装时注册了
     SYSTEM 服务(那次 UAC 用户亲手点过"是"),服务在 Winlogon 桌面挂辅助进程
     (SYSTEM 在安全桌面 DACL 白名单)实现捕获+注入,多数另有驱动级键鼠注入。
     反证:RustDesk 便携版(不起服务)同样看不见 UAC/锁屏——盲区与我们一致。
     即插即用零安装 = 永无 SYSTEM = 安全桌面止于盲过,这是定位取舍非技术缺陷;
     未来若扩展"常驻安装模式"再走服务+驱动路线。
2. **采集方式(2026-10-20 定案:一代走 GDI,游戏场景后置)**:
   - 快捷键/GDI(PrtSc、CopyFromScreen、Win+Shift+S):真独占全屏
     (D3D9 FSE、**DirectDraw**)黑屏;现代游戏(Win10"全屏优化"翻转合成)多可拍。
   - **DXGI Desktop Duplication(Win8+):独占全屏官方支持路径**(OBS 显示捕获
     同款),普通 2D/游戏/独占全覆盖。
   - **定案:一代以传统桌面用途为准,watcher 采集实现选定 GDI `CopyFromScreen`**
     (Windows 自带、零安装、最快)。DXGI Desktop Duplication **后续可插拔**——
     采集封装留接口缝,替换实现不动协议与固件。
   - 取舍理由(2026-10-21 作者定调):DirectDraw 属**遗留特例**,现代软件极少
     使用;一代首要目标是**兼容多数桌面场景**,游戏/独占场景待条件成熟再以
     DXGI 插件支持。窗口化运行的游戏(含 sango)GDI 即可拍摄,仅真独占全屏
     需要 DXGI。
   - 一代已知边界(如实记录,非缺陷):真独占全屏可能黑帧;DRM 保护内容与
     安全桌面谁都拍不到(设计如此,见上条)。
   - 副作用(正向):传统办公场景画面变化小 → **ROI 截图价值更高**(N1),
     分层快照(滚动预览 + 变化 ROI)成为带宽收敛的主力手段。
     见 `design-review.md` 结论 1/2。
3. **act 时序 <10ms/步**:ESP32 本地定时器保证注入节奏;OS 侧处理延迟不计入。

## 六、验收判据(产品级)

- 手:act 一次下发,本地序列执行,<10ms/步
- 眼:全屏 ≤1s(L1 预算 ~350ms);ROI 回传再降一个量级
- 闭环:act→observe(diff)→决策,无人工、无 agent 侧对目标机的软件依赖
- 状态诚实:observe 三态(正常帧/无信号/黑帧)+ secure_desktop,不误报
