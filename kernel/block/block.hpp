#pragma once
#include <nord/types.hpp>

namespace nord::block {

struct BlockDevice {
    const char* name;
    u64 (*sector_count)();
    bool (*read_sector)(u64 sector, void* buffer);
    bool (*write_sector)(u64 sector, const void* buffer);
    bool (*flush)();
};

static constexpr usize MAX_DEVICES = 8;

bool register_device(const BlockDevice& device);
const BlockDevice* device_by_index(usize index);
const BlockDevice* device_by_name(const char* name);
usize device_count();

} // namespace nord::block
