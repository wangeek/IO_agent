---
name: sophon
author: 网瘾老登干点啥
version: "0.3.0"
descriptions:
  zh-Hans: |
    操作插着智子（Sophon）硬件棒的机器：注入键鼠、读回屏幕。当用户想"操作那台电脑"、
    "看那台机器的屏幕"、"远程点一下"、或提到 dongle / 智子 / 远控智子时使用。
  en: |
    Drive the machine that has the Sophon dongle plugged in: inject keyboard
    and mouse, read back its screen. Use when the user wants to operate, watch
    or automate "that other machine", or mentions the dongle / Sophon / 智子.
---

# 远控智子（Sophon）— 实体机器的眼与手

Sophon 是一根 ESP32-S3 硬件棒，插在**目标机**上，同时扮演：

- **手**：一个真键盘 + 真鼠标（USB HID）。按键走的是硬件，目标机不用装任何软件。
- **眼**：目标机把画面写进棒子的虚拟 U 盘，agent 再读回来。
- **嘴**：棒子自带一块 160×80 小屏，能把一行字显示给站在机器前的人看。

你通过本连接器暴露的 MCP 工具与它对话。设备在用户局域网里，**不需要公网**。

## 先确认三件事，再动手

1. **目标机是哪台** —— dongle 插着的那台。它和运行 WorkBuddy 的这台通常不是同一台。
2. **注入是破坏性操作** —— 它会真的按键。若目标机锁屏，按键会被当成 PIN 输入，
   连续失败会触发账户保护。**除非用户明确说这是靶机/测试机，先问一句。**
3. **只读工具无需确认** —— `status` / `ls` / `read` / `capture` 随时可用。

## 八个工具

### `status` — 无参数

返回身份、USB/HID 链路、LED 状态码、flash 槽位表、watcher 是否在跑、OTA 状态。

| 字段 | 含义 |
|---|---|
| `device` / `ip` | 设备名与当前地址 |
| `usb.link_alive` | 目标机是否真的在链路里（`mounted && !suspended`） |
| `led` | 0 就绪 / 1 启动 / 2 无网 / 3 动作中 / 4 故障 |
| `busy` | 有动作在执行时为 true |
| `watcher.running` | 目标机上的驻守脚本是否在跑 |

### `ls` — 无参数

列共享盘（虚拟 U 盘）里的文件：名称、大小、最后写入时间、是否帧。

### `read` — 读一个文件

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `name` | string | 是 | 8.3 文件名，如 `SNAP_P.JPG`、`INJ.TXT` |

文本内联返回，图片返回 image 内容块，超过 64 KB 给 `/files/<name>` URL。

### `capture` — 无参数

取**最新**一帧（按最后写入时间挑），返回 MCP image 内容块 + 文件名/大小/时间。
这是"看那台机器屏幕"的调用。

### `draw` — 往设备小屏写文字

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `text` | string | 是 | 要显示的文字，**只能 ASCII**，`\n` 换行 |

会覆盖屏上原有内容；目标机画面在流时，这段字会作为字幕压在图上面重绘。

### `input` — 注入一个动作（破坏性）

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `action` | string | 是 | `type` / `tap` / `hotkey` / `move` / `click` / `scroll` / `macro` |
| `text` | string | — | `action=type` 时要打的文字 |
| `key` | string | — | `action=tap/hotkey` 的键名 |
| `modifiers` | string[] | — | 要按住的修饰键：`ctrl` `shift` `alt` `win` |
| `dx` / `dy` | number | — | 鼠标位移（`move` / `scroll`） |
| `button` | string | — | `left` / `right` / `middle` |
| `macro` | string | — | `focus_reset` / `selftest` / `launch_watcher` / `ime_toggle` |
| `hold_ms` | number | — | 按键按住时长，默认 60 |
| `per_char_ms` | number | — | `type` 的逐字符时长，默认 60 |

