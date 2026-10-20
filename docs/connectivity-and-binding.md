# 配网、绑定与回传设计 v1(2026-10-20)

回答一个问题:**一块插进陌生机器的 ESP32,靠什么连到 agent,又靠什么证明"它是我的"。**

来源:design-review(2026-10-20)对 README"WiFi 带外第一公民"这条决策的落地补全。
修订权:本文与 output-channel-design.md(通道容量/延迟预算)、README(架构决策表)并读;
**README 决策表与本文冲突时以本文为准**。

## 一、三条带外性约束(决定方案形态,不可绕)

1. **ESP32 不能向主机发起任何数据。** USB 主从架构 + FAT 缓存使"设备 → 主机"方向不存在
   (见 output-channel-design.md §一)。推论:所有"主机 → ESP32"的输入必须由主机**主动写入**;
   配网**不能**设计成"ESP32 拉取配置",只能设计成"主机推送到盘上"。
2. **USB CDC 兜底在硬件上不存在。** ESP32-S2/S3 USB 控制器共 **7 个端点(含 EP0)**、
   最多 5 个 IN(含 EP0),即 6 个可配置端点(Espressif esp-usb device guide,
   https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_device.html)。
   HID(2: IN+OUT)+MSC(2: bulk IN+OUT)=4,CDC-ACM 需 3(interrupt IN + bulk IN + bulk OUT),
   7 > 6 → **塞不下**。结论:回传只有 WiFi 一条硬件路,想再开一路,只能从"目标机自己的网络"借。
   (README 决策表"HID + MSC,4/6 端点,剩 2 富余"与官方数据一致,无需改。)
3. **无痕基线禁止在目标机装服务或写盘。** 因此"配网"这一步必须**不碰目标机**完成
   → 天然指向"出厂预置"或"用手机",两者都不动目标机。

## 二、方案总览

```
                  ┌──────────── 目标机(零安装) ────────────┐
                  │  HID 注入  ──►  敲 Win+R 拉起 watch.ps1 │
                  │  MSC 主机写 ──►  RAM disk: 脚本/快照/命令 │
                  └────────────────────┬────────────────────┘
                                       │  watch.ps1 读盘 / 写盘
                        ┌──────────────┴──────────────┐
                        │  ESP32-S3                   │
                        │  · 读自己的 RAM disk        │
                        │  · 本地定时执行注入序列      │
                        │  · WiFi 回传 / SoftAP 配网  │
                        └──────────────┬──────────────┘
                                       │ TLS(mTLS/PSK)
                              ┌────────┴────────┐
                              │  agent 端        │
                              │  · 设备绑定/鉴权 │
                              │  · 截图入库      │
                              │  · MCP server    │
                              └─────────────────┘
```

两级配网(出厂预置 / SoftAP 门户)+ 运行时分级回传。

## 三、两级配网

### Level 0 — 出厂预置(默认路径,自用阶段)

- NVS 分区存 `wifi_ssid` / `wifi_pass` / `agent_host` / `agent_port` / `device_token` /
  `device_uuid`。烧录时一并写入(cfg 分区,或用 esptool write_flash 写 NVS)。
- 上电流程:读 NVS → 有有效配置 → 连 WiFi → 连 agent → 进 Stage 0。
- 适配:个人自用、少量设备、部署点固定(机房/工位/固定工控机)。
- 密钥管理:token 只存 NVS,日志与抓包中一律脱敏;每台设备独立 token,不共用。

### Level 1 — 首次/换网(SoftAP 配置门户)

触发条件:NVS 无有效网络配置,或连接失败超过 N 次。

1. ESP32 启动 SoftAP(SSID `IOAGENT-<uuid前4>`,默认开放或简单口令)+ 极简 HTTP 服务。
2. 用户用**手机**连该热点 → 浏览器打开 `http://192.168.4.1/` → 单页表单:
   WiFi SSID/密码、agent 端点(host/port)、绑定码。
3. ESP32 校验后写入 NVS → 关闭 SoftAP → 重连 → 进 Stage 0。

要点:

- **全程不碰目标机**:手机是独立设备,目标机零参与、零落盘,符合无痕基线。
- 目标机上唯一的可见痕迹是那个 U 盘盘符(插着就在)。"无痕"应如实理解为
  **"不落盘、不留服务/计划任务"**,而非"不可见"。
