#include <process/process.hpp>
#include <arch/x86_64/cpu/scheduler.hpp>
#include <fs/nvmfs.hpp>
#include <lib/print.hpp>
#include <lib/string.hpp>
#include <mm/address_space.hpp>
#include <mm/elf_loader.hpp>
#include <mm/heap.hpp>

namespace nord::process {

static constexpr usize MAX_VECTOR = 64;

static bool vector_length(const char* const* vector, usize& length) {
    length = 0;
    if (!vector) return true;
    for (; length <= MAX_VECTOR; ++length)
        if (!vector[length]) return true;
    return false;
}

bool spawn(const char* path, const char* const* argv, const char* const* envp, u32& pid) {
    pid = 0;
    if (!path || !*path) return false;

    usize argc = 0;
    usize envc = 0;
    if (!vector_length(argv, argc) || !vector_length(envp, envc)) return false;

    // Copy the ELF stream while the NVMFS lock keeps the node alive, then
    // release it before allocating page tables or scheduling the new task.
    u8* image = nullptr;
    usize image_size = 0;
    {
        nvmfs::Lock guard;
        nvmfs::NodeInfo info{};
        if (!nvmfs::find_name(path, nvmfs::NodeType::file, info)
            || !info.stream || !info.stream_size) return false;
        image_size = info.stream_size;
        image = static_cast<u8*>(mm::heap::kmalloc(image_size));
        if (!image) return false;
        memcpy(image, info.stream, image_size);
    }

    mm::address_space::AddressSpace space{};
    if (!mm::address_space::create(space)) {
        mm::heap::kfree(image);
        return false;
    }

    u64 entry = 0;
    u64 stack = 0;
    if (!mm::elf_loader::load(space, image, image_size, argv, argc,
                              envp, envc, path, entry, stack)) {
        mm::address_space::destroy(space);
        mm::heap::kfree(image);
        return false;
    }
    mm::heap::kfree(image);

    u32 id = 0;
    if (!arch::scheduler::create_user(path, space, entry, stack, id)) {
        mm::address_space::destroy(space);
        return false;
    }
    pid = id;
    return true;
}

static bool install_file(const char* name, const u8* image, usize size, bool& created) {
    created = false;
    nvmfs::Lock guard;
    nvmfs::NodeInfo existing{};
    if (nvmfs::find_name(name, nvmfs::NodeType::file, existing)) return true;

    nvmfs::NodeId id = 0;
    nvmfs::Error error;
    if (!nvmfs::create(nvmfs::NodeType::file, name, 0, id, error)) return false;
    if (!nvmfs::write(id, image, size, false, error)) {
        nvmfs::Error ignored;
        nvmfs::erase(id, ignored);
        return false;
    }
    created = true;
    return true;
}

void install_builtins() {
    usize size = 0;
    const u8* image = mm::elf_loader::args_image(size);
    if (!image || !size) return;
    bool created = false;
    if (!install_file("args", image, size, created)) return;
    if (created) kprintf("[spawn] installed built-in 'args' (%lu bytes)\n", size);
}

}
