#pragma once
#include <mm/address_space.hpp>

namespace nord::mm::elf_loader {

bool load_demo(address_space::AddressSpace& space, u64& entry, u64& stack_pointer);

}