### `update` — 写 flash 槽位

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `slot` | number | 是 | 0 `WATCH.PS1` / 1 `AGENT.SH` / 2 `AGENT.EXE` |
| `text` | string | — | 文本载荷 |
| `base64` | string | — | 二进制载荷（与 `text` 二选一） |

**下次开机**才生效 —— RAM disk 里那份是开机铺开的副本。

### `flash` — 换设备自己的固件（OTA）

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `url` | string | 是 | 固件镜像的 http(s) 地址 |

设备**去下载**镜像、写进另一个 OTA 槽、校验通过才切启动分区然后重启。
应答立刻返回，几秒后连接会断 —— 那是预期行为。

## 典型流程：让目标机开始"供图"

设备自己看不到目标机的屏幕。目标机上要跑一个**驻守脚本**（watcher），
负责截屏、写进虚拟盘、维持 `CAP.TXT` 存活标记。拉起它需要一次注入：

```
input(action="macro", macro="launch_watcher")
```

之后：

```
status     → watcher.running == true
capture    → 拿到画面
```

**设备端零自动重试**。失败了就是失败了，要不要再来一次由你判断 ——
连续重试会把目标机打进账户保护。

## 注入的硬约束（都是真机踩出来的，别绕过）

- **一次一个动作**。设备是单例：有动作在跑时新请求回 `busy`，不排队。
- **长动作异步**：`macro` / 长文本立刻回 `{"accepted":true,"busy":true}`，
  结果看盘上的标记文件（`INJ.TXT` / `CAP.TXT`）—— 先轮询 `status` 到 `busy:false`，
  再 `read` 标记文件。
- **IME 会自动探**：新开的运行框继承上一次的输入法状态，Shift 是 toggle、方向未知
  ⇒ 设备先跑一个无害探针判断当前是中文还是英文态，再决定打字方式。
  你**不需要**自己传 `ime`。
- **组合键按住 ≥600ms、单键 ≥60ms**：更短会被 Windows 整段丢弃，或让拼音打出乱码
  （`hold_ms` / `per_char_ms` 可调，不要往下调）。
- **运行框会预填历史命令**（且非选中态）⇒ 设备侧已经先 `Ctrl+A` 全选，别改这个顺序。
- **插着 dongle 的机器锁屏时**：抓屏会停止产出，注入也可能打不进去。
  看到 `capture` 内容不变、`INJ.TXT` 不更新，先想到锁屏。

## 输出格式

每个工具结果带三样东西：

- `structuredContent` —— 机器可读的载荷，**读这个**
- `content[0].text` —— 同样内容的文本块（不含 base64）
- `isError` —— 工具本身失败时为 `true`

**两者都要看**：`isError:true` 时载荷里通常还有 `ok:false` 和 `error`。
未知工具名不属于工具失败，而是协议层错误（JSON-RPC `-32602`）。

## 连不上时按这个顺序查

| 现象 | 先看什么 |
|---|---|
| 报 "no Sophon device ... found over mDNS" | dongle 是否插着通电、是否和本机同一网段；设备名是否填错 |
| `status` 能通但 `capture` 一直同一张图 | 目标机上的 watcher 没在写新帧，或目标机锁屏 |
| `input` 回 `busy` | 另一个动作在跑。等 `status.busy` 转 false，不要重发 |
| 注入"成功"但目标机没反应 | 先看 `INJ.TXT` 有没有更新；再看运行框是否被别的窗口抢了焦点 |
| `draw` 报 no display | 这台设备没开屏（固件编译期开关关闭） |

## 不要做的事

- 不要在用户没同意的情况下向活动机器注入按键。
- 不要连续重试失败的注入（账户保护）。
- 不要试图用 `flash` 去改目标机 —— 它换的是 **dongle 自己**的固件。
- 不要指望设备替你解读画面：它只搬像素，理解是模型的事。

---

作者：**网瘾老登干点啥**　·　产品：远控智子（Sophon，AI Agent 的智子）
遇到问题或想反馈，请直接告诉用户联系作者本人。
