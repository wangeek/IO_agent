## 远控智子 · Sophon Remote

WorkBuddy 连接器用的桥 `sophon-bridge` 0.2.2。`npx -y` 直接跑这个 tarball 即可拉起（MCP + stdio，不需要发公共 npm）。

### 这个版本修了什么

- **未替换的 `${VAR}` 占位符回退默认值。** 桌面端「自定义连接器」手贴配置时不做变量替换，之前会把字面量 `${SOPHON_NAME}` 当成设备名去查。
- **mDNS 不再把陌生设备当成 Sophon。** 严格按 `<name>.<service>` / `<name>.local` 匹配，删掉了「只有一个 A 记录就凑合用」的兜底。

### 文件

| 文件 | 大小 | sha256 |
| --- | --- | --- |
| `sophon-bridge-0.2.2.tgz` | 8777 B | `ca70feac21965560b4ce809292c9e9eb2f4c9ccf6e8641f0c5687aebe9399e3e` |

### 用起来

```
npx -y <这个 tarball 的下载地址> --version   # 应回 0.2.2
```

设备是一根 ESP32-S3 硬件棒（单 Type-C），插在**目标机**上，自己跑 MCP server；桥负责 mDNS 发现并做透明代理。固件与宿主工具同在本仓库。

---
作者：网瘾老登干点啥
