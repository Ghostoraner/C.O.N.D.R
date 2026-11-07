# User Mode

C.O.N.D.R. can enter Ring 3 and run one freestanding ELF64 demo in a separate
address space. The kernel clones Limine's PML4 root, selects an unused lower-half
PML4 slot for private user pages, and retains shared kernel mappings as
supervisor-only. The TSS supplies each task's kernel stack on privilege change.

## Demo program

The demo is assembled from `user/demo.asm`, linked at `0x8000000000`, and
embedded into the kernel image by the Makefile. The ELF loader validates the
x86_64 ELF header and `PT_LOAD` ranges, maps pages into an available user slot,
and applies a load bias when the preferred PML4 slot is occupied.

After boot, the demo prints `C.O.N.D.R user-mode ELF started` through the
syscall interface and exits. User CPU exceptions terminate that task without
panicking the kernel.

## Syscall ABI

User programs invoke `int 0x80` with the syscall number in `RAX`:

| Number | Operation | Arguments | Result |
|---|---|---|---|
| `1` | `write` | `RDI` user buffer, `RSI` byte count (maximum 256) | bytes written, or `-1` |
| `2` | `exit` | `RDI` exit status (currently ignored) | task is terminated |
| `3` | `service_register` | `RDI` name, `RSI` length (1-23) | accept handle |
| `4` | `service_connect` | `RDI` name, `RSI` length | send/receive handle |
| `5` | `service_accept` | `RDI` listener handle | connected endpoint handle, or `-11` |
| `6` | `ipc_send` | `RDI` handle, `RSI` data, `RDX` length (0-64) | bytes sent, or negative error |
| `7` | `ipc_receive` | `RDI` handle, `RSI` output buffer, `RDX` capacity (at least 64) | message length, or `-11` |

The kernel validates user ranges by walking the current address space before
copying data. Service names are indexed in NVMFS, while IPC handles are bound to
the owning task and revoked when it exits. Endpoints have bounded queues of
eight messages. The demo connects to its own `echo` service to exercise the
request/reply path. There are no file or memory-management syscalls. Page-table
mappings currently do not enforce NX, and this code is not a security boundary
suitable for untrusted software.