# WorkBuddy Connector 包 — Sophon

作者：**网瘾老登干点啥**　·　产品：Sophon（AI Agent 的智子）

上架用包。规范依据：[open.workbuddy.cn/docs/connector](https://open.workbuddy.cn/docs/connector)（2026-11 核对）。

## 作者署名放在哪

| 位置 | 字段 | 用户在哪看到 |
|---|---|---|
| `connector-meta.json` | `author` | **连接器详情页**（市场里最显眼的位置） |
| `skills/sophon/SKILL.md` | frontmatter `author` + 文末署名 | Agent 加载技能时；技能详情页 |
| 本 README | 抬头 | 提交审核的人打开 zip 就能看到 |
| **设备小屏** | 品牌屏位图 | 插上 dongle 的人**物理看到** |

设备小屏那一条是 2026-11-16 补的，做法见 `docs/screen-decision.md`：
中文在**宿主**渲染成 2bpp 位图（`tools/gen_brand_screen.py` → `firmware/io_lcd/brand_screen.c`），
固件只解包 —— **固件里没有也不需要中文字库**。原来"屏上不做中文"的结论依然成立，
变的是"中文可以以像素形式上去"。


```
wb-connector/
├── connector-meta.json     # 元信息（双语、source、auth_mode）
├── mcp.json                # MCP 连接参数（stdio + ${VAR} 注入）
├── token-schema.json       # 用户自填凭证表单
├── icon.svg                # 市场图标（四色锁死主题）
├── skills/sophon/SKILL.md  # 教 AI 怎么用这八个工具
└── validate.py             # 提交前自检（19 项）
```

## 现在的状态

已就位：

- **传输选型**：`stdio` + 本地桥（`bridge/sophon_bridge.py`）。**不用远程 HTTP**，理由见下。
- **token 模式**：`auth_mode: "token"` + `token-schema.json`，占位符走 `env`。
- **自检**：`python wb-connector/validate.py` → **19/19 通过**。
- **桥实测**：`python bridge/test_bridge.py --endpoint http://<ip>:8080/mcp` → **8/8 通过**
  （真机：initialize / tools/list 八个工具 / status / draw / 未知工具 -32602）。

还没做（上架前必须解决）：

| # | 事项 | 说明 |
|---|---|---|
| 1 | **桥的分发** | `mcp.json` 用的是 `python -m sophon_bridge`，需要先 `pip install sophon-bridge`。包里 `bridge/pyproject.toml` 已备好，但**还没发布到 PyPI**（需要 PyPI 账号 + 你的授权）。另一条路是声明 `mcpServers[].runtime`（官方 4.24.0 变更记录里提到，但没有公开的字段文档）——**这条需要向平台确认**，我不猜 |
| 2 | **设备侧鉴权** | 当前固件是明文 HTTP、无鉴权（README 里挂着的 P0/P3/P5）。`token-schema.json` 已经收了 PSK 并作为 `Authorization: Bearer` 发出，但**固件还没校验**。上公开市场前应该补上 |
| 3 | **审核时长** | 第三方评测明确指出官方 review 流程没有公开文档，排期前需联系运营确认（/docs/contact） |
| 4 | **注入类工具的市场合规** | 这个连接器能"真的按键操作一台电脑"。开发者公约 2.3/2.8 对功能与权限有要求，提交前值得先问平台：注入类工具是否需要额外的安全说明或限制 |

## 字段与版本的一个硬约束（2026-11 核实后修正）

`name_zh` / `name_en` / `examples_zh` / `examples_en` 都是 **4.24.0** 才有的字段。
第一版我写了 `minWorkbuddyVersion: 4.23.0`（token 模式的下限）却用了双语字段 ——
那是**不一致的**：老客户端会拿到读不懂的元信息。现在统一声明 **4.24.0**
（既满足 token 的 ≥4.23.0，也覆盖双语字段）。`validate.py` 的 C12/C13 就是查这个，
免得下次改字段忘了改版本。

另外：示例字段名是 `examples_zh` / `examples_en`，**不是**裸 `examples`。


## 关键决策：为什么是 stdio 而不是远程 HTTP

`mcp.json` 支持三种传输（stdio / sse / streamableHttp）。选 stdio 的原因：

1. **设备 IP 是 DHCP 的** —— 远程 HTTP 的 `url` 要写死地址。`${VAR}` 虽然能出现在
   url 里（官方文档确认，百度地图就是把 AK 拼进 url 的），但让用户去查设备 IP、
   并且每次续租变了还要手动改，体验不可接受。桥内置 mDNS，自动跟上。
2. **设备在局域网里** —— 远程连接器要求公网可达 / HTTPS，而 dongle 是插在用户
   自己网里的硬件，不该被要求暴露到公网。
3. **零安装** —— stdio 由客户端拉起，用户不用装任何东西（除 `pip install`）。

**代价**：必须有本机 Python。这是上表第 1 项要解决的事。

## token 模式怎么运作（官方口径）

- `connector-meta.json` 写 `auth_mode: "token"`，并配 `token-schema.json` 描述表单字段；
- 用户在 WorkBuddy 里安装连接器时看到表单，填**设备名**（必填）和密钥（可选）；
- 值存在**本机** `~/.workbuddy`，**不经过云端**；
- 连接时 WorkBuddy 把 `${VAR}` 替换成用户填的值，**可出现在 url / headers / env 三处**；
- token 模式不需要实现 OAuth 端点，也不会打开浏览器；
- 最低客户端版本 **4.23.0**（token 模式的要求）；
- **占位符与表单 `fields[].key` 一一对应、区分大小写**（`validate.py` 的 C5 就是查这个）。

## 提交步骤

1. `python wb-connector/validate.py` —— 自查必须全绿
2. 打包**这个目录的内容**（不含 `.git/`、`__pycache__/`、本 README、`validate.py`）
3. 到 open.workbuddy.cn 入驻（个人主体可选「工具」类目）
4. 提交审核 → 通过后转「待发布」→ 点发布上架
5. 更新时递增 `connector-meta.json` 的 `version` 重新提交

## 本地先验（不走上架，今天就能做）

WorkBuddy 桌面端 → 侧边栏「连接器」→「自定义连接器」→「配置 MCP」，
临时用**绝对路径**直连桥（不改包，只为本机验证）：

```json
{
  "mcpServers": {
    "sophon": {
      "type": "stdio",
      "command": "python",
      "args": ["D:\\Lab\\IO_agent\\bridge\\sophon_bridge.py"],
      "env": { "SOPHON_NAME": "sophon-dongle" }
    }
  }
}
```

注意三个已知坑（社区实测）：

- 配置文件是 **`~/.workbuddy/mcp.json`（不带点）**；带点的 `.mcp.json` 是系统生成的，改了会被覆盖
- 写完要**完全退出 WorkBuddy 再启动**，不支持热更新
- 启用后要**点「信任」**，否则 AI 看不到这些工具
