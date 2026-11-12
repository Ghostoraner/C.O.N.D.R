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
| `8` | `read_char` | `RDI` user buffer pointer, `RSI` capacity (1-256) | `1` byte written, `-1` invalid pointer/capacity, `-11` caller lacks keyboard focus |
| `9` | `kbd_focus` | `RDI != 0` takes focus, `RDI == 0` returns it to the console | `0`, or `-11` if the request was rejected |

The kernel validates user ranges by walking the current address space before
copying data. Service names are indexed in NVMFS, while IPC handles are bound to
the owning task and revoked when it exits. Each `service_connect` creates its
own client endpoint, and each `service_accept` creates a matching server
endpoint, so every connection has a private pair of bounded eight-message
queues. Multiple clients can therefore talk to one service without sharing a
channel. The demo connects to its own `echo` service to exercise the
request/reply path. There are no file or memory-management syscalls. Page-table
mappings currently do not enforce NX, and this code is not a security boundary
suitable for untrusted software.

## Keyboard input

`read_char` (syscall 8) blocks the caller until a typed character is available,
then writes exactly one byte to the user buffer and returns `1`. Arrow keys,
Home/End and Delete events are discarded; only `Key::character` events are
returned. A task must hold keyboard focus (syscall 9) before calling
`read_char`; otherwise it fails immediately with `-11`. Taking focus flushes
keys typed earlier for the console, and releasing focus wakes the console.

Several tasks may wait at once: each reader registers itself in the waiter list
before blocking, so a keypress wakes the right task and a wakeup can never be
lost even if the interrupt arrives between the queue check and the sleep. Only
the task that owns focus consumes events, and each event is delivered to exactly
one reader. Events dropped because the ring buffer overflowed are counted and
shown by the `sys` shell command.