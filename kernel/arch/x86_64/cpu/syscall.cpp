#include <arch/x86_64/cpu/syscall.hpp>
#include <arch/x86_64/cpu/scheduler.hpp>
#include <arch/x86_64/dev/keyboard.hpp>
#include <fs/nvmfs.hpp>
#include <ipc/ipc.hpp>
#include <lib/print.hpp>
#include <mm/address_space.hpp>
#include <mm/heap.hpp>
#include <process/process.hpp>

namespace nord::arch::syscall {

static constexpr u64 SYS_WRITE = 1;
static constexpr u64 SYS_EXIT = 2;
static constexpr u64 SYS_SERVICE_REGISTER = 3;
static constexpr u64 SYS_SERVICE_CONNECT = 4;
static constexpr u64 SYS_SERVICE_ACCEPT = 5;
static constexpr u64 SYS_IPC_SEND = 6;
static constexpr u64 SYS_IPC_RECEIVE = 7;
static constexpr u64 SYS_READ_CHAR = 8;
static constexpr u64 SYS_KBD_FOCUS = 9;
static constexpr u64 SYS_WAIT = 10;
static constexpr u64 SYS_SLEEP_MS = 11;
static constexpr u64 SYS_SPAWN = 12;
static constexpr u64 SYS_BRK = 13;
static constexpr u64 SYS_MMAP = 14;
static constexpr usize MAX_WRITE_SIZE = 256;
static constexpr usize MAX_READ_CHAR_SIZE = 256;
static constexpr usize MAX_SERVICE_NAME = 24;
static constexpr usize MAX_SPAWN_PATH = 64;
static constexpr usize MAX_SPAWN_VECTOR = 64;
static constexpr usize MAX_SPAWN_BYTES = 4096;

struct SpawnCopy {
    char bytes[MAX_SPAWN_BYTES];
    usize used;
    const char* argv[MAX_SPAWN_VECTOR + 1];
    const char* envp[MAX_SPAWN_VECTOR + 1];
};

static bool copy_user_string(mm::address_space::AddressSpace& space, vaddr_t source,
                             char* destination, usize capacity, usize& length) {
    if (!source) return false;
    for (usize i = 0; i < capacity; ++i) {
        char character = 0;
        if (!mm::address_space::copy_from_user(space, &character, source + i, 1))
            return false;
        destination[i] = character;
        if (!character) {
            length = i;
            return true;
        }
    }
    return false;
}

static bool copy_user_vector(mm::address_space::AddressSpace& space,
                             vaddr_t user_vector, SpawnCopy& copy,
                             const char* (&vector)[MAX_SPAWN_VECTOR + 1]) {
    if (!user_vector) {
        vector[0] = nullptr;
        return true;
    }
    for (usize count = 0; count <= MAX_SPAWN_VECTOR; ++count) {
        u64 pointer = 0;
        if (!mm::address_space::copy_from_user(
                space, &pointer, user_vector + count * sizeof(u64), sizeof(pointer)))
            return false;
        if (!pointer) {
            vector[count] = nullptr;
            return true;
        }
        if (count == MAX_SPAWN_VECTOR) return false;
        if (copy.used >= MAX_SPAWN_BYTES) return false;
        usize length = 0;
        char* destination = copy.bytes + copy.used;
        if (!copy_user_string(space, pointer, destination,
                              MAX_SPAWN_BYTES - copy.used, length)) return false;
        vector[count] = destination;
        copy.used += length + 1;
    }
    return false;
}

static bool read_service_name(mm::address_space::AddressSpace* space,
                              Registers* registers, char (&name)[MAX_SERVICE_NAME]) {
    const usize size = registers->rsi;
    if (!space || !size || size >= sizeof(name)
        || !mm::address_space::copy_from_user(*space, name, registers->rdi, size)) return false;
    name[size] = '\0';
    return true;
}

void dispatch(Registers* registers) {
    if (registers->rax == SYS_WRITE) {
        auto* space = scheduler::current_address_space();
        const usize size = registers->rsi;
        char buffer[MAX_WRITE_SIZE];
        if (!space || size > sizeof(buffer)
            || !mm::address_space::copy_from_user(*space, buffer, registers->rdi, size)) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        for (usize i = 0; i < size; ++i) kprintf("%c", buffer[i]);
        registers->rax = size;
        return;
    }
    if (registers->rax == SYS_EXIT) {
        scheduler::mark_current_exited(static_cast<i32>(registers->rdi));
        registers->rax = 0;
        return;
    }
    auto* space = scheduler::current_address_space();
    if (registers->rax == SYS_BRK) {
        vaddr_t result = 0;
        if (!space || !mm::address_space::brk(*space, registers->rdi, result)) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        registers->rax = result;
        return;
    }
    if (registers->rax == SYS_MMAP) {
        if (!space) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        // Standard argument order is addr, length, prot, flags. A bare
        // mmap(length) call (length in RDI, zero RSI) is also accepted.
        vaddr_t hint = registers->rdi;
        usize length = registers->rsi;
        if (!length && hint) {
            length = hint;
            hint = 0;
        }
        vaddr_t result = 0;
        if (!mm::address_space::mmap(*space, hint, length,
                                     static_cast<u32>(registers->rdx),
                                     static_cast<u32>(registers->r10), result)) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        registers->rax = result;
        return;
    }
    if (registers->rax == SYS_WAIT) {
        const vaddr_t status_out = registers->rsi;
        i32 status = 0;
        // Validate the user pointer before blocking: a bad buffer fails
        // immediately instead of sleeping first.
        if (!space || !status_out
            || !mm::address_space::copy_to_user(*space, status_out, &status, sizeof(status))) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        u32 child_id = 0;
        if (!scheduler::wait(static_cast<i32>(registers->rdi), status, child_id)) {
            registers->rax = static_cast<u64>(-10);
            return;
        }
        if (!mm::address_space::copy_to_user(*space, status_out, &status, sizeof(status))) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        registers->rax = child_id;
        return;
    }
    if (registers->rax == SYS_SLEEP_MS) {
        scheduler::sleep_ms(registers->rdi);
        registers->rax = 0;
        return;
    }
    if (registers->rax == SYS_SPAWN) {
        if (!space) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        auto* copy = static_cast<SpawnCopy*>(mm::heap::kmalloc(sizeof(SpawnCopy)));
        if (!copy) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        copy->used = 0;
        char path[MAX_SPAWN_PATH];
        usize path_length = 0;
        bool ok = copy_user_string(*space, registers->rdi, path, sizeof(path), path_length)
            && path_length > 0
            && copy_user_vector(*space, registers->rsi, *copy, copy->argv)
            && copy_user_vector(*space, registers->rdx, *copy, copy->envp);
        u32 pid = 0;
        if (ok) ok = process::spawn(path, copy->argv, copy->envp, pid);
        mm::heap::kfree(copy);
        registers->rax = ok ? pid : static_cast<u64>(-1);
        return;
    }
    if (registers->rax == SYS_SERVICE_REGISTER || registers->rax == SYS_SERVICE_CONNECT) {
        char name[MAX_SERVICE_NAME];
        if (!read_service_name(space, registers, name)) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        ipc::Handle handle;
        ipc::Error error;
        bool success;
        if (registers->rax == SYS_SERVICE_REGISTER) {
            nvmfs::NodeId node_id;
            nvmfs::Error fs_error;
            if (!nvmfs::create(nvmfs::NodeType::service, name, 0, node_id, fs_error)) {
                registers->rax = static_cast<u64>(-1);
                return;
            }
            success = ipc::register_service(scheduler::current_id(), name, node_id, handle, error);
            if (!success) {
                nvmfs::Error cleanup_error;
                nvmfs::erase(node_id, cleanup_error);
            }
        } else {
            success = ipc::connect(scheduler::current_id(), name, handle, error);
        }
        registers->rax = success ? handle : static_cast<u64>(-1);
        return;
    }
    if (registers->rax == SYS_SERVICE_ACCEPT) {
        ipc::Handle handle;
        ipc::Error error;
        registers->rax = ipc::accept(scheduler::current_id(), registers->rdi, handle, error)
            ? handle : static_cast<u64>(-11);
        return;
    }
    if (registers->rax == SYS_IPC_SEND) {
        const usize size = registers->rdx;
        u8 buffer[64];
        if (!space || size > sizeof(buffer)
            || !mm::address_space::copy_from_user(*space, buffer, registers->rsi, size)) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        ipc::Error error;
        registers->rax = ipc::send(scheduler::current_id(), registers->rdi,
                                   buffer, size, error)
            ? size : static_cast<u64>(-11);
        return;
    }
    if (registers->rax == SYS_IPC_RECEIVE) {
        if (!space || registers->rdx < 64) {
            registers->rax = static_cast<u64>(-90);
            return;
        }
        ipc::Message message{};
        ipc::Error error;
        if (!ipc::receive_blocking(scheduler::current_id(), registers->rdi,
                                   message, error)) {
            registers->rax = static_cast<u64>(-11);
            return;
        }
        if (!mm::address_space::copy_to_user(*space, registers->rsi,
                                             message.bytes, message.size)) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        registers->rax = message.size;
        return;
    }
    if (registers->rax == SYS_READ_CHAR) {
        const usize capacity = registers->rsi;
        char scratch = 0;
        // Validate the user pointer before blocking: a bad buffer fails
        // immediately instead of sleeping first.
        if (!space || capacity < 1 || capacity > MAX_READ_CHAR_SIZE
            || !mm::address_space::copy_to_user(*space, registers->rdi, &scratch, 1)) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        char character = 0;
        // Returns false (-11) when this task does not hold keyboard focus.
        if (!keyboard::read_char_for_task(scheduler::current_id(), character)) {
            registers->rax = static_cast<u64>(-11);
            return;
        }
        if (!mm::address_space::copy_to_user(*space, registers->rdi, &character, 1)) {
            registers->rax = static_cast<u64>(-1);
            return;
        }
        registers->rax = 1;
        return;
    }
    if (registers->rax == SYS_KBD_FOCUS) {
        // rdi != 0: take keyboard focus. rdi == 0: return it to the console.
        const u32 id = scheduler::current_id();
        const bool ok = registers->rdi != 0 ? keyboard::request_focus(id)
                                            : keyboard::release_focus(id);
        registers->rax = ok ? 0 : static_cast<u64>(-11);
        return;
    }
    registers->rax = static_cast<u64>(-38);
}

}