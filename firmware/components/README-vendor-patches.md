# 第三方组件 vendor patch 记录

本目录下的 `esp_tinyusb/` 与 `tinyusb/` 是 **vendored（随仓库走）** 的第三方源码，
不是从组件注册表在线拉取的。原因：需要离线可复现构建 + 需要对 esp_tinyusb 打一处
功能性补丁。

## 版本与来源

| 组件 | 版本 | 来源 | 说明 |
|---|---|---|---|
| `esp_tinyusb` | 2.4.0 | `https://components.espressif.com/api/components/espressif/esp_tinyusb/versions/2.4.0` → `components-file.espressif.com/.../espressif__esp_tinyusb-v2.4.0.zip` | ESP-IDF 组件注册表 zip（免子模块） |
| `tinyusb` | 0.21.0~2 | `https://components.espressif.com/api/components/espressif/tinyusb/versions/0.21.0~2` → `components-file.espressif.com/.../espressif__tinyusb-v0.21.0_2.zip` | 同上，与 esp_tinyusb 2.4.0 同代 |

下载后的 zip 缓存在 `D:\Espressif\dist\`（`espressif__esp_tinyusb-v2.4.0.zip`、
`espressif__tinyusb-v0.21.0_2.zip`），SHA-256 已校验。

组件通过 `firmware/components/`（IDF 默认组件目录）参与构建，**不走组件管理器**：
`firmware/CMakeLists.txt` 里 `set(COMPONENT_MANAGER 0)`。原因见下节 esp_tinyusb 的
CMakeLists 逻辑——它已经支持"本地 tinyusb 组件"模式，禁用组件管理器后
`idf_component.yml` 依赖被忽略，esp_tinyusb 的 CMakeLists 会自动 `REQUIRES tinyusb`
指向本地组件。

## 已打补丁：esp_tinyusb 2.4.0 / tinyusb 0.21.0~2

一共 3 处。补丁 1 是功能性改动（MSC 解耦），补丁 2、3 是**离线构建兼容性**改动
（组件管理器关闭 + vendored 布局带来的必然副作用），不改变运行时行为。

### 补丁 1：解耦 MSC 类驱动与内置存储层

文件：`esp_tinyusb/include/tusb_config.h`

```diff
-#define CFG_TUD_MSC                 CONFIG_TINYUSB_MSC_ENABLED
+#define CFG_TUD_MSC                 1

-#define CFG_TUD_MSC_BUFSIZE         CONFIG_TINYUSB_MSC_BUFSIZE
+#ifndef CONFIG_TINYUSB_MSC_BUFSIZE
+#define CONFIG_TINYUSB_MSC_BUFSIZE 512
+#endif
+#define CFG_TUD_MSC_BUFSIZE         CONFIG_TINYUSB_MSC_BUFSIZE
```

### 为什么必须打这个补丁

esp_tinyusb 2.4.0 的内置 MSC 层（`tinyusb_msc.c`）把 5 个 MSC 回调实现成 **强符号**：

- `tud_msc_read10_cb`
- `tud_msc_write10_cb`
- `tud_msc_capacity_cb`
- `tud_msc_test_unit_ready_cb`
- `tud_msc_scsi_cb`

而 TinyUSB 0.21 的 `src/class/msc/msc_device.c` **只调用、不定义**这 5 个回调
（已逐条 grep 核实：`msc_device.c:548/602/686/732/767/864/924` 只有调用点）。
结论：**媒体后端必须由应用提供**，而 esp_tinyusb 又抢先定义了同名强符号 → 重复符号。

esp_tinyusb 2.4.0 公开的建存储 API 只有三种介质：

| API | 可用性 |
|---|---|
| `tinyusb_msc_new_storage_spiflash()` | 可用，但介质是 Flash 磨损均衡分区，不是 PSRAM RAM disk |
| `tinyusb_msc_new_storage_sdmmc()` | 板子上没有 SD 卡 |
| `tinyusb_msc_new_storage_blockdev()` | 需要 IDF ≥ 6.0.4（本项目 IDF 5.5.6，不可用） |

`storage_medium_t`（`include_private/msc_storage.h`）虽是私有头，但它的 `type` 字段是
`const storage_medium_type_t` 且枚举只有 3 个值，**没有注册自定义介质的公开入口**。

而 Sophon 已冻结的设计是：

- **设备端零 FAT 逻辑**（`docs/design-review.md` 结论 1：MSC 是块设备，Windows 自己
  负责 FAT；设备端只提供格式正确的 FAT16 块设备，FAT 正确性由 `tools/mkfatimg.py`
  保证）。
- 介质是 **PSRAM RAM disk**（`firmware/io_disk/`），上电时从 Flash 里的预置镜像恢复。

⇒ 必须让 esp_tinyusb 的内置 MSC 层 **不参与编译**，同时保留 TinyUSB 的 MSC **类驱动**。

### 补丁如何做到这一点

| 开关 | 效果 |
|---|---|
| `CONFIG_TINYUSB_MSC_ENABLED=n`（sdkconfig 默认，保持 n） | esp_tinyusb 的 `CMakeLists.txt` 不编译 `tinyusb_msc.c` / `storage_spiflash.c` / `storage_sdmmc.c`；`tinyusb.c` 里的 `msc_storage_mount_to_usb/app()` 调用也被 `#if` 掉 → 无未定义符号 |
| `CFG_TUD_MSC = 1`（补丁） | TinyUSB 的 `src/class/msc/msc_device.c` 真正编译，MSC 类驱动可用 |
| `CFG_TUD_MSC_BUFSIZE = 512`（补丁 + fallback） | `CONFIG_TINYUSB_MSC_BUFSIZE` 这个 Kconfig 符号依赖 `TINYUSB_MSC_ENABLED`，因此不存在，需要 fallback（512 是 S3 的 esp_tinyusb 默认值） |

