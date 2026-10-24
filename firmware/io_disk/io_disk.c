#include "io_disk.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

/* 构建期内嵌的 FAT16 镜像(EMBED_FILES 生成,见 io_disk/CMakeLists.txt) */
extern const uint8_t _binary_ramdisk_img_start[] asm("_binary_ramdisk_img_start");
extern const uint8_t _binary_ramdisk_img_end[] asm("_binary_ramdisk_img_end");

/* BPB 里的卷容量字段(引导扇区偏移)。
   total16 = 0 时以 total32 为准(Windows 对 >65535 扇区的卷这么写)。 */
#define FAT_BPB_TOTAL16_OFF 19
#define FAT_BPB_TOTAL32_OFF 32

/* PSRAM 8MB → 卷容量上限 16384 扇区。BPB 损坏时用来挡住离谱的 malloc。 */
#define IO_DISK_MAX_SECTORS (8u * 1024u * 1024u / IO_DISK_SECTOR_SIZE)

static const char *TAG = "io_disk";

static uint8_t *s_vol;              /* PSRAM 中的卷 */
static uint32_t s_sectors;
static uint32_t s_write_blocks;
static uint64_t s_write_bytes;

/* 从引导扇区读卷容量。镜像可以是稀疏的:文件长度只到最后一个已用扇区,
   但 BPB 里的 total32 仍然是完整卷容量。 */
static uint32_t fat_total_sectors(const uint8_t *img, size_t img_size)
{
    uint16_t t16;
    uint32_t t32;

    if (img_size < 64) {
        return 0;
    }
    t16 = (uint16_t)img[FAT_BPB_TOTAL16_OFF] |
          ((uint16_t)img[FAT_BPB_TOTAL16_OFF + 1] << 8);
    t32 = (uint32_t)img[FAT_BPB_TOTAL32_OFF] |
          ((uint32_t)img[FAT_BPB_TOTAL32_OFF + 1] << 8) |
          ((uint32_t)img[FAT_BPB_TOTAL32_OFF + 2] << 16) |
          ((uint32_t)img[FAT_BPB_TOTAL32_OFF + 3] << 24);
    return (t16 != 0) ? (uint32_t)t16 : t32;
}

esp_err_t io_disk_init(void)
{
    const size_t img_size = (size_t)(_binary_ramdisk_img_end - _binary_ramdisk_img_start);
    uint32_t total_sectors;
    size_t vol_size;

    if (s_vol) {
        return ESP_OK;
    }
    if (img_size == 0 || img_size % IO_DISK_SECTOR_SIZE) {
        ESP_LOGE(TAG, "embedded image invalid: %u bytes", (unsigned)img_size);
        return ESP_ERR_INVALID_SIZE;
    }

    total_sectors = fat_total_sectors(_binary_ramdisk_img_start, img_size);
    if (total_sectors == 0 || total_sectors > IO_DISK_MAX_SECTORS) {
        ESP_LOGE(TAG, "BPB volume size invalid: %u sectors", (unsigned)total_sectors);
        return ESP_ERR_INVALID_SIZE;
    }
    vol_size = (size_t)total_sectors * IO_DISK_SECTOR_SIZE;
    if (img_size > vol_size) {
        ESP_LOGE(TAG, "image larger than declared volume");
        return ESP_ERR_INVALID_SIZE;
    }

    s_vol = heap_caps_malloc(vol_size, MALLOC_CAP_SPIRAM);
    if (!s_vol) {
        ESP_LOGE(TAG, "PSRAM alloc of %u bytes failed", (unsigned)vol_size);
        return ESP_ERR_NO_MEM;
    }
    /* 镜像是稀疏的:只覆盖到最后一个已用扇区,其余(未用的数据区)补零。
       宿主看到的是一个完整的 4MB 可移动卷,设备端依然零 FAT 逻辑。 */
    memcpy(s_vol, _binary_ramdisk_img_start, img_size);
    if (vol_size > img_size) {
        memset(s_vol + img_size, 0, vol_size - img_size);
    }
    s_sectors = total_sectors;

    ESP_LOGI(TAG, "RAM disk ready: %u sectors (%u KB, PSRAM), image %u sectors (%u KB)",
             (unsigned)s_sectors, (unsigned)(vol_size / 1024),
             (unsigned)(img_size / IO_DISK_SECTOR_SIZE), (unsigned)(img_size / 1024));
    return ESP_OK;
}

