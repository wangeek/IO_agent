# sophon-bridge

把 [Sophon](https://github.com/wangeek/sophon) 硬件棒的 MCP 服务器接成 **stdio**
形式，并解决两个真问题：

1. **设备 IP 会变**（DHCP）—— 桥每次用时现找，走 mDNS 自动发现；
2. **桌面 Agent 要本地进程**（不要求设备公网可达 / 有证书）—— 桥提供 stdio 传输。

## 设计上的一条硬约束：透明代理

桥**不重新实现**任何工具语义。它把 stdin 收到的 JSON-RPC 原样 `POST` 给设备的
`/mcp`，把回包原样写回 stdout。所以：

- 设备端契约（工具集、schema、`isError` 语义）永远是唯一真源；
- 桥不会随设备固件演进"漂移"；
- 设备新增工具，桥**不用改一行代码**。

## 安装

```bash
pip install sophon-bridge
```

只用标准库，没有第三方依赖（这是为了在任何机器上都能装上）。

## 单独用

```bash
sophon-bridge                      # 发现设备并阻塞读 stdin
sophon-bridge --name sophon-dongle # 指定设备名
sophon-bridge --endpoint http://172.17.21.231:8080/mcp   # 跳过 mDNS
sophon-bridge --psk <key>          # 设备密钥（可选）
```

手动喂一条 JSON-RPC 试试：

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | sophon-bridge
```

## 环境变量

| 变量 | 含义 |
|---|---|
| `SOPHON_NAME` | 设备名（默认 `sophon-dongle`） |
| `SOPHON_PSK` | 设备密钥，作为 `Authorization: Bearer` 发出去 |
| `SOPHON_ENDPOINT` | 直接指定端点，跳过 mDNS |

## 接到 MCP 客户端

```json
{
  "mcpServers": {
    "sophon": {
      "type": "stdio",
      "command": "sophon-bridge",
      "env": { "SOPHON_NAME": "sophon-dongle" }
    }
  }
}
```

## 自测

```bash
python test_bridge.py                                  # 走 mDNS 发现
python test_bridge.py --endpoint http://<ip>:8080/mcp  # 指定端点
```

8 项检查：`initialize` / `tools/list` / `status` / `draw` / 未知工具错误码。

## 已知限制

- **必须和 dongle 在同一个二层网络**（mDNS 是组播，不走路由）。
- 如果设备端将来强制 PSK 校验，`--psk` 已经就位，桥不用改。
- 设备离线时报 `no Sophon device ... found over mDNS`；先确认它插着通电。
