#include <drivers/block/virtio_blk.hpp>
#include <block/block.hpp>
#include <arch/x86_64/dev/pci.hpp>
#include <arch/x86_64/dev/timer.hpp>
#include <arch/x86_64/io.hpp>
#include <lib/print.hpp>
#include <lib/string.hpp>
#include <mm/pmm.hpp>

namespace nord::drivers::block::virtio_blk {

static constexpr u16 VIRTIO_VENDOR = 0x1AF4;
static constexpr u16 VIRTIO_BLOCK_LEGACY = 0x1001;
static constexpr u16 PCI_COMMAND = 0x04;
static constexpr u16 PCI_BAR0 = 0x10;
static constexpr u16 SECTOR_SIZE = 512;
static constexpr u32 QUEUE_SELECT = 0x0E;
static constexpr u32 QUEUE_SIZE = 0x0C;
static constexpr u32 QUEUE_PFN = 0x08;
static constexpr u32 QUEUE_NOTIFY = 0x10;
static constexpr u32 DEVICE_STATUS = 0x12;
static constexpr u32 INTERRUPT_ACK = 0x13;
static constexpr u32 DEVICE_CAPACITY = 0x14;
static constexpr u16 DESC_NEXT = 1;
static constexpr u16 DESC_DEVICE_WRITE = 2;

struct [[gnu::packed]] Descriptor {
    u64 address;
    u32 length;
    u16 flags;
    u16 next;
};

struct [[gnu::packed]] RequestHeader {
    u32 type;
    u32 reserved;
    u64 sector;
};

static arch::pci::Device g_pci_device{};
static u16 g_io_base;
static u16 g_queue_size;
static u64 g_sector_count;
static paddr_t g_queue_phys;
static usize g_queue_pages;
static paddr_t g_request_phys;
static bool g_ready;
static bool flush_cb();
static bool transfer(u64 sector, void* buffer, bool write);
static u8 in8(u32 offset);
static u16 in16(u32 offset);
static u32 in32(u32 offset);
static void out8(u32 offset, u8 value);
static void out16(u32 offset, u16 value);
static void out32(u32 offset, u32 value);

static u64 sector_count_cb() { return g_sector_count; }
static bool read_sector_cb(u64 sector, void* buffer) {
    return g_ready ? transfer(sector, buffer, false) : false;
}
static bool write_sector_cb(u64 sector, const void* buffer) {
    return g_ready ? transfer(sector, const_cast<void*>(buffer), true) : false;
}
static bool flush_cb() {
    if (!g_ready) return false;
    out16(QUEUE_NOTIFY, 0);
    return true;
}


static u8 in8(u32 offset) { return arch::inb(static_cast<u16>(g_io_base + offset)); }
static u16 in16(u32 offset) { return arch::inw(static_cast<u16>(g_io_base + offset)); }
static u32 in32(u32 offset) { return arch::inl(static_cast<u16>(g_io_base + offset)); }
static void out8(u32 offset, u8 value) { arch::outb(static_cast<u16>(g_io_base + offset), value); }
static void out16(u32 offset, u16 value) { arch::outw(static_cast<u16>(g_io_base + offset), value); }
static void out32(u32 offset, u32 value) { arch::outl(static_cast<u16>(g_io_base + offset), value); }

bool init() {
    g_ready = false;
    if (!arch::pci::find(VIRTIO_VENDOR, VIRTIO_BLOCK_LEGACY, g_pci_device)) {
        kprintf("[virtio-blk] no legacy PCI device found\n");
        return false;
    }

    u16 command = arch::pci::read16(g_pci_device, PCI_COMMAND);
    arch::pci::write16(g_pci_device, PCI_COMMAND, command | 0x0005);
    const u32 bar0 = arch::pci::read32(g_pci_device, PCI_BAR0);
    if (!(bar0 & 1)) {
        kprintf("[virtio-blk] BAR0 is not an I/O BAR\n");
        return false;
    }
    g_io_base = static_cast<u16>(bar0 & 0xFFFC);

    out8(DEVICE_STATUS, 0);
    out8(DEVICE_STATUS, 1);
    out8(DEVICE_STATUS, 3);
    out32(4, 0);
    out16(QUEUE_SELECT, 0);
    g_queue_size = in16(QUEUE_SIZE);
    if (g_queue_size < 3) {
        out8(DEVICE_STATUS, 0x80);
        kprintf("[virtio-blk] queue is too small\n");
        return false;
    }

    const u64 descriptor_bytes = static_cast<u64>(g_queue_size) * sizeof(Descriptor);
    const u64 available_offset = descriptor_bytes;
    const u64 available_bytes = 6 + static_cast<u64>(g_queue_size) * 2;
    const u64 used_offset = align_up(available_offset + available_bytes, PAGE_SIZE);
    const u64 used_bytes = 6 + static_cast<u64>(g_queue_size) * 8;
    const u64 total_bytes = used_offset + used_bytes;
    g_queue_pages = align_up(total_bytes, PAGE_SIZE) / PAGE_SIZE;
    g_queue_phys = mm::pmm::alloc_pages(g_queue_pages);
    g_request_phys = mm::pmm::alloc_page();
    if (!g_queue_phys || !g_request_phys || (g_queue_phys >> 12) > 0xFFFFFFFFULL) {
        if (g_queue_phys) mm::pmm::free_pages(g_queue_phys, g_queue_pages);
        if (g_request_phys) mm::pmm::free_page(g_request_phys);
        out8(DEVICE_STATUS, 0x80);
        kprintf("[virtio-blk] DMA memory allocation failed\n");
        return false;
    }

    out32(QUEUE_PFN, static_cast<u32>(g_queue_phys >> 12));
    out8(DEVICE_STATUS, 7);
    g_sector_count = static_cast<u64>(in32(DEVICE_CAPACITY))
        | (static_cast<u64>(in32(DEVICE_CAPACITY + 4)) << 32);
    g_ready = g_sector_count != 0;
    if (!g_ready) {
        kprintf("[virtio-blk] device reports zero sectors\n");
        return false;
    }
    kprintf("[virtio-blk] %lu sectors, queue %u, I/O base %#x\n",
            g_sector_count, static_cast<u32>(g_queue_size), static_cast<u32>(g_io_base));
    nord::block::BlockDevice dev{};
    dev.name = "virtio-blk";
    dev.sector_count = sector_count_cb;
    dev.read_sector = read_sector_cb;
    dev.write_sector = write_sector_cb;
    dev.flush = flush_cb;
    nord::block::register_device(dev);

    return true;
}

bool ready() { return g_ready; }
u64 sector_count() { return g_sector_count; }

static bool transfer(u64 sector, void* buffer, bool write) {
    if (!g_ready || !buffer || sector >= g_sector_count) return false;

    auto* queue_memory = static_cast<u8*>(mm::pmm::phys_to_virt<void>(g_queue_phys));
    auto* descriptors = reinterpret_cast<Descriptor*>(queue_memory);
    auto* available_flags = reinterpret_cast<volatile u16*>(queue_memory + g_queue_size * sizeof(Descriptor));
    auto* available_index = available_flags + 1;
    auto* available_ring = reinterpret_cast<volatile u16*>(available_index + 1);
    const u64 used_offset = align_up(
        static_cast<u64>(g_queue_size) * sizeof(Descriptor) + 6 + static_cast<u64>(g_queue_size) * 2,
        PAGE_SIZE);
    auto* used_memory = queue_memory + used_offset;
    auto* used_index = reinterpret_cast<volatile u16*>(used_memory + 2);

    auto* request = static_cast<u8*>(mm::pmm::phys_to_virt<void>(g_request_phys));
    auto* header = reinterpret_cast<RequestHeader*>(request);
    header->type = write ? 1 : 0;
    header->reserved = 0;
    header->sector = sector;
    auto* data = request + sizeof(RequestHeader);
    auto* status = data + SECTOR_SIZE;
    *status = 0xFF;
    if (write) memcpy(data, buffer, SECTOR_SIZE);

    descriptors[0] = {g_request_phys, sizeof(RequestHeader), DESC_NEXT, 1};
    descriptors[1] = {g_request_phys + sizeof(RequestHeader), SECTOR_SIZE,
                       static_cast<u16>(DESC_NEXT | (write ? 0 : DESC_DEVICE_WRITE)), 2};
    descriptors[2] = {g_request_phys + sizeof(RequestHeader) + SECTOR_SIZE,
                       1, DESC_DEVICE_WRITE, 0};

    const u16 old_used = *used_index;
    const u16 avail = *available_index;
    available_ring[avail % g_queue_size] = 0;
    asm volatile("mfence" ::: "memory");
    *available_index = static_cast<u16>(avail + 1);
    asm volatile("mfence" ::: "memory");
    out16(QUEUE_NOTIFY, 0);

    const u64 start_tick = arch::timer::ticks();
    while (*used_index == old_used) {
        if (arch::timer::ticks() - start_tick > 500) return false;
        asm volatile("pause");
    }
    asm volatile("mfence" ::: "memory");
    (void)in8(INTERRUPT_ACK);
    if (*status != 0) return false;
    if (!write) memcpy(buffer, data, SECTOR_SIZE);
    return true;
}

bool read_sector(u64 sector, void* buffer) { return transfer(sector, buffer, false); }
bool write_sector(u64 sector, const void* buffer) {
    return transfer(sector, const_cast<void*>(buffer), true);
}
bool flush() { return flush_cb(); }

}