---
name: sophon
author: 网瘾老登干点啥
version: "0.3.0"
descriptions:
  zh-Hans: |
    远控智子：把一台真机交给 agent —— 注入键鼠、读回屏幕、在小屏上留言。
    当用户说"操作那台电脑"、"看那台机器的屏幕"、"远程点一下"，或提到
    远控智子 / 智子 / Sophon 时使用。
  en: |
    Sophon Remote: hand a real machine to the agent -- inject keyboard and mouse,
    read its screen back, leave a note on its small display. Use when the user
    wants to operate or watch "that other machine", or mentions Sophon.
---

# 远控智子（Sophon）

远控智子是一台插在**目标机** USB 口上的小设备，替 agent 长出手和眼：

- **手** —— 一个真键盘加真鼠标（USB HID）。按键走硬件，目标机不装任何软件。
- **眼** —— 目标机把画面写进智子的虚拟 U 盘，agent 再读回来。
- **嘴** —— 智子自带一块 160×80 小屏，能给站在机器前的人显一行字。

设备在用户的局域网里，全程**不需要公网**。agent 通过本连接器暴露的 MCP 工具与它对话。

## 先确认三件事，再动手

1. **目标机是哪台** —— 智子插着的那台。它和运行 WorkBuddy 的这台通常不是同一台。
2. **注入是破坏性操作** —— 它会真的按键。目标机若锁屏，按键会被当成 PIN 输入，
   连续失败会触发账户保护。**除非用户明确说这是靶机/测试机，先问一句。**
3. **只读工具不用确认** —— `status` / `ls` / `read` / `capture` 随时可用。

## 工具一览（八个，全是动词，零分隔符）

| 工具 | 做什么 | 参数 | 会动目标机吗 | 返回方式 |
|---|---|---|---|---|
| `status` | 身份、USB/HID 链路、LED 码、flash 槽位、watcher、OTA | 无 | 否 | 立即 |
| `ls` | 列共享盘里的文件 | 无 | 否 | 立即 |
| `read` | 读盘上的一个文件 | `name` | 否 | 立即 |
| `capture` | 取最新一帧画面 | 无 | 否 | 立即 |
| `draw` | 往智子自带小屏写文字 | `text` | 否 | 立即 |
| `input` | 向目标机注入**一个**动作 | `action` + 按档位 | **是** | 短动作立即、长动作异步 |
| `update` | 写 flash 槽位（设备自身） | `slot` + `text`/`base64` | 否 | 立即，**下次开机**才生效 |
| `flash` | OTA 换智子自己的固件 | `url` | 否 | 立即应答，随后连接会断 |

## 参数总表

**全部参数都在这一张表里**，没列出来的就是不存在。

| 工具 | 参数 | 类型 | 必填 | 默认 | 取值 / 说明 |
|---|---|---|---|---|---|
| `read` | `name` | string | **是** | — | 8.3 文件名，如 `CAP.TXT`、`INJ.TXT`、`SNAP_P.JPG` |
| `draw` | `text` | string | **是** | — | **只能 ASCII**（无中文字库）；`\n` 换行，超宽自动折行；屏只有 26×10 字符 |
| `input` | `action` | string | **是** | — | `type` \| `tap` \| `hotkey` \| `move` \| `click` \| `scroll` \| `macro` |
| `input` | `text` | string | `type` 需要 | — | 要打的文字 |
| `input` | `key` | string | `tap`/`hotkey` 需要 | — | **单个字符**，不是键名。传 `"r"` 表示 R 键 |
| `input` | `modifiers` | string[] | `hotkey` 需要 | — | 按住哪些修饰键：`ctrl` \| `shift` \| `alt` \| `win`，如 `["ctrl","shift"]` |
| `input` | `dx` / `dy` | number | `move` 需要；`scroll` 用 `dy` | — | `move`：鼠标位移像素；`scroll`：滚轮格数，负数为向下 |
| `input` | `button` | string | `click` 需要 | — | `left` \| `right` \| `middle` |
| `input` | `macro` | string | `macro` 需要 | — | `focus_reset` \| `selftest` \| `launch_watcher` \| `ime_toggle` |
| `input` | `ime` | string | 否 | 不按 Shift | 只配 `macro=launch_watcher`：`"toggle"` = 打字前先按一下 Shift（宿主输入法在中文态时用） |
| `input` | `hold_ms` | number | 否 | **单击 60、带修饰键 600** | 按键按住时长。**不要往下调**：低于 600 的组合键会被 Windows 整段丢弃 |
| `input` | `per_char_ms` | number | 否 | 60 | `type` 的逐字符时长。低于 60 时拼音输入会打出乱码 |
| `update` | `slot` | number | **是** | — | `0` = `WATCH.PS1`（32K）｜`1` = `AGENT.SH`（32K）｜`2` = `AGENT.EXE`（64K） |
| `update` | `text` | string | 二选一 | — | 文本载荷 |
| `update` | `base64` | string | 二选一 | — | 二进制载荷（与 `text` 只能给一个） |
| `flash` | `url` | string | **是** | — | 固件镜像的 http(s) 地址，如 `http://192.168.1.5:8000/sophon.bin` |

