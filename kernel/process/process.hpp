#pragma once
#include <nord/types.hpp>

namespace nord::process {

// Loads an ELF64 file node from NVMFS and starts it as a new user task.
// `path` names a flat NVMFS file (an optional leading '/' is ignored).
// `argv` and `envp` are NULL-terminated arrays of C strings; either may be
// null for an empty vector. The user stack is built per the System V AMD64 ABI.
// On success `pid` receives the new task id; on failure it is set to 0.
bool spawn(const char* path, const char* const* argv, const char* const* envp, u32& pid);

// Copies the kernel-embedded user programs into NVMFS when they are missing,
// so spawn() can load them through the normal storage path.
void install_builtins();

}
