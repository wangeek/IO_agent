# sophon-bridge —— 远控智子的 stdio 桥

把**远控智子**（Sophon Remote）设备的 MCP 服务器接成 **stdio** 形式，解决两个真问题：

1. **设备 IP 会变**（DHCP）—— 桥每次用时现找，走 mDNS 自动发现；
2. **桌面 Agent 要本地进程** —— 桥提供 stdio 传输，WorkBuddy 用 `npx` 拉起。

远控智子是一台插在目标机 USB 口上的小设备，自己跑 MCP server（HTTP）并向局域网
广播 `_ioagent._tcp.local`。桥只做发现与转发，不解读任何工具语义。

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

本包**没有发布到公共 npm**，所以不要写 `npx -y sophon-bridge` —— 那个名字拉不到东西。
正确用法是把 tarball 的地址直接交给 `npx`：

```bash
npx -y <tarball URL>                                    # mDNS 发现设备
npx -y <tarball URL> --name sophon-dongle               # 指定设备名
npx -y <tarball URL> --endpoint http://192.168.1.9:8080/mcp
npx -y <tarball URL> --psk <key>
```

本地调试直接用目录或本地 tarball 也行：

```bash
npx -y "D:\Lab\IO_agent\bridge" --version
npx -y "./sophon-bridge-0.2.3.tgz" --version
```

零第三方依赖，只用 Node 内置模块（`dgram` / `http` / `readline`），要求 Node ≥18。

## 环境变量

| 变量 | 含义 |
|---|---|
| `SOPHON_NAME` | 设备名（默认 `sophon-dongle`，这是设备的出厂广播名，不是产品名） |
| `SOPHON_PSK` | 设备密钥，作为 `Authorization: Bearer` 发出去 |
| `SOPHON_ENDPOINT` | 直接指定端点，跳过 mDNS |
| `SOPHON_SERVICE` | mDNS 服务类型（默认 `_ioagent._tcp.local`） |

未替换的 `${VAR}` 会回退到默认值：桌面端「自定义连接器」手贴配置时**不做变量替换**，
不这样处理就会拿字面量 `${SOPHON_NAME}` 去查设备、然后超时 30 秒。

## mDNS 的三个坑（都踩过，别简化）

1. **必须绑 5353，并逐个本机 IP 入组**（`addMembership(addr, ip)`）。
   用 `0.0.0.0` 入组在多网卡机器上常常收不到；从临时端口发查询也收不到应答。
2. **必须发多种查询**：`<name>.local` 的 A/ANY + `_ioagent._tcp.local` 的 PTR/SRV/ANY。
   只发一种时，本项目的 lwIP responder 常常不回。
3. **必须解析 answer / authority / additional 三段**。本项目设备的应答是
   `an=0`、记录全在 **authority** 里（实测抓包 `qd=2 an=0 ns=2 ar=0`）——
   只遍历 `an` 会永远解出 0 条记录，症状是"能抓到包但找不到设备"。

另外：**只按名称匹配，不做兜底**。曾经写过"只有一个 A 记录就凑合用"，结果把局域网上
一个陌生设备（`192.168.20.2:10443`）当成了远控智子。现在严格按
`<name>.<service>` / `<name>.local` 比对。

DNS 名字压缩指针要**先前进 2 字节再记 `end`，然后才跳**；写成
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

真机验收 8 项（mDNS 发现 / initialize / tools/list 八个工具 / status / draw /
未知工具 -32602 等）见仓库 `bridge/test_bridge.py`，用指定端点或 mDNS 均可。

## 已知限制

- 必须和远控智子在同一二层网络（mDNS 是组播，不走路由）。
- 设备离线时报 `no Sophon device ... found over mDNS`；先确认它插着通电。
- 发现失败有 10 秒冷却，避免"设备没插"时每条调用都白等一次扫描。

---

产品名 **远控智子**（Sophon Remote）· 作者 **网瘾老登干点啥**
仓库：https://github.com/wangeek/IO_agent
