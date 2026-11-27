#pragma once
#include <mm/address_space.hpp>

namespace nord::mm::elf_loader {

// Loads a statically linked ELF64 image from memory into `space`, maps its
// PT_LOAD segments and builds a System V AMD64 initial process stack.
// `argv` and `envp` are arrays of `argc`/`envc` NUL-terminated strings. The
// initial `%rsp` points at argc followed by the argv/envp pointer arrays, their
// NULL terminators, and an auxiliary vector terminated by AT_NULL.
bool load(address_space::AddressSpace& space, const u8* image, usize image_size,
          const char* const* argv, usize argc,
          const char* const* envp, usize envc,
          const char* path, u64& entry, u64& stack_pointer);

bool load_demo(address_space::AddressSpace& space, u64& entry, u64& stack_pointer);
bool load_kbd_demo(address_space::AddressSpace& space, u64& entry, u64& stack_pointer);

// Built-in user program embedded into the kernel image. `install_builtins`
// copies it into NVMFS so it can be spawned through the normal storage path.
const u8* args_image(usize& size);

}
