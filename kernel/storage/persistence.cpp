#include <storage/persistence.hpp>
#include <block/block.hpp>
#include <drivers/block/virtio_blk.hpp>
#include <fs/nvmfs.hpp>
#include <lib/print.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <nord/types.hpp>

namespace nord::storage::persistence {

static constexpr u32 SNAPSHOT_MAGIC = 0x434F4E44;
static constexpr u16 SNAPSHOT_VERSION = 1;
static constexpr usize SECTOR_SIZE = 512;
static constexpr u64 SLOT_SECTORS = 640;
static constexpr u64 SLOT0_LBA = 8;
static constexpr u64 SLOT1_LBA = SLOT0_LBA + SLOT_SECTORS;
static constexpr usize SLOT_BYTES = SLOT_SECTORS * SECTOR_SIZE;

struct [[gnu::packed]] DiskHeader {
    u32 magic;
    u16 version;
    u16 header_size;
    u64 generation;
    u32 payload_size;
    u32 checksum;
    u8 reserved[8];
};

static bool g_available;
static bool g_dirty;
static u8 g_active_slot;
static u64 g_generation;
static const block::BlockDevice* g_blk = nullptr;

static u32 checksum(const u8* data, usize size) {
    u32 value = 0xFFFFFFFF;
    for (usize i = 0; i < size; ++i) {
        value ^= data[i];
        for (u32 bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (0xEDB88320U & (0U - (value & 1U)));
    }
    return ~value;
}

static void mark_dirty() {
    g_dirty = true;
}

static bool read_slot(u64 lba, u8* buffer, DiskHeader& header) {
    if (!g_blk || !g_blk->read_sector(lba, buffer)) return false;
    memcpy(&header, buffer, sizeof(header));
    if (header.magic != SNAPSHOT_MAGIC || header.version != SNAPSHOT_VERSION
        || header.header_size != sizeof(DiskHeader)
        || header.payload_size > SLOT_BYTES - SECTOR_SIZE) return false;

    const usize sectors = (header.payload_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    for (usize sector = 0; sector < sectors; ++sector) {
        if (!g_blk->read_sector(
                lba + 1 + sector, buffer + SECTOR_SIZE + sector * SECTOR_SIZE)) return false;
    }
    return checksum(buffer + SECTOR_SIZE, header.payload_size) == header.checksum;
}

static void load_snapshot() {
    auto* first = static_cast<u8*>(mm::heap::kmalloc(SLOT_BYTES));
    auto* second = static_cast<u8*>(mm::heap::kmalloc(SLOT_BYTES));
    if (!first || !second) {
        if (first) mm::heap::kfree(first);
        if (second) mm::heap::kfree(second);
        kprintf("[store] snapshot buffers unavailable\n");
        return;
    }

    DiskHeader first_header{};
    DiskHeader second_header{};
    const bool first_valid = read_slot(SLOT0_LBA, first, first_header);
    const bool second_valid = read_slot(SLOT1_LBA, second, second_header);
    bool restored = false;
    if (first_valid && second_valid) {
        const bool first_is_newer = first_header.generation >= second_header.generation;
        const u8* newest = first_is_newer ? first : second;
        const u8* older = first_is_newer ? second : first;
        const DiskHeader& newest_header = first_is_newer ? first_header : second_header;
        const DiskHeader& older_header = first_is_newer ? second_header : first_header;
        restored = nvmfs::deserialize(newest + SECTOR_SIZE, newest_header.payload_size);
        if (restored) {
            g_active_slot = first_is_newer ? 0 : 1;
            g_generation = newest_header.generation;
        } else {
            restored = nvmfs::deserialize(older + SECTOR_SIZE, older_header.payload_size);
            if (restored) {
                g_active_slot = first_is_newer ? 1 : 0;
                g_generation = older_header.generation;
            }
        }
    } else if (first_valid) {
        restored = nvmfs::deserialize(first + SECTOR_SIZE, first_header.payload_size);
        if (restored) {
            g_active_slot = 0;
            g_generation = first_header.generation;
        }
    } else if (second_valid) {
        restored = nvmfs::deserialize(second + SECTOR_SIZE, second_header.payload_size);
        if (restored) {
            g_active_slot = 1;
            g_generation = second_header.generation;
        }
    }

    g_dirty = false;
    if (restored) kprintf("[store] restored snapshot generation %lu\n", g_generation);
    else {
        g_generation = 0;
        g_active_slot = 1;
        kprintf("[store] no valid snapshot; starting empty\n");
    }

    mm::heap::kfree(first);
    mm::heap::kfree(second);
}

bool init() {
    g_available = drivers::block::virtio_blk::init();
    g_blk = block::device_by_name("virtio-blk");
    g_dirty = false;
    g_active_slot = 1;
    g_generation = 0;
    if (!g_available || !g_blk) {
        nvmfs::set_change_hook(nullptr);
        kprintf("[store] no persistent block device; NVMFS is volatile\n");
        return false;
    }
    if (g_blk->sector_count() < SLOT1_LBA + SLOT_SECTORS) {
        g_available = false;
        nvmfs::set_change_hook(nullptr);
        kprintf("[store] block device is too small for NVMFS snapshots\n");
        return false;
    }

    load_snapshot();
    nvmfs::set_change_hook(mark_dirty);
    return true;
}

bool available() { return g_available; }
bool dirty() { return g_dirty; }

bool sync() {
    if (!g_available) return false;
    if (!g_dirty) return true;
    auto* buffer = static_cast<u8*>(mm::heap::kmalloc(SLOT_BYTES));
    if (!buffer) return false;
    memset(buffer, 0, SLOT_BYTES);

    usize payload_size = 0;
    if (!nvmfs::serialize(buffer + SECTOR_SIZE, SLOT_BYTES - SECTOR_SIZE, payload_size)) {
        mm::heap::kfree(buffer);
        return false;
    }

    const u8 target_slot = g_active_slot ^ 1;
    const u64 target_lba = target_slot ? SLOT1_LBA : SLOT0_LBA;
    DiskHeader header{};
    header.magic = SNAPSHOT_MAGIC;
    header.version = SNAPSHOT_VERSION;
    header.header_size = sizeof(DiskHeader);
    header.generation = g_generation + 1;
    header.payload_size = payload_size;
    header.checksum = checksum(buffer + SECTOR_SIZE, payload_size);
    memcpy(buffer, &header, sizeof(header));

    const usize payload_sectors = (payload_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    bool success = true;
    for (usize sector = 0; sector < payload_sectors; ++sector) {
        if (!g_blk->write_sector(
                target_lba + 1 + sector, buffer + SECTOR_SIZE + sector * SECTOR_SIZE)) {
            success = false;
            break;
        }
    }
    if (success) success = g_blk->write_sector(target_lba, buffer);
    mm::heap::kfree(buffer);
    if (!success) return false;

    g_active_slot = target_slot;
    g_generation = header.generation;
    g_dirty = false;
    return true;
}

}