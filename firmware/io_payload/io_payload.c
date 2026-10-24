#include "io_payload.h"

#include <string.h>
#include <stdbool.h>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "io_disk.h"

static const char *TAG = "io_payload";

/* ------------------------------------------------------------------ 常量 */

#define PW_MAGIC        "IOAGPAY1"
#define PW_MAGIC_LEN    8
#define PW_VERSION      1u
#define PW_HDR_SIZE     64u
#define PW_REC_SIZE     32u
#define PW_MAX_RECORDS  8u
#define PW_ALIGN        512u
#define PW_CHUNK_SIZE   4096u
#define PW_PART_LABELS  { "script0", "script1" }

typedef struct __attribute__((packed)) {
    char     magic[PW_MAGIC_LEN];
    uint32_t version;
    uint32_t seq;
    uint32_t count;
    uint32_t dir_crc;
    uint8_t  reserved[40];
} pw_header_t;

typedef struct __attribute__((packed)) {
    uint32_t slot_index;
    uint32_t len;
    uint32_t crc32;
    uint32_t data_off;
    uint8_t  reserved[16];
} pw_record_t;

typedef struct {
    bool        valid;
    int         part;                  /* 0/1 = script0/script1 */
    uint32_t    seq;
    uint32_t    count;
    pw_record_t rec[PW_MAX_RECORDS];
} pw_snapshot_t;

static pw_snapshot_t s_active;

/* ------------------------------------------------------------------ CRC32 */

/* 自己实现而不是用 IDF 的 esp_crc32_le:要和宿主侧 zlib.crc32 逐字节对上,
   而且要能分块喂数据(校验 payload 时按 4KB 读,不用把整个 payload 读进 RAM)。 */
static uint32_t pw_crc32(uint32_t crc, const void *data, size_t len)
{
    static uint32_t table[256];
    static bool table_ready;

    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_ready = true;
    }

    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    while (len--) {
        c = table[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ 分区 */

static const esp_partition_t *pw_partition(int idx)
{
    static const char *labels[2] = PW_PART_LABELS;
    if (idx < 0 || idx > 1) {
        return NULL;
    }
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     ESP_PARTITION_SUBTYPE_ANY,
                                     labels[idx]);
}

/* ------------------------------------------------------------------ 快照 */

/* 读一个分区,校验通过才算有效。 */
static bool pw_load(int part_idx, pw_snapshot_t *out)
{
    const esp_partition_t *part = pw_partition(part_idx);
    uint8_t dir[PW_MAX_RECORDS * PW_REC_SIZE];
    uint8_t *chunk = NULL;
    bool ok = false;

    if (!part) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* 分块缓冲**必须走堆**:app_main 的主任务栈只有 3584 字节,
       4KB 的栈上缓冲会把主任务栈冲爆(实测:连带 ipc1 任务栈溢出)。 */
    chunk = (uint8_t *)heap_caps_malloc(PW_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) {
        return false;
    }

    pw_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        goto done;
    }
    if (memcmp(hdr.magic, PW_MAGIC, PW_MAGIC_LEN) != 0) {
        goto done;
    }
    if (hdr.version != PW_VERSION || hdr.count > PW_MAX_RECORDS) {
        goto done;
    }
    if (hdr.count) {
        if (esp_partition_read(part, PW_HDR_SIZE, dir, hdr.count * PW_REC_SIZE) != ESP_OK) {
            goto done;
        }
        if (pw_crc32(0, dir, hdr.count * PW_REC_SIZE) != hdr.dir_crc) {
            goto done;
        }
    }

    for (uint32_t i = 0; i < hdr.count; i++) {
        const pw_record_t *r = (const pw_record_t *)&dir[i * PW_REC_SIZE];
        if (r->len == 0 || (uint64_t)r->data_off + r->len > part->size) {
            goto done;
        }
        /* payload crc:分块读,不把整个 payload 读进 RAM */
        uint32_t crc = 0;
        uint32_t left = r->len;
        uint32_t off = r->data_off;
        while (left) {
            uint32_t n = (left > PW_CHUNK_SIZE) ? (uint32_t)PW_CHUNK_SIZE : left;
            if (esp_partition_read(part, off, chunk, n) != ESP_OK) {
                goto done;
            }
            crc = pw_crc32(crc, chunk, n);
            off += n;
            left -= n;
        }
        if (crc != r->crc32) {
            goto done;
        }
        out->rec[i] = *r;
    }

    out->valid = true;
    out->part = part_idx;
    out->seq = hdr.seq;
    out->count = hdr.count;
    ok = true;

done:
    heap_caps_free(chunk);
    return ok;
}

