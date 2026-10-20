# MSC Watcher 协议 v0.1

> **状态(2026-10-20)**:本文档基于"主机侧常驻 watcher + MSC 双向文件交换"假设。
> v2 Output 设计(`output-channel-design.md`)已证伪"ESP32 → 主机"方向的 MSC 数据流
> (USB 主从架构 + FAT 缓存),回读链路改为"主机写 U 盘 → ESP32 本地读 → WiFi 上传"。
> 本文的 CMD/RESP 文件约定、幂等键、超时参数仍可平移到 v2 的 `X:\CMD\ / X:\RESP\`
> 与 L2 直连形态。以下原文仅存档。
>
> **两处再更新(2026-10-20 评审,见 `design-review.md`)**:
> - **LED 通道退出主路径**:下面"LED ACK 序列"整节**已作废**——3-9 bit/s、
>   macOS 不下发 LED 报告、v1 状态机把每次状态迁移都绑在 LED 上,
>   而 v2 已有 RESP 文件回执,LED 沦为纯冗余。LED 仅保留为可选调试/心跳信号,
>   Stage 0 不以 LED 成功为前提。
> - **安全基线平移,不随本文作废**:"安全基线"一节的 op 白名单、type ≤256B、
>   鼠标 clamp ±127、键盘基线恢复、检测到人动键盘即中止,已写入
>   `implementation-plan.md` §三与 M3 验收条目,按 v2 目录结构落位。

主机侧常驻服务(watcher)与 ESP32 之间,通过虚拟 U 盘上的文件交换命令与数据。
**回执不走文件,走 HID LED 3-bit 通道**(见 README)。

## 角色与方向

```
主机(watcher)                       ESP32-S3(MSC + HID)
     │  写 CMD 文件 (bulk OUT)          │ 读 CMD 文件
     │  ←─────────────────────────────  │
     │                                  │ 执行(注入按键/采集)
     │  ←── LED OUT 报告 (ACK, 3bit) ── │ 完成时闪 LED 序列
     │  读 RESP 文件 (bulk IN)           │ (仅主机写 RESP;ESP32 只读)
```

- 命令流:主机 → ESP32,文件 `CMD.TXT`
- 结果流:主机 ← ESP32,**但写方仍是主机**——ESP32 把结果放进一个固定 RAM 缓冲,
  主机 watcher 轮询 `RESP.TXT`(ESP32 只读导出),避免 ESP32 在 FAT 上建文件。

## 文件布局(虚拟盘根目录, FAT16, ≤32MB)

| 文件 | 写方 | 读方 | 内容 |
|---|---|---|---|
| `CMD.TXT` | 主机 | ESP32 | 当前待执行命令(JSON 单行) |
| `RESP.TXT` | 主机(watcher 从 ESP32 RAM 导出后写) | 无人 | 最近一次执行结果(JSON 单行) |
| `HEART.TXT` | 主机 | ESP32 | 时间戳,watcher 存活标志 |
| `CAP.TXT` | 主机预置 | ESP32 | 能力清单(见下) |

命名固定短名(8.3),避免 LFN 兼容问题。

## CMD.TXT 格式

```json
{"id":"a1b2c3","op":"type","args":{"text":"hello"},"deadline_ms":5000}
```

- `id`:主机生成的 6 hex 随机串。**幂等键**:ESP32 记住最近 8 个已执行 id,重复看到直接跳过。
- `op` 首版只支持:`type`(打文本)、`key`(单键+修饰键)、`move`(鼠标)、`click`、`noop`。
- `deadline_ms`:超时后 ESP32 丢弃该命令并报 `TIMEOUT`。

## 状态机(ESP32 侧)

```
扫描 CMD.TXT
  ├─ 不存在/大小未变 → idle (100ms 后再扫)
  ├─ 大小变了 → 读全文 → 解析 JSON
  │     ├─ 解析失败        → RESP: {"id":?, "st":"PARSE_ERR"}
  │     ├─ id 已见过       → 静默跳过(不重发 ACK)
  │     ├─ op 未知         → RESP: {"st":"BAD_OP"}
  │     └─ 通过            → 执行 → RESP: {"id":..., "st":"OK"} 或具体错误
  └─ 每次状态迁移 → LED 序列(见下)
```

`st` 枚举:`OK | PARSE_ERR | BAD_OP | TIMEOUT | BUSY | EXEC_ERR | HW_ERR`

## LED ACK 序列(3 bit:Num=1, Caps=2, Scroll=4)

ESP32 只能注入按键让**主机翻 LED**,不能直接写 LED。因此 ACK = ESP32 注入
`ScrollLock` N 次来编码,主机 watcher 数 LED 翻转:

| 事件 | ScrollLock 闪烁次数 |
|---|---|
| 命令收到 | 1 |
| 执行完成 | 2 |
| 任何错误 | 3 连闪(慢) |
| 心跳(每 30s) | 1 次慢闪 |

去抖 ~500ms/次 → 一次 ACK 最多 ~1.5s。主机 watcher **只把 LED 当"活着"信号**,
真正的结果以 `RESP.TXT` 内容为准。LED 通道探测失败(macOS 等)→ 降级:纯轮询,无 ACK。

## 超时与重试(主机侧 watcher)

- 写 CMD 后 2×deadline 内无 RESP → 视为失败,重发同一 id 最多 2 次。
- ESP32 靠 id 幂等,重发安全;若 ESP32 正在执行返回 `BUSY`,watcher 退避 1s 再查。
- watcher 每 30s 重写 `HEART.TXT`;ESP32 发现 90s 未更新 → 假定 watcher 死亡,
  停止接受命令,LED 慢闪报警,回到 Stage 0 等待重新 bootstrap。

## CAP.TXT(主机 watcher 启动时预置)

```json
{"os":"win11","arch":"x64","admin":false,"net":true,
 "audio_out":true,"serial":["COM3"],"screen":"1920x1080",
 "caps_lock_disabled":false,"watcher_ver":"0.1.0"}
```

ESP32 据此决定后续阶段(Stage 2 能力自陈的落盘形式)。

## 安全基线(不可省)

1. ESP32 侧命令白名单:`op` 不在表内一律拒绝。
2. `type` 文本长度上限 256 字节;鼠标坐标 clamp 到 ±127/报告。
3. 会话开始记录 LED baseline,会话结束恢复;中途发现 NumLock 意外变化 → 判定有人在
   动键盘 → 立即中止并重新同步。
4. CMD/RESP 循环缓冲各 1KB,溢出截断并置 `TRUNC` 标志。

## 验收清单(首版)

- [ ] Windows:echo 写 CMD → 3s 内 RESP 出现 OK,LED 闪 2 次
- [ ] 重发同 id → 无重复执行、无重复 ACK
- [ ] 拔 U 盘 → ESP32 5s 内回到 idle,不崩溃
- [ ] watcher 进程被杀 → 90s 后 ESP32 报警态
- [ ] type 注入含 Shift/特殊字符文本,主机收到的字符逐字节一致
