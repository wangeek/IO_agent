/*
 * io_disk — 虚拟 U 盘(RAM disk)后端
 *
 * 关键设计(见 docs/design-review.md 结论 1 与 implementation-plan.md M1):
 *   USB MSC 是**块设备**。Windows 自己负责 FAT 文件系统的一切:
 *   分配簇、写目录项、更新 FAT。设备端只需要提供一块**格式正确的 FAT16
 *   块设备**——所以 FAT 正确性全部由 tools/mkfatimg.py 保证,固件零 FAT 逻辑。
 *
 * 数据方向只有一条合法路径(USB 主从架构决定的):
 *   主机写 U 盘(bulk OUT) → 写入本模块的 PSRAM 缓冲 → 固件本地读 → WiFi 上传。
 *   ESP32 不能向主机发起任何数据。
 *
 * 掉电语义:RAM disk 是易失的,每次上电从 Flash 里的镜像重建。
 *   → 主机写入的内容(命令/响应)不跨重启存活;需留存的只有 NVS 里的配置。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "ramdisk_slots.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IO_DISK_SECTOR_SIZE 512

/** 上电初始化:在 PSRAM 分配卷并载入预置镜像(工具脚本等)。 */
esp_err_t io_disk_init(void);

/** 卷容量(扇区数)。 */
uint32_t io_disk_sector_count(void);

/** 读块。out 至少 count * 512 字节。 */
esp_err_t io_disk_read(uint32_t lba, void *out, size_t count);

/** 写块(主机 → 设备,唯一合法的数据入口)。 */
esp_err_t io_disk_write(uint32_t lba, const void *in, size_t count);

/**
 * 字节级读写(MSC 回调用)。
 *
 * MSC 的 READ(10)/WRITE(10) 回调给出的是 (lba, offset, len),其中 offset 是
 * 扇区内字节偏移,一次传输可能跨扇区边界。io_disk 的块接口是扇区粒度的,
 * 所以这里补一层字节粒度的入口。
 *
 * @param lba    起始逻辑块号
 * @param offset 扇区内字节偏移(0 .. 511)
 * @param buf    数据缓冲
 * @param len    字节数
 */
esp_err_t io_disk_read_at(uint32_t lba, uint32_t offset, void *buf, size_t len);
esp_err_t io_disk_write_at(uint32_t lba, uint32_t offset, const void *buf, size_t len);

/** 按卷内字节偏移读一段(固件内部用,不经过 MSC)。 */
esp_err_t io_disk_read_range(uint32_t offset, void *out, size_t len);

/**
 * 把 payload 写进一个预留槽位,并把目录项的 size 字段改成实际长度。
 *
 * 这是 T2 的核心写入口:槽位的簇链在出厂镜像里**已经分配好**(所以 FAT 表
 * 一个字节都不用动,也不会和宿主的 FAT 驱动抢簇),这里只做两件事:
 *   1. 把 payload memcpy 进槽位的数据簇;
 *   2. 定点写目录项里的 4 字节 FileSize(size_field_offset 是构建期已知偏移)。
 *
 * 尾部(len .. reserved)清零,避免上一版更长的 payload 残留。
 */
esp_err_t io_disk_apply_slot(const io_slot_desc_t *slot, const void *data, uint32_t len);

/** 槽位表(构建期由 tools/mkfatimg.py --emit-slot-table 生成)。 */
const io_slot_desc_t *io_disk_slot_table(uint32_t *count);

/** 按 8.3 名字找槽位(大小写不敏感)。找不到返回 NULL。 */
const io_slot_desc_t *io_disk_slot_find(const char *name);

/** 按下标取槽位。越界返回 NULL。 */
const io_slot_desc_t *io_disk_slot_at(uint32_t index);

/** 根目录里一个文件项能拿到的字段。 */
typedef struct {
    uint32_t size;           /**< 文件字节数 */
    uint16_t first_cluster;  /**< 起始簇号 */
} io_file_info_t;

/** FAT16 几何信息(设备端只读解析用)。 */
typedef struct {
    uint32_t data_lba;        /**< 数据区起始 LBA(簇 2 在这里) */
    uint32_t cluster_sectors; /**< 每簇扇区数 */
    uint32_t fat_lba;         /**< FAT1 起始 LBA */
    uint32_t fat_sectors;     /**< 每个 FAT 的扇区数 */
    uint32_t root_entries;    /**< 根目录项数 */
} io_fat_geom_t;

/**
 * 在根目录里按 8.3 名字找文件(只读扫描)。
 *
 * 用途:设备要判断"目标机上的 watcher 起没起"(看 CAP.TXT 在不在)、
 * 要读 CMD\*.cmd 的回执 —— 这些文件是**宿主写的**,不在预留槽位表里。
 *
 * 这只是**读**根目录项(名字/属性/size/起始簇),**不解析 FAT 簇链**,
 * FAT 表仍然一个字节不动 —— 与"设备端零 FAT 写入器"不冲突。
 *
 * @param name 8.3 名字,如 "CAP.TXT";大小写不敏感
 * @param out  可为 NULL(只问存在性)
 * @return 找到返回 true
 */
bool io_disk_find_file(const char *name, io_file_info_t *out);

/** 取 FAT16 几何信息(只读)。 */
esp_err_t io_disk_fat_geometry(io_fat_geom_t *g);

/**
 * 读一个文件的**内容**(只读:沿簇链走 FAT,不改任何字节)。
 *
 * 用途:observe 工具把目标机上的快照文件原样转发给 agent ——
 * 设备不解码、不比对,只搬运(哑设备原则)。
 *
 * @param buf     接收缓冲
 * @param max     缓冲大小(超出则截断)
 * @param out_len 实际读到的字节数
 */
esp_err_t io_disk_read_file(const char *name, void *buf, size_t max, size_t *out_len);

/** 统计:主机写块次数与字节数(M1 验证"主机写 → 设备收到"用)。 */
uint32_t io_disk_write_stats_blocks(void);
uint64_t io_disk_write_stats_bytes(void);

#ifdef __cplusplus
}
#endif