/* 两个分区里选 seq 大的那份。 */
static bool pw_load_active(pw_snapshot_t *out)
{
    pw_snapshot_t a, b;
    const bool oka = pw_load(0, &a);
    const bool okb = pw_load(1, &b);

    if (oka && okb) {
        *out = (b.seq > a.seq) ? b : a;
    } else if (oka) {
        *out = a;
    } else if (okb) {
        *out = b;
    } else {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

/* 把一条记录里的 payload 读出来,应用到 RAM disk 槽位。 */
static esp_err_t pw_apply_record(int part_idx, const pw_record_t *r)
{
    const io_slot_desc_t *slot = io_disk_slot_at(r->slot_index);
    const esp_partition_t *part = pw_partition(part_idx);
    esp_err_t rc;
    uint8_t *buf;

    if (!slot || !part) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot->kind != IO_SLOT_KIND_SLOT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (r->len > slot->capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    buf = (uint8_t *)heap_caps_malloc(r->len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    rc = esp_partition_read(part, r->data_off, buf, r->len);
    if (rc == ESP_OK) {
        rc = io_disk_apply_slot(slot, buf, r->len);
    }
    heap_caps_free(buf);
    return rc;
}

esp_err_t io_payload_apply(void)
{
    if (!s_active.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    for (uint32_t i = 0; i < s_active.count; i++) {
        const esp_err_t rc = pw_apply_record(s_active.part, &s_active.rec[i]);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "apply record %u failed: %s", (unsigned)i, esp_err_to_name(rc));
            return rc;
        }
        ESP_LOGI(TAG, "overlay applied: slot %u %s len=%u",
                 (unsigned)s_active.rec[i].slot_index,
                 io_disk_slot_at(s_active.rec[i].slot_index)->name,
                 (unsigned)s_active.rec[i].len);
    }
    return ESP_OK;
}

esp_err_t io_payload_init(void)
{
    if (!pw_load_active(&s_active)) {
        ESP_LOGI(TAG, "no valid payload in flash (factory state)");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "flash payload active: part=%s seq=%u records=%u",
             (s_active.part == 0) ? "script0" : "script1",
             (unsigned)s_active.seq, (unsigned)s_active.count);
    return io_payload_apply();
}

/* ------------------------------------------------------------------ 写入 */

esp_err_t io_payload_store(uint32_t slot_index, const void *data, uint32_t len)
{
    const io_slot_desc_t *slot = io_disk_slot_at(slot_index);
    const esp_partition_t *src_part;
    const esp_partition_t *dst_part;
    pw_record_t recs[PW_MAX_RECORDS];
    uint32_t count = 0;
    uint32_t blob_off;
    uint32_t total;
    uint8_t *buf = NULL;
    esp_err_t rc = ESP_OK;
    int dst_idx;
    uint32_t crc;

    if (!slot || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot->kind != IO_SLOT_KIND_SLOT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > slot->capacity) {
        ESP_LOGE(TAG, "payload %u bytes exceeds slot %s capacity %u",
                 (unsigned)len, slot->name, (unsigned)slot->capacity);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 1) 组装记录列表:沿用当前生效的记录,目标槽位替换/追加 */
    if (s_active.valid) {
        for (uint32_t i = 0; i < s_active.count && count < PW_MAX_RECORDS; i++) {
            recs[count++] = s_active.rec[i];
        }
    }
    bool replaced = false;
    for (uint32_t i = 0; i < count; i++) {
        if (recs[i].slot_index == slot_index) {
            recs[i].len = len;
            recs[i].crc32 = pw_crc32(0, data, len);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        if (count >= PW_MAX_RECORDS) {
            return ESP_ERR_NO_MEM;
        }
        recs[count].slot_index = slot_index;
        recs[count].len = len;
        recs[count].crc32 = pw_crc32(0, data, len);
        recs[count].data_off = 0;
        count++;
    }

    /* 2) 布局:头 + 目录 + 512 对齐的 payload 块 */
    blob_off = PW_HDR_SIZE + count * PW_REC_SIZE;
    blob_off = (blob_off + (PW_ALIGN - 1)) & ~(PW_ALIGN - 1);
    total = blob_off;
    for (uint32_t i = 0; i < count; i++) {
        recs[i].data_off = total;
        total += (recs[i].len + (PW_ALIGN - 1)) & ~(PW_ALIGN - 1);
    }

    dst_idx = s_active.valid ? (1 - s_active.part) : 0;
    dst_part = pw_partition(dst_idx);
    src_part = s_active.valid ? pw_partition(s_active.part) : NULL;
    if (!dst_part || total > dst_part->size) {
        ESP_LOGE(TAG, "payload snapshot %u bytes does not fit partition %s",
                 (unsigned)total, dst_part ? "?" : "(missing)");
        return ESP_ERR_NO_MEM;
    }

    buf = (uint8_t *)heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    /* 3) 填内容:目标槽位用新数据,其它槽位从当前生效分区搬过来 */
    for (uint32_t i = 0; i < count; i++) {
        if (recs[i].slot_index == slot_index) {
            memcpy(buf + recs[i].data_off, data, len);
        } else if (src_part) {
            const pw_record_t *old = NULL;
            for (uint32_t k = 0; k < s_active.count; k++) {
                if (s_active.rec[k].slot_index == recs[i].slot_index) {
                    old = &s_active.rec[k];
                    break;
                }
            }
            if (!old) {
                rc = ESP_ERR_NOT_FOUND;
                goto done;
            }
            if (esp_partition_read(src_part, old->data_off,
                                   buf + recs[i].data_off, old->len) != ESP_OK) {
                rc = ESP_FAIL;
                goto done;
            }
        }
    }

    /* 4) 写头 + 目录 */
    pw_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, PW_MAGIC, PW_MAGIC_LEN);
    hdr.version = PW_VERSION;
    hdr.seq = s_active.valid ? (s_active.seq + 1) : 1;
    hdr.count = count;
    hdr.dir_crc = pw_crc32(0, recs, count * PW_REC_SIZE);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + PW_HDR_SIZE, recs, count * PW_REC_SIZE);

    /* 5) 擦 + 写(写非当前生效的分区 -> 写到一半掉电不影响当前生效的那份) */
    const int64_t t0 = esp_timer_get_time();
    rc = esp_partition_erase_range(dst_part, 0, dst_part->size);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(rc));
        goto done;
    }
    rc = esp_partition_write(dst_part, 0, buf, total);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(rc));
        goto done;
    }
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "flash write done in %lld ms (part=%s seq=%u records=%u total=%u)",
             (long long)ms, (dst_idx == 0) ? "script0" : "script1",
             (unsigned)hdr.seq, (unsigned)count, (unsigned)total);

    /* 6) 回读校验:新分区必须能被完整校验通过,否则这次写入不算数 */
    pw_snapshot_t check;
    if (!pw_load(dst_idx, &check) || check.seq != hdr.seq) {
        ESP_LOGE(TAG, "readback verify failed");
        rc = ESP_FAIL;
        goto done;
    }

    /* 7) 生效
     *
     * 只更新 flash 侧的生效快照,**不动 RAM disk 槽位**。
     * RAM disk 槽位是"每次开机从 flash 铺开的副本";运行期改它 = 和宿主抢
     * 同一块存储(宿主可能已删槽文件、簇被释放,或已建了占用同簇的新文件)。
     * 所以运行期下发一律"落 flash -> 下次开机生效"。
     * 要"立刻生效"由调用方在 store 之后显式调 io_payload_apply() */
    s_active = check;
    crc = pw_crc32(0, data, len);
    ESP_LOGI(TAG, "payload stored: slot %s len=%u crc=0x%08X seq=%u "
             "(flash only, effective after reboot)",
             slot->name, (unsigned)len, (unsigned)crc, (unsigned)hdr.seq);