uint32_t io_disk_sector_count(void)
{
    return s_sectors;
}

esp_err_t io_disk_read(uint32_t lba, void *out, size_t count)
{
    if (!s_vol) {
        return ESP_ERR_INVALID_STATE;
    }
    if (lba + count > s_sectors) {
        return ESP_ERR_INVALID_ARG;   /* 越界:SCSI 层会转成 sense code */
    }
    memcpy(out, s_vol + (size_t)lba * IO_DISK_SECTOR_SIZE, count * IO_DISK_SECTOR_SIZE);
    return ESP_OK;
}

esp_err_t io_disk_write(uint32_t lba, const void *in, size_t count)
{
    if (!s_vol) {
        return ESP_ERR_INVALID_STATE;
    }
    if (lba + count > s_sectors) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_vol + (size_t)lba * IO_DISK_SECTOR_SIZE, in, count * IO_DISK_SECTOR_SIZE);
    s_write_blocks += (uint32_t)count;
    s_write_bytes += (uint64_t)count * IO_DISK_SECTOR_SIZE;
    return ESP_OK;
}

/* 字节粒度入口:MSC 回调给的是 (lba, offset, len),可能跨扇区边界。
   offset 恒 < 512,所以第一段一定在 lba 扇区内,后续段整扇对齐。 */
static esp_err_t io_disk_access_at(uint32_t lba, uint32_t offset,
                                    void *buf, size_t len, bool write)
{
    if (!s_vol) {
        return ESP_ERR_INVALID_STATE;
    }
    if (offset >= IO_DISK_SECTOR_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lba >= s_sectors) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *p = (uint8_t *)buf;
    size_t done = 0;

    while (done < len) {
        const uint32_t cur_sector = lba + (uint32_t)((offset + done) / IO_DISK_SECTOR_SIZE);
        const uint32_t cur_off = (uint32_t)((offset + done) % IO_DISK_SECTOR_SIZE);
        size_t chunk = IO_DISK_SECTOR_SIZE - cur_off;
        if (chunk > len - done) {
            chunk = len - done;
        }
        if (cur_sector >= s_sectors) {
            return ESP_ERR_INVALID_ARG;   /* 越界:SCSI 层会转成 sense code */
        }

        uint8_t *dst = s_vol + (size_t)cur_sector * IO_DISK_SECTOR_SIZE + cur_off;
        if (write) {
            memcpy(dst, p + done, chunk);
            s_write_bytes += chunk;
        } else {
            memcpy(p + done, dst, chunk);
        }
        done += chunk;
    }

    if (write) {
        s_write_blocks += (uint32_t)((offset + len + IO_DISK_SECTOR_SIZE - 1) / IO_DISK_SECTOR_SIZE);
    }
    return ESP_OK;
}

esp_err_t io_disk_read_at(uint32_t lba, uint32_t offset, void *buf, size_t len)
{
    return io_disk_access_at(lba, offset, buf, len, false);
}

esp_err_t io_disk_write_at(uint32_t lba, uint32_t offset, const void *buf, size_t len)
{
    /* io_disk_access_at 内部只写不读 const 数据,这里做一次显式转型 */
    return io_disk_access_at(lba, offset, (void *)(uintptr_t)buf, len, true);
}