- 配置文件类内容**一律进 NVS(flash)**,RAM disk 每次上电重建(内容必丢),只放脚本/快照/命令。

## 四、绑定(把设备认领到某个 agent)

1. agent 端生成**一次性绑定码**(8 位短码,短时效,单次消费)。
2. 用户把绑定码填进 SoftAP 配置页(或出厂预置时由烧录工具写入)。
3. ESP32 上报 `device_uuid` + 绑定码 → agent 端校验 → 下发长期 token。
4. ESP32 存 NVS,后续每次回传携带 `device_uuid` + token,agent 侧按 UUID 查表鉴权。

N6("设备 UUID 进回传包头/MQTT topic")在此落地为具体的握手步骤。
agent 端应支持**吊销**:token 可单独吊销而不影响其他设备。

## 五、回传:运行时分级,不是设计期二选一

| 运行时条件 | 回传路径 | 性质 | 延迟量级 |
|---|---|---|---|
| ESP32 能连上 agent(经它自己的 WiFi) | ESP32 → WiFi → agent | **真带外**:与目标机网络无关,目标机断网/隔离网仍可用 | ~30ms(控制)+ 上行 |
| ESP32 连不上,但目标机有网 | L2:watch.ps1 直推 agent 端点 | 依赖目标机网络;快,天然过 NAT | ~300ms |
| 两者都不行 | 纯本地:agent 只能 act(注入) | 能力降级,**不是死机** | observe 不可用 |

**定位修正(重要)**:README 原写"WiFi 带外通道为第一公民"。经端点预算与真实运维场景
复核,应改为:

- **带外(ESP32 自己的 WiFi)的真正价值 = 目标机完全隔离/无网时的唯一通路**;
  这是"带外"一词唯一站得住的场景,应明确写进定位,不要泛化宣称。
- **L2 不是"加速",而是"最可能成功的主路径"**:真实运维场景里目标机通常有网,
  而 ESP32 常连不上外网(企业网、隔离网、需认证的 WiFi)。
- 故正确表述是:**"ESP32 带外 + L2 直连,两条都要有;运行时哪条通走哪条。"**

## 六、传输安全

- TLS:ESP32-S3 支持 mTLS;采用 **PSK 模式**成本最低(无需证书链与 CA 管理)。
- 只连预置端点;**不做 mDNS/广播**,避免暴露设备。

**帧格式(冻结建议 v1)**:

```
magic(2)=0xAA55 | ver(1) | type(1) | seq(4) | device_uuid(16) | len(4) | payload | tag(16)
```

- `tag`:AES-GCM 附加认证标签(或 HMAC-SHA256);`seq` + 时间戳窗口防重放。
- `type`:`HELLO / HELLO_ACK / CAP / FRAME_META / FRAME_DATA / CMD / RESP / HEARTBEAT / ERROR`。

## 七、量产与产线约束(由一条官方限制推导)

Espressif 文档明确:**启用 flash encryption 或 secure boot 会禁用 ROM 中的 USB-OTG 栈**,
该口将无法再用于串口仿真/DFU 更新(同见上文 usb_device 文档页)。

对本项目的直接含义:**本项目的 USB 口就是产品主接口(HID+MSC)**,因此

1. 量产固件**必须支持 WiFi OTA**,不能把"USB 重新烧录"当作升级路径;
2. 开发期**不开 flash encryption / secure boot**,否则每次固件更新都要全量擦除(丢密钥);
3. 若后期确需加密存储,密钥管理与 OTA 流程必须先设计,再开 eFuse。

## 八、bootstrap 阶段修订

README 原 bootstrap 为 Stage 0/1/2,补一个前置阶段:

- **Stage -1 配网与绑定**:出厂已预置 → 跳过;否则 SoftAP 门户 + 手机配置(唯一人工步骤)。
- **Stage 0 连通性探测**(修订):USB 枚举 + HID 通路 + agent 链路连通性。
  **LED 往返从 Stage 0 的必选项降为可选加分项**(详见 design-review 结论 4)。
- Stage 1 / Stage 2 维持原样(HID 拉起驻守 → 能力自陈)。
