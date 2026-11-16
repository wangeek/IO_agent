---
name: sophon
descriptions:
  zh-Hans: |
    操作插着 Sophon 硬件棒的机器：注入键鼠、读回屏幕。当用户想"操作那台电脑"、
    "看那台机器的屏幕"、"远程点一下"、或提到 dongle / Sophon 时使用。
  en: |
    Drive the machine that has the Sophon dongle plugged in: inject keyboard
    and mouse, read back its screen. Use when the user wants to operate, watch
    or automate "that other machine", or mentions the dongle / Sophon.
---

# Sophon — 实体机器的眼与手

Sophon 是一根 ESP32-S3 硬件棒，插在**目标机**上，同时扮演：

- **手**：一个真键盘 + 真鼠标（USB HID）。按键走的是硬件，不依赖目标机装任何软件。
- **眼**：目标机把抓到的画面写进棒子的虚拟 U 盘，agent 再读回来。
- **嘴**（可选）：棒子自带一块 160×80 小屏，能把一行字显示给站在机器前的人看。

你（Agent）通过本连接器与它对话。设备就在用户的局域网里，**不需要公网**。

## 先确认三件事，再动手

1. **目标机是哪台** —— dongle 插着的那台。它和运行 WorkBuddy 的这台通常不是同一台。
2. **注入是破坏性操作** —— 它会真的按键。若目标机锁屏，按键会被当成 PIN 输入，
   连续失败会触发账户保护。**除非用户明确说这是靶机/测试机，先问一句。**
3. **只读工具无需确认** —— `status` / `ls` / `read` / `capture` 随时可用。

## 八个工具

| 工具 | 作用 | 会动到目标机吗 |
|---|---|---|
| `status` | 身份、USB/HID 链路、LED 状态码、槽位表、watcher 是否在跑、OTA 状态 | 否 |
| `ls` | 列共享盘（虚拟 U 盘）里的文件 | 否 |
| `read` | 按 8.3 文件名读一个文件 | 否 |
| `capture` | 取**最新**一帧画面（MCP image 块） | 否 |
| `input` | 注入**一个**动作（键 / 鼠标 / 宏） | **是** |
| `update` | 把一段内容写进设备的 flash 槽位（下次开机生效） | 否 |
| `flash` | 换设备的固件（OTA，设备自己去下载） | 否 |
| `draw` | 往设备自带小屏写 ASCII 文字 | 否 |

## 典型流程：让目标机开始"供图"

设备自己看不到目标机的屏幕。目标机上要跑一个**驻守脚本**（watcher），
它负责截屏、写进虚拟盘、并维持 `CAP.TXT` 存活标记。拉起它需要一次注入：

```
input(action="macro", macro="launch_watcher")
```

之后：

```
status            -> watcher.running == true
capture           -> 拿到画面
```

**设备端零自动重试**。失败了就是失败了，要不要再来一次由你判断 ——
连续重试会把目标机打进账户保护。

## 注入的硬约束（都是真机踩出来的，别绕过）

- **一次一个动作**。设备是单例：有动作在跑时新请求回 `busy`，不排队。
- **长动作异步**：`macro` / 长文本立刻回 `{"accepted":true}`，结果看盘上的标记文件
  （`INJ.TXT` / `CAP.TXT`）—— 轮询 `status.busy` 转 false 再去 `read`。
- **IME 会自动探**：新开的运行框继承上一次的输入法状态，Shift 是 toggle、
  方向未知 ⇒ 设备会先跑一个无害探针判断当前是中文还是英文态，再决定打字方式。
  你**不需要**自己传 `ime` 参数。
- **组合键按住 ≥600ms、单键 ≥60ms**：更短会被 Windows 整段丢弃，或让拼音打出乱码。
- **运行框会预填历史命令**（且非选中态）⇒ 设备侧已经先 `Ctrl+A` 全选，别改这个顺序。
- **插着 dongle 的机器锁屏时**：抓屏会停止产出（画面上是安全桌面），
  注入也可能打不进去。看到 `capture` 内容不变、`INJ.TXT` 不更新，先想到锁屏。

## 设备自带的小屏（`draw`）

站在目标机前的人不用看聊天窗口就知道 agent 在干什么：

```
draw(text="SETUP WIZARD\nstep 2/5")
```

- **只能 ASCII**，没有中文字模。屏是 160×80，每行 26 个字符、共 10 行。
- `\n` 换行，过长会自动折行。
- 会**覆盖**屏上原有内容（包括开机信息屏）。
- 目标机画面缩略图在流的时候，这段文字会作为"字幕"压在图上重绘。

## 输出约定

每个工具结果都带三样东西：

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
| `draw` 报 no display | 这台设备没开屏（编译期开关关闭） |

## 不要做的事

- 不要在用户没同意的情况下向活动机器注入按键。
- 不要连续重试失败的注入（账户保护）。
- 不要试图用 `flash` 去改目标机 —— 它换的是 **dongle 自己**的固件。
- 不要指望设备替你解读画面：它只搬像素，理解是模型的事。