## 七种 action 怎么填

| action | 必须给 | 可选 | 最小例子 |
|---|---|---|---|
| `type` | `text` | `per_char_ms` | `{"action":"type","text":"notepad"}` |
| `tap` | `key` | `hold_ms` | `{"action":"tap","key":"a"}` |
| `hotkey` | `key` + `modifiers` | `hold_ms` | `{"action":"hotkey","key":"r","modifiers":["win"]}` |
| `move` | `dx` + `dy` | — | `{"action":"move","dx":120,"dy":0}` |
| `click` | `button` | — | `{"action":"click","button":"left"}` |
| `scroll` | `dy` | `dx` | `{"action":"scroll","dy":-3}` |
| `macro` | `macro` | `ime`（仅 `launch_watcher`） | `{"action":"macro","macro":"launch_watcher"}` |

四个 macro 分别做什么：

| macro | 作用 |
|---|---|
| `focus_reset` | `Win+M` 收所有窗口，把桌面夺回焦点。**注入前必做** |
| `selftest` | 打一条自检命令，成功则盘上出现 `INJ.TXT` —— 用来判"按键真的到达目标机了" |
| `launch_watcher` | 拉起目标机上的驻守脚本（后续才有画面可 `capture`） |
| `ime_toggle` | 单按一下 Shift，只切输入法态、不打字符 |

## 返回什么

`status` 的字段（`structuredContent`，只列真实存在的键）：

| 字段 | 含义 |
|---|---|
| `ok` | 恒 `true`（工具自身失败走 `isError`） |
| `device` / `ip` | 设备名、当前地址 |
| `fw` / `app.version` / `app.build` | 固件标识与编译时间 |
| `wifi_connected` | WiFi 是否连上 |
| `uptime_s` / `sectors` | 已运行秒数、虚拟盘扇区数 |
| `usb.mounted` / `usb.suspended` / `usb.host_configured` / `usb.link_alive` | 链路四项。**判"目标机还在不在"只看 `link_alive`**（= `mounted && !suspended`） |
| `hid.endpoint_ready` / `hid.layout_ok` | HID 端点就绪、报告描述符自检通过 |
| `busy` | 有动作在执行。轮询它判断后台动作跑完没有 |
| `led` | `0` 就绪 / `1` 启动中 / `2` 网络未就绪 / `3` 动作执行中 / `4` 故障 |
| `slots[]` | 每个槽的 `name` / `kind` / `capacity` / `len` |
| `payload_seq` | 载荷版本号，写槽后 +1 |
| `watcher.running` / `watcher.cap_mtime` | 目标机驻守脚本是否在跑、`CAP.TXT` 的最后写入时间 |
| `ota.state` / `ota.received` / `ota.total` / `ota.error` | OTA 进度；`ota.boot` / `ota.next` 是启动分区与下次要切过去的分区 |
| `log.port` / `log.active` / `log.rx` / `log.tx` / `log.dropped` | 日志通道（没配 `logips` 时 `active:false`） |
| `free_heap` / `min_free_heap` | 当前余量、历史最低点 |

其余工具的返回：

| 工具 | 返回 |
|---|---|
| `ls` | `{ok, count, files:[{name,size,mtime,frame}]}`；`frame:true` 表示这张是画面帧。超过上限时多一个 `note:"listing truncated"` |
| `read` | `{ok, file, size, mtime}` 外加三选一：文本进 `text`；图片给 image 内容块（`mime:"image/jpeg"` + `image_b64`）；**超过 64 KB** 只给 `url` + `note:"too large to inline; fetch the url"` |
| `capture` | 最新一帧的 image 内容块 + `file` / `size` / `mtime` |
| `draw` | `{ok, lines}`（实际画了几行） |
| `input`（短动作） | `{ok, action:{...}}` |
| `input`（长动作） | `{ok:true, accepted:true, busy:true, note, action:{...}}` |
| `update` | `{ok, error?}` |
| `flash` | `{accepted:true}`，之后连接断 |