done:
    heap_caps_free(buf);
    return rc;
}

/* ------------------------------------------------------------------ 查询 */

uint32_t io_payload_active_seq(void)
{
    return s_active.valid ? s_active.seq : 0;
}

uint32_t io_payload_active_len(uint32_t slot_index)
{
    if (!s_active.valid) {
        return 0;
    }
    for (uint32_t i = 0; i < s_active.count; i++) {
        if (s_active.rec[i].slot_index == slot_index) {
            return s_active.rec[i].len;
        }
    }
    return 0;
}

bool io_payload_active_matches(uint32_t slot_index, uint32_t len, uint32_t crc32)
{
    if (!s_active.valid) {
        return false;
    }
    for (uint32_t i = 0; i < s_active.count; i++) {
        if (s_active.rec[i].slot_index == slot_index) {
            return s_active.rec[i].len == len && s_active.rec[i].crc32 == crc32;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ 信箱 */

/*
 * 信箱:宿主往 RAM disk 的 PAYLOAD.BIN 里写 [64 字节头][payload],设备轮询到
 * 变化就落 flash。这条通道是"还没上 WiFi/MCP 时"也能下发的路径,也是
 * update 在 M5 落地前的等价物(MCP 版本直接调 io_payload_store)。
 *
 * 宿主必须**按信箱容量原样大小**覆写(不够就补零):这样簇链不变,设备才知道
 * payload 在卷里的位置。文件内容超容量 = 拒绝,设备不替宿主改 FAT。
 */
#define INGEST_MAGIC     "IOPB"
#define INGEST_HDR_SIZE  64
#define INGEST_POLL_MS   200

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint32_t version;
    uint32_t slot_index;
    uint32_t len;
    uint32_t crc32;
    uint32_t flags;        /* bit0 = commit */
    uint8_t  reserved[36];
} ingest_hdr_t;

static const io_slot_desc_t *ingest_slot(void)
{
    uint32_t count = 0;
    const io_slot_desc_t *tbl = io_disk_slot_table(&count);
    for (uint32_t i = 0; i < count; i++) {
        if (tbl[i].kind == IO_SLOT_KIND_INBOX) {
            return &tbl[i];
        }
    }
    return NULL;
}

static void ingest_task(void *arg)
{
    const io_slot_desc_t *inbox = (const io_slot_desc_t *)arg;
    ingest_hdr_t hdr;
    uint8_t *payload = NULL;
    uint32_t payload_cap = 0;
    bool seen_valid = false;
    bool boot_sync = true;
    bool magic_warned = false;
    bool crc_warned = false;
    uint32_t seen_slot = 0, seen_len = 0, seen_crc = 0;

    ESP_LOGI(TAG, "ingest task started on %s (capacity %u)",
             inbox->name, (unsigned)inbox->capacity);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(INGEST_POLL_MS));

        if (io_disk_read_range(inbox->data_offset, &hdr, sizeof(hdr)) != ESP_OK) {
            continue;
        }
        if (memcmp(hdr.magic, INGEST_MAGIC, 4) != 0 || hdr.version != 1u) {
            if (!magic_warned) {
                magic_warned = true;
                ESP_LOGW(TAG, "ingest: no valid header at %s (magic=%02X%02X%02X%02X) "
                         "-- host wrote the file somewhere else?",
                         inbox->name, hdr.magic[0], hdr.magic[1], hdr.magic[2], hdr.magic[3]);
            }
            continue;
        }
        if (!(hdr.flags & 1u)) {
            continue;   /* 还没 commit */
        }
        if (!seen_valid || hdr.slot_index != seen_slot ||
            hdr.len != seen_len || hdr.crc32 != seen_crc) {
            ESP_LOGI(TAG, "ingest: header seen: slot=%u len=%u crc=0x%08X",
                     (unsigned)hdr.slot_index, (unsigned)hdr.len, (unsigned)hdr.crc32);
            crc_warned = false;

            if (boot_sync) {
                /* 开机第一次看到信箱:如果 flash 里已经有同一份,别重复擦写 */
                boot_sync = false;
                if (io_payload_active_matches(hdr.slot_index, hdr.len, hdr.crc32)) {
                    ESP_LOGI(TAG, "ingest: inbox already matches flash snapshot");
                    seen_valid = true;
                    seen_slot = hdr.slot_index;
                    seen_len = hdr.len;
                    seen_crc = hdr.crc32;
                    continue;
                }
            }

            const io_slot_desc_t *target = io_disk_slot_at(hdr.slot_index);
            if (!target || target->kind != IO_SLOT_KIND_SLOT) {
                ESP_LOGW(TAG, "ingest: bad slot index %u", (unsigned)hdr.slot_index);
                seen_valid = true;
                seen_slot = hdr.slot_index;
                seen_len = hdr.len;
                seen_crc = hdr.crc32;
                continue;
            }
            if (hdr.len == 0 || hdr.len > target->capacity) {
                ESP_LOGW(TAG, "ingest: len %u exceeds slot %s capacity %u",
                         (unsigned)hdr.len, target->name, (unsigned)target->capacity);
                seen_valid = true;
                seen_slot = hdr.slot_index;
                seen_len = hdr.len;
                seen_crc = hdr.crc32;
                continue;
            }
            if (hdr.len + INGEST_HDR_SIZE > inbox->capacity) {
                ESP_LOGW(TAG, "ingest: payload does not fit inbox");
                continue;
            }

            if (!payload || payload_cap < hdr.len) {
                heap_caps_free(payload);
                payload_cap = hdr.len;
                payload = (uint8_t *)heap_caps_malloc(payload_cap, MALLOC_CAP_SPIRAM);
                if (!payload) {
                    payload_cap = 0;
                    continue;
                }
            }
            if (io_disk_read_range(inbox->data_offset + INGEST_HDR_SIZE,
                                   payload, hdr.len) != ESP_OK) {
                continue;
            }
            if (pw_crc32(0, payload, hdr.len) != hdr.crc32) {
                /* 宿主可能还在写,下一轮再试 */
                if (!crc_warned) {
                    crc_warned = true;
                    ESP_LOGW(TAG, "ingest: crc mismatch (want 0x%08X got 0x%08X) "
                             "-- host still writing?",
                             (unsigned)hdr.crc32,
                             (unsigned)pw_crc32(0, payload, hdr.len));
                }
                continue;
            }

            /* 运行期下发:只写 flash,不动 RAM disk 槽位(下次开机生效) */
            const esp_err_t rc = io_payload_store(hdr.slot_index, payload, hdr.len);
            if (rc == ESP_OK) {
                seen_valid = true;
                seen_slot = hdr.slot_index;
                seen_len = hdr.len;
                seen_crc = hdr.crc32;
            } else {
                ESP_LOGE(TAG, "ingest: store failed: %s", esp_err_to_name(rc));
            }
        }
    }
}

esp_err_t io_payload_start_ingest(void)
{
    const io_slot_desc_t *inbox = ingest_slot();
    if (!inbox) {
        ESP_LOGW(TAG, "no inbox slot in the slot table");
        return ESP_ERR_NOT_FOUND;
    }
    if (xTaskCreate(ingest_task, "ingest", 4096, (void *)inbox, 4, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
