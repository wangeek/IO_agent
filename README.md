# 远控智子（Sophon Remote）

> 给 AI agent 装上一只手和一只眼：它能在**另一台电脑**上真按键、真点鼠标，
> 并把那台机器的屏幕读回来。目标机**不装任何软件**，只插一个 USB 设备。

**不用读完这份文档。** 把下面这条丢给你的 agent（Claude Code / Cursor / WorkBuddy 均可）：

```
请读这个 README 并帮我把远控智子装好；每一步卡住就告诉我卡在哪：
https://raw.githubusercontent.com/wangeek/IO_agent/main/README.md
（国内备用：https://gitee.com/wangeek/IO_agent/raw/main/README.md）
```

---

## 如果你是 LLM agent，从这里开始

你的任务是**把第 3 节装完并验证**，不是夸这个项目。

规则（违反任何一条都算失败）：

1. **逐步执行，逐步核对**。每条命令都要拿到「期望输出」；对不上就停下向用户报告，不要跳步、不要猜。
2. **第 5 节是红线**。`input` 会向目标机**真实按键**，调用前必须获得用户明确同意。
3. **没硬件就是装不成**。设备不在网、或用户根本没有设备时，如实告知并停在第 0 步，**不要编造步骤、不要用别的东西冒充**。
4. 所有命令在**运行 WorkBuddy 的这台机器**上执行；设备插在**另一台**（目标机）上。

---

## 0. 这是什么，能不能用

远控智子是一台插在**目标机** USB 口上的小设备（ESP32-S3）。它替 agent 长出手和眼：

- **手** —— 真键盘 + 真鼠标（USB HID），按键走硬件，目标机零软件安装；
- **眼** —— 目标机把画面写进设备的虚拟 U 盘，agent 读回来；
- **嘴** —— 设备自带 160×80 小屏，能给站在机器前的人显一行字。

链路：`agent ⇄ stdio 桥（本仓库 bridge/）⇄ mDNS 发现 ⇄ 设备上的 MCP server`。
全程在你局域网里，**不需要公网、不需要域名**。

| 你需要 | 说明 |
|---|---|
| 远控智子设备 ×1 | 没有它什么都做不了（见第 8 节） |
| 一台跑 agent 的电脑 | 装 Node ≥18（`node --version` 应 ≥ `v18`） |
| 目标机 | 要被操作的那台 Windows，有一个空闲 USB 口 |
| 两者同一局域网 | mDNS 是组播，**不跨路由/跨网段** |

## 1. 先自检三条前置

在运行 agent 的机器上逐条跑：

| # | 命令 | 期望输出 |
|---|---|---|
| 1 | `node --version` | `v18` 或更高 |
| 2 | `npx --version` | 有版本号即可 |
| 3 | （确认设备已上电：目标机上它的指示灯应亮，小屏显示信息） | — |

任何一条不满足，先解决再继续。

## 2. 设备上线确认

设备上电后会自动连 WiFi 并在局域网广播。**等小屏出现设备信息再继续**（约 10 秒）。

验证（任选其一）：

- 看**目标机**的设备管理器 / 文件管理器：应出现一个名为 `SOPHON` 的可移动磁盘；
- 或在运行 agent 的机器上探测（PowerShell）：

```powershell
npx -y https://gitee.com/wangeek/IO_agent/releases/download/sophon-bridge-v0.2.3/sophon-bridge-0.2.3.tgz --version
# 期望输出：0.2.3
```

> 这条只是验证桥能跑起来，**不代表设备在线**。

## 3. 安装四步

### 3.1 验证桥可用

```powershell
npx -y https://gitee.com/wangeek/IO_agent/releases/download/sophon-bridge-v0.2.3/sophon-bridge-0.2.3.tgz --version
# 期望输出：0.2.3
```

### 3.2 验证能发现设备并列出工具

```powershell
$env:SOPHON_NAME='sophon-dongle'
'{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | npx -y https://gitee.com/wangeek/IO_agent/releases/download/sophon-bridge-v0.2.3/sophon-bridge-0.2.3.tgz
# 期望输出：一段 JSON，里面有 "tools"，共 8 个：
#   status, ls, read, capture, input, update, flash, draw
```

对不上说明设备不在网：见第 6 节排查表。

### 3.3 写进客户端配置

WorkBuddy：侧边栏「连接器」→「自定义连接器」→「配置 MCP」，贴入：

```json
{
  "mcpServers": {
    "sophon": {
      "type": "stdio",
      "command": "npx",
      "args": ["-y", "https://gitee.com/wangeek/IO_agent/releases/download/sophon-bridge-v0.2.3/sophon-bridge-0.2.3.tgz"],
      "runtime": { "type": "node", "version": ">=18.0.0" },
      "env": { "SOPHON_NAME": "sophon-dongle" },
      "timeout": 30000
    }
  }
}
```

其它客户端（Claude Desktop / Cursor 等）：把上面这段放进它们的 MCP 配置文件即可，
字段同名。**手贴配置时 `SOPHON_NAME` 写字面值**（只有从市场安装的连接器才会做
`${VAR}` 替换；手贴不替换，写错就会拿 `${SOPHON_NAME}` 这个字面量去查设备，
表现是每次调用都超时 30 秒）。

### 3.4 重启客户端并「信任」

改完配置必须**完全退出客户端再启动**（WorkBuddy 改 `~/.workbuddy/mcp.json`
后尤其如此）；启动后在连接器面板点**「信任」**，否则 agent 看不到工具。

## 4. 装好的判据

让 agent 调一次 `status`，期望（真实样例，来自真机）：