esp_err_t io_disk_read_range(uint32_t offset, void *out, size_t len)
{
    if (!s_vol) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((uint64_t)offset + len > (uint64_t)s_sectors * IO_DISK_SECTOR_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(out, s_vol + offset, len);
    return ESP_OK;
}

const io_slot_desc_t *io_disk_slot_table(uint32_t *count)
{
    if (count) {
        *count = IO_SLOT_COUNT;
    }
    return io_slot_table;
}

const io_slot_desc_t *io_disk_slot_find(const char *name)
{
    if (!name || !name[0]) {
        return NULL;
    }
    for (uint32_t i = 0; i < IO_SLOT_COUNT; i++) {
        const char *n = io_slot_table[i].name;
        uint32_t k = 0;
        while (n[k] && name[k] &&
               (n[k] == name[k] ||
                (n[k] >= 'A' && n[k] <= 'Z' && n[k] + 32 == name[k]) ||
                (n[k] >= 'a' && n[k] <= 'z' && n[k] - 32 == name[k]))) {
            k++;
        }
        if (n[k] == '\0' && name[k] == '\0') {
            return &io_slot_table[i];
        }
    }
    return NULL;
}

const io_slot_desc_t *io_disk_slot_at(uint32_t index)
{
    return (index < IO_SLOT_COUNT) ? &io_slot_table[index] : NULL;
}

/* ------------------------------------------------------------------ */
/* 根目录只读扫描                                                       */
/* ------------------------------------------------------------------ */

/*
 * FAT16 BPB 字段偏移(引导扇区偏移)。
 *
 * ⚠⚠ 这些常量必须精确到字节 —— 错一位就会扫到完全错误的区域,表现为
 * "文件永远找不到"(这个坑踩了两次:曾按"OEM Name 是 3 字节"的记忆写偏移,
 *  实际规范里 OEM Name 是 **8 字节**(0x03..0x0A),后面所有字段右移 5;
 *  第二次照着同一套错记忆"修正",依然错)。
 *
 * 正确布局(用 tools/dump_rootdir.py 对着真实镜像字节核对过):
 *   0x03 OEM Name (8 字节)  |  0x0B bytes/sector |  0x0D sectors/cluster
 *   0x0E reserved sectors   |  0x10 num FATs      |  0x11 root entries
 *   0x13 total sectors 16   |  0x16 FAT size 16   |  0x20 total sectors 32
 */
#define FAT_BPB_ROOT_ENTRIES_OFF  17     /* 根目录项数(2 字节) */
#define FAT_BPB_NUM_FATS_OFF      16     /* FAT 个数(1 字节) */
#define FAT_BPB_FAT_SIZE_OFF      22     /* 每个 FAT 的扇区数(2 字节) */
#define FAT_BPB_RESERVED_OFF      14     /* 保留扇区数(2 字节) */
#define FAT_BPB_SPC_OFF           13     /* 每簇扇区数(1 字节) */

#define FAT_DIRENT_SIZE           32
#define FAT_DIRENT_FREE           0x00    /* 该项起后面全空 */
#define FAT_DIRENT_DELETED        0xE5    /* 已删除 */
#define FAT_DIRENT_ATTR_VOLUME    0x08    /* 卷标,不是文件 */

static uint16_t fat_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 根目录区在卷内的字节偏移,以及项数。BPB 坏了就返回 0。 */
static uint32_t fat_root_dir_offset(uint32_t *entries)
{
    if (!s_vol || s_sectors < 2) {
        return 0;
    }
    const uint8_t *bpb = s_vol;
    const uint16_t reserved = fat_rd16(&bpb[FAT_BPB_RESERVED_OFF]);
    const uint8_t num_fats = bpb[FAT_BPB_NUM_FATS_OFF];
    const uint16_t fat_size = fat_rd16(&bpb[FAT_BPB_FAT_SIZE_OFF]);
    const uint16_t root_entries = fat_rd16(&bpb[FAT_BPB_ROOT_ENTRIES_OFF]);

    if (num_fats == 0 || fat_size == 0 || root_entries == 0) {
        return 0;
    }
    const uint32_t root_lba = (uint32_t)reserved + (uint32_t)num_fats * fat_size;
    const uint32_t root_bytes = (uint32_t)root_entries * FAT_DIRENT_SIZE;
    if ((uint64_t)(root_lba + root_bytes / IO_DISK_SECTOR_SIZE) > s_sectors) {
        return 0;
    }
    if (entries) {
        *entries = root_entries;
    }
    return root_lba * IO_DISK_SECTOR_SIZE;
}

esp_err_t io_disk_fat_geometry(io_fat_geom_t *g)
{
    if (!s_vol || !g) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *bpb = s_vol;
    const uint16_t reserved = fat_rd16(&bpb[FAT_BPB_RESERVED_OFF]);
    const uint8_t num_fats = bpb[FAT_BPB_NUM_FATS_OFF];
    const uint16_t fat_size = fat_rd16(&bpb[FAT_BPB_FAT_SIZE_OFF]);
    const uint16_t root_entries = fat_rd16(&bpb[FAT_BPB_ROOT_ENTRIES_OFF]);
    const uint8_t spc = bpb[FAT_BPB_SPC_OFF];

    if (num_fats == 0 || fat_size == 0 || root_entries == 0 || spc == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t root_lba = (uint32_t)reserved + (uint32_t)num_fats * fat_size;
    const uint32_t root_sectors = ((uint32_t)root_entries * FAT_DIRENT_SIZE +
                                   IO_DISK_SECTOR_SIZE - 1) / IO_DISK_SECTOR_SIZE;
    const uint32_t data_lba = root_lba + root_sectors;
    if (data_lba >= s_sectors) {
        return ESP_ERR_INVALID_SIZE;
    }
    g->data_lba = data_lba;
    g->cluster_sectors = spc;
    g->fat_lba = reserved;
    g->fat_sectors = fat_size;
    g->root_entries = root_entries;
    return ESP_OK;
}

/* 读 FAT16 的下一个簇号。cluster 从 2 开始。 */
static uint16_t fat_next_cluster(const io_fat_geom_t *g, uint16_t cluster)
{
    if (cluster < 2) {
        return 0xFFFF;
    }
    const uint32_t byte_off = (uint32_t)cluster * 2u;   /* FAT16 每项 2 字节 */
    const uint32_t sector = g->fat_lba + byte_off / IO_DISK_SECTOR_SIZE;
    const uint32_t off = byte_off % IO_DISK_SECTOR_SIZE;
    if (sector >= g->fat_lba + g->fat_sectors) {
        return 0xFFFF;
    }
    return fat_rd16(s_vol + (size_t)sector * IO_DISK_SECTOR_SIZE + off);
}

esp_err_t io_disk_read_file(const char *name, void *buf, size_t max, size_t *out_len)
{
    io_fat_geom_t g;
    io_file_info_t info;

    if (!name || !buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if (io_disk_fat_geometry(&g) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!io_disk_find_file(name, &info) || info.first_cluster < 2) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t copied = 0;
    uint16_t cluster = info.first_cluster;
    uint32_t guard = 0;
    const uint32_t max_clusters = g.fat_sectors * (IO_DISK_SECTOR_SIZE / 2);

    while (cluster >= 2 && cluster < 0xFFF8) {
        if (++guard > max_clusters) {
            return ESP_ERR_INVALID_SIZE;   /* 簇链成环 */
        }
        const uint32_t lba = g.data_lba +
                             ((uint32_t)cluster - 2u) * g.cluster_sectors;
        if ((uint64_t)lba + g.cluster_sectors > s_sectors) {
            break;
        }
        const size_t chunk = (size_t)g.cluster_sectors * IO_DISK_SECTOR_SIZE;
        const size_t take = (info.size - copied < chunk) ? (info.size - copied) : chunk;
        if (copied + take > max) {
            break;                          /* 缓冲区不够,截断 */
        }
        memcpy(dst + copied, s_vol + (size_t)lba * IO_DISK_SECTOR_SIZE, take);
        copied += take;
        if (copied >= info.size) {
            break;
        }
        cluster = fat_next_cluster(&g, cluster);
    }

    if (out_len) {
        *out_len = copied;
    }
    return (copied > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* "CAP.TXT" -> "CAP     TXT"(8.3 的 11 字节目录项名)。 */
static bool fat_make_83(const char *in, uint8_t out[11])
{
    memset(out, ' ', 11);
    int i = 0;
    while (*in && *in != '.' && i < 8) {
        uint8_t c = (uint8_t)*in++;
        if (c >= 'a' && c <= 'z') {
            c = (uint8_t)(c - 32);
        }
        out[i++] = c;
    }
    if (*in == '.') {
        in++;
        int j = 0;
        while (*in && j < 3) {
            uint8_t c = (uint8_t)*in++;
            if (c >= 'a' && c <= 'z') {
                c = (uint8_t)(c - 32);
            }
            out[8 + j++] = c;
        }
    }
    return (*in == '\0');
}

/* FAT16 目录项里另外两个我们要的字段:最后写入时间/日期。 */
#define FAT_DIRENT_MTIME_OFF   24     /* 最后写入时间(2 字节) */
#define FAT_DIRENT_MDATE_OFF   26     /* 最后写入日期(2 字节) */
#define FAT_DIRENT_ATTR_DIR    0x10   /* 子目录 */

/*
 * 8.3 目录项名 -> "NAME.EXT"。
 * 目录项里是 8 字节名 + 3 字节扩展,空格补齐;扩展为空时不带点。
 */
static void fat_dirent_name(const uint8_t *e, char *out)
{
    char base[9];
    char ext[4];
    memcpy(base, e, 8);
    base[8] = '\0';
    for (int i = 7; i >= 0 && base[i] == ' '; i--) {
        base[i] = '\0';
    }
    memcpy(ext, e + 8, 3);
    ext[3] = '\0';
    for (int i = 2; i >= 0 && ext[i] == ' '; i--) {
        ext[i] = '\0';
    }
    if (ext[0]) {
        snprintf(out, 13, "%s.%s", base, ext);
    } else {
        snprintf(out, 13, "%s", base);
    }
}

/*
 * FAT16 的最后写入时间/日期 -> Unix 秒。
 *
 * 时间位(2 字节): bit15-11=时, bit10-5=分, bit4-0=秒/2
 * 日期位(2 字节): bit15-9=年-1980, bit8-5=月, bit4-0=日
 *
 * 用的是标准的"从 1970-01-01 算天数"算法(Howard Hinnant 的 civil 算法),
 * 闰年规则才不会错。
 */
static int64_t fat_mtime_to_epoch(uint16_t time, uint16_t date)
{
    if (date == 0 || time == 0) {
        return 0;
    }
    const int year = 1980 + (int)((date >> 9) & 0x7F);
    const int month = (int)((date >> 5) & 0x0F);
    const int day = (int)(date & 0x1F);
    const int hour = (int)((time >> 11) & 0x1F);
    const int minute = (int)((time >> 5) & 0x3F);
    const int second = (int)(time & 0x1F) * 2;
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    int64_t y = year;
    y -= (month <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;   /* 1970-01-01 是第 0 天 */
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

/* 把一个目录项解析成 io_file_info_t。 */
static void fat_dirent_to_info(const uint8_t *e, io_file_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->size = (uint32_t)e[0x1C] | ((uint32_t)e[0x1D] << 8) |
                ((uint32_t)e[0x1E] << 16) | ((uint32_t)e[0x1F] << 24);
    out->first_cluster = fat_rd16(&e[0x1A]);
    fat_dirent_name(e, out->name);
    out->mtime = fat_mtime_to_epoch(fat_rd16(&e[FAT_DIRENT_MTIME_OFF]),
                                    fat_rd16(&e[FAT_DIRENT_MDATE_OFF]));
}

bool io_disk_find_file(const char *name, io_file_info_t *out)
{
    uint8_t want[11];
    uint32_t entries = 0;

    if (!name || !name[0] || !fat_make_83(name, want)) {
        return false;
    }
    const uint32_t root_off = fat_root_dir_offset(&entries);
    if (root_off == 0) {
        return false;
    }

    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *e = s_vol + root_off + (size_t)i * FAT_DIRENT_SIZE;
        if (e[0] == FAT_DIRENT_FREE) {
            break;                      /* 后面的项都是空的 */
        }
        if (e[0] == FAT_DIRENT_DELETED) {
            continue;                   /* 已删除 */
        }
        if (e[11] & FAT_DIRENT_ATTR_VOLUME) {
            continue;                   /* 卷标 */
        }
        if (memcmp(e, want, 11) != 0) {
            continue;
        }
        if (out) {
            fat_dirent_to_info(e, out);
        }
        return true;
    }
    return false;
}

size_t io_disk_list_files(io_file_info_t *out, size_t max)
{
    uint32_t entries = 0;

    if (!s_vol) {
        return 0;
    }
    const uint32_t root_off = fat_root_dir_offset(&entries);
    if (root_off == 0) {
        return 0;
    }

    size_t n = 0;
    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *e = s_vol + root_off + (size_t)i * FAT_DIRENT_SIZE;
        if (e[0] == FAT_DIRENT_FREE) {
            break;                      /* 后面的项都是空的 */
        }
        if (e[0] == FAT_DIRENT_DELETED) {
            continue;                   /* 已删除 */
        }
        if (e[11] & FAT_DIRENT_ATTR_VOLUME) {
            continue;                   /* 卷标 */
        }
        if (e[11] & FAT_DIRENT_ATTR_DIR) {
            continue;                   /* 子目录:设备这边不处理 */
        }
        if (out && n < max) {
            fat_dirent_to_info(e, &out[n]);
        }
        n++;
    }
    return n;
}

esp_err_t io_disk_apply_slot(const io_slot_desc_t *slot, const void *data, uint32_t len)
{
    if (!s_vol || !slot) {
        return ESP_ERR_INVALID_STATE;
    }
    if (slot->kind != IO_SLOT_KIND_SLOT) {
        return ESP_ERR_INVALID_ARG;      /* 信箱由宿主写,设备不许碰 */
    }
    if (len > slot->capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (len && !data) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint64_t)slot->data_offset + slot->reserved >
        (uint64_t)s_sectors * IO_DISK_SECTOR_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1) 数据簇:纯 memcpy。簇链在出厂镜像里已分配,FAT 表不动。 */
    memcpy(s_vol + slot->data_offset, data, len);
    /* 2) 尾部清零:避免上一版更长的 payload 残留被宿主读到。 */
    if (slot->reserved > len) {
        memset(s_vol + slot->data_offset + len, 0, slot->reserved - len);
    }
    /* 3) 目录项 size 字段:构建期已知偏移的 4 字节定点写(小端)。 */
    uint8_t *p = s_vol + slot->size_field_offset;
    p[0] = (uint8_t)(len & 0xFFu);
    p[1] = (uint8_t)((len >> 8) & 0xFFu);
    p[2] = (uint8_t)((len >> 16) & 0xFFu);
    p[3] = (uint8_t)((len >> 24) & 0xFFu);

    ESP_LOGI(TAG, "slot %s applied: %u bytes (capacity %u)",
             slot->name, (unsigned)len, (unsigned)slot->capacity);
    return ESP_OK;
}

uint32_t io_disk_write_stats_blocks(void)
{
    return s_write_blocks;
}

uint64_t io_disk_write_stats_bytes(void)
{
    return s_write_bytes;
}
