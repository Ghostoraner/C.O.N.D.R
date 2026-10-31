#pragma once
#include <stdint.h>
#include <stddef.h>

namespace nord {

using u8  = uint8_t;   using i8  = int8_t;
using u16 = uint16_t;  using i16 = int16_t;
using u32 = uint32_t;  using i32 = int32_t;
using u64 = uint64_t;  using i64 = int64_t;

using usize   = size_t;
using paddr_t = u64;
using vaddr_t = u64;

constexpr usize PAGE_SIZE = 4096;

constexpr u64 align_up(u64 v, u64 a)   { return (v + a - 1) & ~(a - 1); }
constexpr u64 align_down(u64 v, u64 a) { return v & ~(a - 1); }

}