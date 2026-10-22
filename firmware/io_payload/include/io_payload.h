/*
 * io_payload — T2 运行时 payload 仓
 *
 * 为什么需要它(见 docs/agent-interface-mcp.md §二之二):
 *   RAM disk 是**易失**的,每次上电从出厂镜像重建。要让"引导文件"跨断电存活,
 *   内容必须落在 flash 里;但内容本身是"AI 现场就能生成"的东西,不该烤进固件。
 *   所以:固件只占**位置**(RAM disk 里的预留槽位),内容落在 flash payload 仓。
 *
 * flash 布局(两个 512KB 分区,script0 / script1,A/B):
 *   [0..63]   头:magic / version / seq / count / dir_crc
 *   [64..]    记录目录:每条 32 字节(slot_index / len / crc32 / data_off)
 *   [...]     payload 数据块(512 字节对齐)
 *
 * A/B 语义:**seq 大者胜**,但只有 magic + 目录 crc + 每条 payload crc 全过
 * 才算有效。写入永远写"非当前生效"的那个分区 -> 写到一半掉电,那个分区的
 * crc 必然不过,当前生效的分区纹丝不动。切换不需要额外的提交标记。
 *
 * 设备端依然零 FAT 逻辑:槽位位置全部来自构建期生成的槽位表。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 开机:扫两个分区,选出 seq 最大且校验通过的一份,应用到 RAM disk 槽位。 */
esp_err_t io_payload_init(void);

/**
 * 落盘一个 payload 到 flash(**只写 flash,不碰 RAM disk 槽位**)。
 *
 * 槽位是"每次开机从 flash 铺开的副本"。运行期改它等于和宿主抢同一块存储:
 * 宿主可能已经删了槽文件(簇被释放)或新建了占用同簇的文件,设备再写就会
 * 写进孤儿簇、或覆盖宿主的新文件。所以运行期下发一律"落 flash -> 下次开机生效",
 * 运行期竞争窗口为零。
 *
 * 要"部署完立刻生效"时:store 成功后再显式调 io_payload_apply()。
 * **运行期调 apply 是后期优化项**,届时必须先实现"写槽前校验 FAT1 与出厂镜像
 * 一致",否则就是上面说的竞争窗口。开机时的 apply 是安全的(宿主刚枚举)。
 *
 * @param slot_index 槽位表下标(见 io_disk_slot_at)
 * @param data       payload 字节
 * @param len        字节数,不能超过槽位容量
 */
esp_err_t io_payload_store(uint32_t slot_index, const void *data, uint32_t len);

/**
 * 把当前生效的快照覆盖到 RAM disk 槽位。
 *
 * 正常只在**开机时**调用(io_payload_init)—— 那时宿主刚枚举、还没动过盘。
 * 运行期调用需要自己保证宿主没动过目标簇链。
 */
esp_err_t io_payload_apply(void);

/** 当前生效快照的 seq(0 = 还没有任何 payload)。 */
uint32_t io_payload_active_seq(void);

/** 当前生效快照里指定槽位的 payload 长度(没有则 0)。 */
uint32_t io_payload_active_len(uint32_t slot_index);

/** 当前生效快照里目标槽位是否与 (len, crc32) 一致。 */
bool io_payload_active_matches(uint32_t slot_index, uint32_t len, uint32_t crc32);

/** 启动"信箱轮询"任务:宿主往 RAM disk 的信箱文件里写 payload,设备自动落盘。 */
esp_err_t io_payload_start_ingest(void);

#ifdef __cplusplus
}
#endif
