#pragma once
#include <nord/types.hpp>

#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL
#define LIMINE_REQ __attribute__((used, section(".limine_requests")))

namespace limine {

enum : nord::u64 {
    MEMMAP_USABLE                 = 0,
    MEMMAP_RESERVED               = 1,
    MEMMAP_ACPI_RECLAIMABLE       = 2,
    MEMMAP_ACPI_NVS               = 3,
    MEMMAP_BAD_MEMORY             = 4,
    MEMMAP_BOOTLOADER_RECLAIMABLE = 5,
    MEMMAP_KERNEL_AND_MODULES     = 6,
    MEMMAP_FRAMEBUFFER            = 7,
};

struct MemmapEntry   { nord::u64 base, length, type; };
struct MemmapResponse{ nord::u64 revision, entry_count; MemmapEntry** entries; };
struct MemmapRequest { nord::u64 id[4]; nord::u64 revision; MemmapResponse* response; };

struct HhdmResponse  { nord::u64 revision, offset; };
struct HhdmRequest   { nord::u64 id[4]; nord::u64 revision; HhdmResponse* response; };

struct Framebuffer {
    void*     address;
    nord::u64 width, height, pitch;
    nord::u16 bpp;
    nord::u8  memory_model;
    nord::u8  red_mask_size,   red_mask_shift;
    nord::u8  green_mask_size, green_mask_shift;
    nord::u8  blue_mask_size,  blue_mask_shift;
    nord::u8  unused[7];
    nord::u64 edid_size;
    void*     edid;
    nord::u64 mode_count;
    void**    modes;
};
struct FramebufferResponse { nord::u64 revision, framebuffer_count; Framebuffer** framebuffers; };
struct FramebufferRequest  { nord::u64 id[4]; nord::u64 revision; FramebufferResponse* response; };

struct KernelAddressResponse { nord::u64 revision, physical_base, virtual_base; };
struct KernelAddressRequest  { nord::u64 id[4]; nord::u64 revision; KernelAddressResponse* response; };

struct RsdpResponse { nord::u64 revision; void* address; };
struct RsdpRequest  { nord::u64 id[4]; nord::u64 revision; RsdpResponse* response; };

}

#define LIMINE_MEMMAP_REQUEST         { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806fULL, 0xe304acdfc50c3c62ULL }
#define LIMINE_HHDM_REQUEST           { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852ULL, 0x63984e959a98244bULL }
#define LIMINE_FRAMEBUFFER_REQUEST    { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75ULL, 0xa3148604f6fab11bULL }
#define LIMINE_KERNEL_ADDRESS_REQUEST { LIMINE_COMMON_MAGIC, 0x71ba76863cc55f63ULL, 0xb2644a48c516a487ULL }
#define LIMINE_RSDP_REQUEST           { LIMINE_COMMON_MAGIC, 0xc5e77b6b397e7b43ULL, 0x27637845accdcf3cULL }