每个工具结果都带三样东西，**两样都要看**：

- `structuredContent` —— 机器可读载荷，读这个
- `content[0].text` —— 同样的文本副本（不含 base64）
- `isError` —— 工具本身失败时为 `true`；此时载荷里通常还有 `ok:false` 和 `error`

未知工具名不算工具失败，是协议层错误（JSON-RPC `-32602`）。

## 常见错误串

| `error` | 意思 | 怎么办 |
|---|---|---|
| `device busy (another action is running)` | 单例槽被占 | 轮询 `status.busy`，**不要重发** |
| `missing action` / `missing macro` / `unknown macro` | `input` 参数不全或 macro 名写错 | 对照上面的 action 表 |
| `missing name` | `read` 没给 `name` | 先 `ls` 看真实文件名 |
| `usb link is not alive (host suspended or unplugged)` | 目标机不在链路里 | 看是不是拔了/关机/睡眠 |
| `HID endpoint is busy (host is not polling it)` | 目标机没在轮询 HID | 通常还是链路问题，先 `status` |
| `job queue full` | 后台动作堆满 | 等 `busy` 转 `false` 再来 |

## 典型流程：让目标机开始"供图"

智子自己看不到目标机屏幕。目标机上要跑一个**驻守脚本**（watcher），负责截屏、
写进虚拟盘、维持 `CAP.TXT` 存活标记。拉起它要注入一次：

```
input  {"action":"macro","macro":"launch_watcher"}
status → watcher.running == true
capture → 拿到画面
```

**设备端零自动重试**：失败了就是失败了，要不要再来一次由你判断 ——
连续重试会把目标机打进账户保护。

## 注入的硬约束（都是真机踩出来的，别绕过）

- **一次一个动作。** 设备是单例：有动作在跑时新请求回 `busy`，**不排队**。
- **长动作异步。** `macro` 和长文本立刻回 `{accepted:true,busy:true}`，
  结果看盘上的标记文件（`INJ.TXT` / `CAP.TXT`）—— 先轮询 `status` 到 `busy:false`，
  再 `read` 标记文件。
- **IME 会自动探。** 新开的运行框继承上一次的输入法状态，Shift 是 toggle、方向未知，
  所以设备先跑一个无害探针判断当前是中文还是英文态，再决定打字方式。
  一般**不用**自己传 `ime`；只有在探针给出中文态、而你要用 `launch_watcher` 时，
  才配合 `{"ime":"toggle"}`。
- **时长不要往下调。** 组合键 600ms、单键与逐字符 60ms 是实测下限：
  更短会被 Windows 整段丢弃，或让拼音打出乱码（`ni hao` 变成一串"啊"）。
- **运行框会预填历史命令**（且非选中态）⇒ 设备侧已经先 `Ctrl+A` 全选，别改这个顺序。
- **插着智子的机器锁屏时**：抓屏会停止产出，注入也可能打不进去。
  看到 `capture` 内容不变、`INJ.TXT` 不更新，先想到锁屏。

## 连不上时按这个顺序查

| 现象 | 先看什么 |
|---|---|
| 报 `no Sophon device ... found over mDNS` | 智子是否插着通电、是否和本机同一网段、`SOPHON_NAME` 是否填错 |
| `status` 能通但 `capture` 一直是同一张图 | 目标机上的 watcher 没在写新帧，或目标机锁屏 |
| `input` 回 `busy` | 另一个动作在跑，等 `status.busy` 转 `false` |
| 注入"成功"但目标机没反应 | 先看 `INJ.TXT` 有没有更新（没有 = 按键没到达）；再看运行框是否被别的窗口抢了焦点 |
| `draw` 报 no display | 这台设备没开屏（固件编译期开关关闭） |
| `flash` 之后连不上 | 预期：设备在重启。重新发现它，核对 `app.build` 变了没有 |

## 不要做的事

- 不要在用户没同意的情况下向活动机器注入按键。
- 不要连续重试失败的注入（账户保护）。
- 不要试图用 `flash` 去改目标机 —— 它换的是**智子自己**的固件。
- 不要指望设备替你解读画面：它只搬像素，理解是模型的事。

---

作者：**网瘾老登干点啥**　·　产品：**远控智子**（Sophon Remote）
遇到问题或想反馈，请直接告诉用户联系作者本人。
