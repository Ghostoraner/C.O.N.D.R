#pragma once
#include <nord/types.hpp>

namespace nord::drivers::block::virtio_blk {

bool init();
bool ready();
u64 sector_count();
bool read_sector(u64 sector, void* buffer);
bool write_sector(u64 sector, const void* buffer);
bool flush();

}