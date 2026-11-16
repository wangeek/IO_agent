# sophon-bridge

把 [Sophon](https://github.com/wangeek/sophon) 硬件棒的 MCP 服务器接成 **stdio**
形式，并解决两个真问题：

1. **设备 IP 会变**（DHCP）—— 桥每次用时现找，走 mDNS 自动发现；
2. **桌面 Agent 要本地进程** —— 桥提供 stdio 传输，WorkBuddy 用 `npx` 拉起。

## 为什么是 Node

WorkBuddy 的 `mcpServers[].runtime` **只支持 node**：

- 官方原文：「`runtime.type` 目前仅支持 `"node"`」
- 版本表里 `python` 只挂在 `CliConfig.runtime.type`（CLI 方案）名下
- 四个真实上架的连接器（canvas-agent / iform-survey / cdcleaner / qeubee）全部 `runtime:{type:"node"}`

而 MCP 方案的承诺是「用户无需额外安装」⇒ 只有 `npx` + 托管 node 这一条零安装路径。
（Python 版只能走 CLI 方案，那条路要求客户端 ≥5.0.0。）

## 设计上的一条硬约束：透明代理

桥**不重新实现**任何工具语义。它把 stdin 收到的 JSON-RPC 原样 `POST` 给设备的
`/mcp`，把回包原样写回 stdout。所以设备端契约永远是唯一真源，设备新增工具时
桥**不用改一行代码**。

## 安装 / 运行

```bash
npx -y sophon-bridge                       # mDNS 发现设备
npx -y sophon-bridge --name sophon-dongle
npx -y sophon-bridge --endpoint http://172.17.21.231:8080/mcp
npx -y sophon-bridge --psk <key>
```

零第三方依赖，只用 Node 内置模块（`dgram` / `http` / `readline`），要求 Node ≥18。

## 环境变量

| 变量 | 含义 |
|---|---|
| `SOPHON_NAME` | 设备名（默认 `sophon-dongle`） |
| `SOPHON_PSK` | 设备密钥，作为 `Authorization: Bearer` 发出去 |
| `SOPHON_ENDPOINT` | 直接指定端点，跳过 mDNS |
| `SOPHON_SERVICE` | mDNS 服务类型（默认 `_ioagent._tcp.local`） |

## mDNS 的三个坑（都踩过，别简化）

1. **必须绑 5353，并逐个本机 IP 入组**（`addMembership(addr, ip)`）。
   用 `0.0.0.0` 入组在多网卡机器上常常收不到；从临时端口发查询也收不到应答。
2. **必须发多种查询**：`<name>.local` 的 A/ANY + `_ioagent._tcp.local` 的 PTR/SRV/ANY。
   只发一种时，本项目的 lwIP responder 常常不回。
3. **必须解析 answer / authority / additional 三段**。本项目设备的应答是
   `an=0`、记录全在 **authority** 里（实测抓包 `qd=2 an=0 ns=2 ar=0`）——
   只遍历 `an` 会永远解出 0 条记录，症状是"能抓到包但找不到设备"。

另外 DNS 名字压缩指针要**先前进 2 字节再记 `end`，然后才跳**；写成
`end = 跳转目标` 会让后续资源记录整体错位。

## 接到 WorkBuddy（本地先验）

连接器 → 自定义连接器 → 配置 MCP：

```json
{
  "mcpServers": {
    "sophon": {
      "type": "stdio",
      "command": "node",
      "args": ["D:\\Lab\\IO_agent\\bridge\\sophon_bridge.js"],
      "runtime": { "type": "node", "version": ">=18.0.0" },
      "env": { "SOPHON_NAME": "sophon-dongle" }
    }
  }
}
```

改完要**完全退出 WorkBuddy 再启动**，并在连接器面板点**「信任」**。

## 自测

```bash
node sophon_bridge.js --version
echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | node sophon_bridge.js
```

真机验收（4 项：initialize / tools/list / status / 未知工具错误码）见仓库
`bridge/test_bridge.py`（用指定端点或 mDNS 均可）。

## 已知限制

- 必须和 dongle 在同一个二层网络（mDNS 是组播，不走路由）。
- 设备离线时报 `no Sophon device ... found over mDNS`；先确认它插着通电。
- 发现失败有 10 秒冷却，避免"设备没插"时每条调用都白等一次扫描。