副作用检查（已核实）：

- `esp_tinyusb/usb_descriptors.c` 里的"默认复合描述符"会因 `CFG_TUD_MSC` 为真而包含
  MSC 接口，但 Sophon 自己提供 `full_speed_config` 静态描述符 blob，默认描述符
  **不会被使用**（`descriptors_control.c:205-216` 优先取调用方传入的 blob）。
- `esp_tinyusb/tinyusb_msc.h`、`msc_storage.h` 在 `CONFIG_TINYUSB_MSC_ENABLED=n` 时
  不会被包含。

### 补丁 2：本地 tinyusb 组件名解析 + script-mode 保护

文件：`esp_tinyusb/CMakeLists.txt`

**2a —— 组件名解析。** 组件管理器关闭时，esp_tinyusb 需要知道 tinyusb 组件叫什么名字。
上游逻辑是「`tinyusb` 在 `BUILD_COMPONENTS` 里 → 本地组件，否则 `espressif__tinyusb`
→ 注册表组件」。但 IDF 的**早展开阶段**（CMake script mode，见下文）里
`BUILD_COMPONENTS` 还是空的，上游会去找一个根本不存在的 `espressif__tinyusb`。

```diff
 if(tinyusb IN_LIST build_components)
     set(tinyusb_name tinyusb) # Local component
+elseif(NOT idf_component_manager)
+    # VENDOR PATCH (Sophon): the component manager is disabled and TinyUSB is
+    # vendored as a local component. BUILD_COMPONENTS is still empty during the
+    # early-expansion pass (it is filled only while components are processed),
+    # so the check above cannot see the local component yet.
+    set(tinyusb_name tinyusb) # Local component
 else()
     set(tinyusb_name espressif__tinyusb) # Managed component
 endif()
```

**2b —— script-mode 保护。** esp_tinyusb 的 CMakeLists 末尾要把自己的 `include/`
目录加到 tinyusb 库上：

```cmake
target_include_directories(${tusb_lib} PRIVATE "include")
```

`target_include_directories` **不是可脚本化命令**，在 `cmake -P` 下调用会直接
`FATAL_ERROR` 并中止整个配置阶段。IDF 的早展开正是在 script mode 里跑的
（`tools/cmake/build.cmake:683` → `component_get_requirements.cmake:107` 逐个
`include()` 组件的 CMakeLists.txt），所以必须加守卫：

```diff
+if(NOT CMAKE_SCRIPT_MODE_FILE)
     idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
     target_include_directories(${tusb_lib} PRIVATE "include")
+endif()
```

`CMAKE_SCRIPT_MODE_FILE` 在 script mode 下有值（实测为正在执行的脚本路径），
是 CMake 官方提供的判定方式。

### 补丁 3：`tusb_config.h` 归位到 tinyusb 组件

**这是一次文件移动，不改内容：**
`esp_tinyusb/include/tusb_config.h` → `tinyusb/src/tusb_config.h`。

原因：TinyUSB 的 `src/tusb_option.h` 会 `#include "tusb_config.h"`，而
`tusb_config.h` 原本只存在于 `esp_tinyusb/include/`。IDF 的组件依赖检查器
（`check_component_dependencies`）在编译前扫描每个组件的源文件 include，
发现 tinyusb 用了 esp_tinyusb 提供的头却没在 REQUIRES 里声明，直接报：

```
Compilation failed because tusb_option.h (in "tinyusb" component) includes
tusb_config.h, provided by esp_tinyusb component(s).
However, esp_tinyusb component(s) is not in the requirements list of "tinyusb".
```

直接给 tinyusb 加 `REQUIRES esp_tinyusb` 会和 esp_tinyusb 现有的
`REQUIRES tinyusb` 形成循环依赖。移动文件是更干净的解法：

| | 提供方 | 依赖声明 |
|---|---|---|
| 移动前 | esp_tinyusb | tinyusb → esp_tinyusb（缺失，检查器报错） |
| 移动后 | **tinyusb 自己**（`src/` 是它的 public include 目录） | esp_tinyusb → tinyusb（已有，`REQUIRES ${req}` 里的 `tinyusb`） |

移动后 `esp_tinyusb/include/tusb_config.h` 不再存在，esp_tinyusb 通过
`REQUIRES tinyusb` 拿到同一份文件——**唯一真源仍在 tinyusb 组件内**，不存在两份配置
漂移的风险。

### 升级组件时怎么办

升级 esp_tinyusb 版本后，**必须重新核对**：

1. `tinyusb_msc.c` 里 5 个 `tud_msc_*_cb` 是否仍是强符号（`grep -n 'tud_msc_\w+_cb' tinyusb_msc.c`）；
2. `include/tusb_config.h` 里 `CFG_TUD_MSC` / `CFG_TUD_MSC_BUFSIZE` 的写法是否变化；
3. TinyUSB 侧 `src/class/msc/msc_device.c` 是否开始自带这 5 个回调的 weak 默认实现
   （若自带，App 侧强定义会覆盖 weak，仍然可用，但需要重新确认语义）；
4. `esp_tinyusb` 是否新增了 `storage_medium_t` 的公开注册入口（有的话可以去掉本补丁）；
5. 上游 `CMakeLists.txt` 里 `tinyusb_name` 的解析逻辑与 `target_include_directories`
   是否变化（补丁 2 是否仍然需要）；
6. `tusb_config.h` 的位置约定是否变化（补丁 3 是否仍然需要）——升级后要先跑一次
   `idf.py build`，若检查器又报同样的 include 依赖错误，说明上游又调整了归属。

核对通过后，把本文件里的"版本"表格更新，并重新 grep 一次 `tud_msc_*_cb` 的归属。
