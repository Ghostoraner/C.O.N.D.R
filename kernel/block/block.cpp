#include <block/block.hpp>
#include <lib/string.hpp>
#include <lib/print.hpp>

namespace nord::block {

static BlockDevice g_devices[MAX_DEVICES];
static usize g_device_count = 0;

static bool same(const char* left, const char* right) {
    return left && right && strcmp(left, right) == 0;
}

bool register_device(const BlockDevice& device) {
    if (!device.name || !device.read_sector || !device.write_sector) return false;
    if (g_device_count >= MAX_DEVICES) return false;
    if (device_by_name(device.name)) return false;
    g_devices[g_device_count++] = device;
    kprintf("[block] registered device '%s'\\n", device.name);
    return true;
}

const BlockDevice* device_by_index(usize index) {
    if (index >= g_device_count) return nullptr;
    return &g_devices[index];
}

const BlockDevice* device_by_name(const char* name) {
    for (usize i = 0; i < g_device_count; ++i)
        if (same(g_devices[i].name, name)) return &g_devices[i];
    return nullptr;
}

usize device_count() { return g_device_count; }

} // namespace nord::block
