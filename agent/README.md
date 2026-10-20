# agent/ — Agent 侧

跑在 agent 端的设备管理与 MCP 服务。本地或云端均可——回传路径按
`docs/connectivity-and-binding.md` §五 运行时择一。

## 职责

1. **设备绑定与鉴权**:生成一次性绑定码 → 校验后下发长期 token → 按 `device_uuid` 查表鉴权;
   支持单独吊销某台设备。
2. **回传接收**:TLS(PSK)服务端,解帧
   `magic|ver|type|seq|device_uuid|len|payload|tag`,校验 tag 与 seq 防重放。
3. **截图与响应入库**:预览帧 / 全帧 / ROI 图块 / `RESP` 结果。
4. **MCP server**:首发三个原语
   - `act(sequence)` — 动作序列(按键/鼠标 + 内嵌延时)一次下发,ESP32 本地按序执行(<10ms/步)
   - `observe(opts)` — `opts ⊆ {全屏, roi(x,y,w,h), quality, diff}`
   - `file/exec` — 预留,不进首发

   标准闭环:`act → observe(diff/roi) → 决策`,全链无人工。

## observe 状态诚实(不误报)

三态 + 一标记:`FRAME`(正常帧)/ `NO_SIGNAL`(无信号,如休眠)/ `BLACK`(黑帧,如锁屏)/
`SECURE_DESKTOP`(UAC 安全桌面,盲行期间)。锁屏与黑屏必须区分,不能都报"无画面"。

## 国际化

MCP 工具描述、CLI 输出、面向用户的报错文案一律走 zh-Hans + en 双语资源,
禁止硬编码;协议层字段保持 ASCII。
