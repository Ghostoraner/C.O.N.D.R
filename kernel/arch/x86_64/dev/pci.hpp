#pragma once
#include <nord/types.hpp>

namespace nord::arch::pci {

struct Device {
    u8 bus;
    u8 slot;
    u8 function;
};

bool find(u16 vendor_id, u16 device_id, Device& device);
u32 read32(const Device& device, u8 offset);
u16 read16(const Device& device, u8 offset);
void write16(const Device& device, u8 offset, u16 value);

}