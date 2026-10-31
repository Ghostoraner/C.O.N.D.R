#pragma once
#include <nord/types.hpp>

namespace nord::arch {

struct [[gnu::packed]] Registers {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error_code;
    u64 rip, cs, rflags, rsp, ss;
};

namespace idt {
    using Handler = void (*)(Registers*);
    void init();
    void set_handler(u8 vector, Handler fn);
}

}