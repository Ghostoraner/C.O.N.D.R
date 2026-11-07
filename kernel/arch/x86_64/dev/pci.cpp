#include <arch/x86_64/dev/pci.hpp>
#include <arch/x86_64/io.hpp>

namespace nord::arch::pci {

static constexpr u16 CONFIG_ADDRESS = 0xCF8;
static constexpr u16 CONFIG_DATA = 0xCFC;

static void select(const Device& device, u8 offset) {
    const u32 address = 0x80000000U
        | (static_cast<u32>(device.bus) << 16)
        | (static_cast<u32>(device.slot) << 11)
        | (static_cast<u32>(device.function) << 8)
        | (offset & 0xFC);
    outl(CONFIG_ADDRESS, address);
}

u32 read32(const Device& device, u8 offset) {
    select(device, offset);
    return inl(CONFIG_DATA);
}

u16 read16(const Device& device, u8 offset) {
    select(device, offset);
    return inw(static_cast<u16>(CONFIG_DATA + (offset & 2)));
}

void write16(const Device& device, u8 offset, u16 value) {
    select(device, offset);
    outw(static_cast<u16>(CONFIG_DATA + (offset & 2)), value);
}

bool find(u16 vendor_id, u16 device_id, Device& result) {
    for (u32 bus = 0; bus < 256; ++bus) {
        for (u32 slot = 0; slot < 32; ++slot) {
            for (u32 function = 0; function < 8; ++function) {
                const Device device{static_cast<u8>(bus), static_cast<u8>(slot),
                                    static_cast<u8>(function)};
                const u32 identity = read32(device, 0);
                if (identity == 0xFFFFFFFFU) continue;
                if (static_cast<u16>(identity) == vendor_id
                    && static_cast<u16>(identity >> 16) == device_id) {
                    result = device;
                    return true;
                }
            }
        }
    }
    return false;
}

}