```json
{
  "ok": true,
  "device": "sophon-dongle",
  "ip": "172.17.21.231",
  "fw": "m2-remote",
  "wifi_connected": true,
  "usb": { "mounted": true, "suspended": false, "link_alive": true },
  "led": 0,
  "busy": false
}
```

三条硬判据：`ok:true`、`usb.link_alive:true`（目标机在链路里）、`led:0`（就绪）。

想看画面：调 `capture`，会返回一张目标机桌面的图片。
（第一次用要先 `input {"action":"macro","macro":"launch_watcher"}` 拉起目标机上的驻守脚本，见第 5 节。）

## 5. 红线：注入是破坏性操作

`input` 工具会向目标机**真实按下键盘鼠标**。三条不可协商的规则：

1. **调用前必须获得用户明确同意**，尤其目标机不是你自己的机器时。
   目标机若在锁屏，按键会被当成 **PIN 输入**，连续失败会触发 Windows 账户保护
   （锁账户，实测发生过）。
2. **设备端零自动重试**。`input` 回 `busy` 说明上一个动作还在跑，**等
   `status.busy` 变 false，不要重发**；连续重发 = 持续破坏。
3. **第一次拉画面走这个序列**（并先征得同意）：

```
input {"action":"macro","macro":"launch_watcher"}   ← 拉起目标机驻守脚本
status  → watcher.running == true                    ← 轮询确认
capture → 返回目标机桌面画面                          ← 之后随时可看
```

时长参数不要往下调（组合键 600ms、单键 60ms 是实测下限；更短会被 Windows
整段丢弃，或让中文输入法打出乱码）。

## 6. 排查表（症状 → 怎么判 → 怎么修）

| 现象 | 先判什么 | 处理 |
|---|---|---|
| `no Sophon device ... found over mDNS` | 设备是否上电、与这台机器是否同一网段、`SOPHON_NAME` 是否 `sophon-dongle` | 三者查完仍失败 = 设备离线，告知用户 |
| 每次调用都超时 30 秒 | 配置里是不是写了 `${SOPHON_NAME}` 字面量 | 手贴配置写字面值 `sophon-dongle`（见 3.3） |
| `tools/list` 只有一半工具或报协议错 | 客户端有没有重启、是否点了「信任」 | 完全退出重启 + 点信任 |
| `status` 能通但 `capture` 总是同一张图 | `watcher.running` 是否 true；目标机是否锁屏 | 没拉起先走第 5 节序列；锁屏则请人解锁 |
| `input` 回 `busy` | 上一个动作没跑完 | 轮询 `status.busy`，**不要重发** |
| `draw` 报 no display | 该设备固件没开屏 | 正常，跳过 |

**Windows 专项**：系统解析器解析不了 `.local` 域名 —— 任何把
`http://sophon-dongle.local:8080/mcp` 写进配置的方案都会失败。发现设备一律走
桥内置的 mDNS（`SOPHON_ENDPOINT` 是给已知固定 IP 用的例外）。

## 7. 它做不到什么（别让用户误以为）

- **不是远程桌面**：不给交互式画面回传，只有「截屏快照 + 键鼠注入」；
- **不能穿内网/公网**：设备与运行 agent 的机器必须在同一局域网；
- **目标机必须有空闲 USB 口**且是 Windows（驻守脚本目前是 PowerShell）；
- **小屏不出中文**（160×80 只装得下 ASCII）；改设备名后要**重启设备**才生效；
- **设备固件明文 HTTP、无鉴权**：局域网内其它主机理论上也能调它——
  别在不可信网络里用（鉴权在路线图上，见 `docs/decisions.md` P3/P5）。

## 8. 没有设备怎么办

**装不了，就此打住。** 这个项目的价值在真硬件：没有它，agent 没有手也没有眼，
第 3 节的 3.2 起会一直报 `no Sophon device ... found over mDNS`。

此时请如实告诉用户：需要先有一台远控智子设备（ESP32-S3 + 固件，固件与全部源码
都在本仓库，可自行制作），不要用「模拟器」「远程桌面」之类的话术搪塞。

## 9. 版本与来源

| 项 | 值 |
|---|---|
| 当前桥版本 | **sophon-bridge 0.2.3**（tag `sophon-bridge-v0.2.3`） |
| tarball | 9344 字节，sha256 `8376ea450e9f5099014d99f2263785dc8e2658a996df7d45d38711413284fc4f` |
| 主分发 | Gitee Release（国内 `npx` 端到端实测 3 秒） |
| 兜底分发 | GitHub Release（国内常 10 秒以上或超时，只作存档） |
| 许可证 | MIT |
| 作者 | 网瘾老登干点啥 |

> 本包**没有发布到公共 npm**——`npx -y sophon-bridge`（不带 URL）拉不到东西，
> 这是有意的：不用占 npm 账号，tarball 直发两个 Release。

---

## 给人看的部分

**仓库结构**：`firmware/`（ESP32-S3 固件，含 ST7735S 屏、WS2812 灯、HID/MSC 复合设备、
mDNS、OTA）、`bridge/`（上面的 stdio 桥）、`wb-connector/`（WorkBuddy 连接器上架包）、
`tools/`（打包/发布/真机验收脚本）。

**想深入了解设计取舍**：[`docs/decisions.md`](docs/decisions.md) 收录了全部架构决策与
实现记录（含每条弯路）；术语见 [`docs/glossary.md`](docs/glossary.md)；
接口全表见 [`wb-connector/skills/sophon/SKILL.md`](wb-connector/skills/sophon/SKILL.md)。

**自制硬件**：主控 ESP32-S3（DevKitC-1-N16R8 实板验证过），固件用 ESP-IDF v5.5.6 构建，
烧录与配网见 `docs/`。

---
产品名 **远控智子**（Sophon Remote）· 作者 **网瘾老登干点啥**
