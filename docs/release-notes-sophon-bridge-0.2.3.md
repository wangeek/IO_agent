## 远控智子 · 桥 0.2.3

WorkBuddy 连接器「远控智子」用的 stdio 桥。`npx -y` 直接跑这个 tarball 即可拉起（MCP + stdio，不需要发公共 npm）。

### 这一版修了什么

上一版（0.2.2）的包内文字没跟产品改名同步，还带了几处硬错：

- **`npx -y sophon-bridge` 拉不到东西。** 这个包从没发过公共 npm（`registry.npmjs.org/sophon-bridge` 返回 404）。正确用法是把 tarball 地址交给 `npx`。
- **仓库地址写错。** `package.json` 与 README 链接都指向 `github.com/wangeek/sophon`，真实仓库是 `wangeek/IO_agent`。
- **包内一处「远控智子」都没有。** 三个文件统一改成中文名 + `Sophon Remote`；说明文字里不再出现「硬件棒 / dongle」。`sophon-dongle` 只作为**设备出厂广播名**保留（绑定层标识，改了发现不到设备）。
- **真机验收项数写错。** README 说 4 项，实际 8 项。
- **报错串里的措辞。** `is the dongle powered...` 改成 `is the device powered...`（这句会出现在 WorkBuddy 日志里）。
- 补写 mDNS 的一条实测约束：**只按名字匹配，不做「只有一个 A 记录就凑合用」的兜底** —— 之前那条兜底把局域网上一个陌生设备当成了远控智子。

### 文件

| 文件 | 大小 | sha256 |
| --- | --- | --- |
| `sophon-bridge-0.2.3.tgz` | 9344 B | `8376ea450e9f5099014d99f2263785dc8e2658a996df7d45d38711413284fc4f` |

### 验收

```
node --check sophon_bridge.js                        # 通过
npx -y ./sophon-bridge-0.2.3.tgz --version           # 0.2.3
python bridge/test_bridge.py                         # 8/8（真机）
```

### 用起来

```
npx -y <这个 tarball 的下载地址> --version   # 应回 0.2.3
```

远控智子是一台插在**目标机** USB 口上的小设备，自己跑 MCP server（HTTP）并向局域网广播 `_ioagent._tcp.local`；桥负责 mDNS 发现并做透明代理（原样转发 JSON-RPC，不重新实现任何工具语义）。固件与宿主工具同在本仓库。

---
作者：网瘾老登干点啥
