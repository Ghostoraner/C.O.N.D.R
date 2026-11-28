# User Mode

C.O.N.D.R. can enter Ring 3 and run freestanding ELF64 programs in separate
address spaces. The kernel clones Limine's PML4 root, selects an unused
lower-half PML4 slot for private user pages, and retains shared kernel mappings
as supervisor-only. The TSS supplies each task's kernel stack on privilege
change. Programs can be started from a NVMFS file with `spawn`, which builds a
System V AMD64 initial stack containing `argc`, `argv`, `envp` and an auxiliary
vector.

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
| `2` | `exit` | `RDI` exit status | task becomes a zombie, status collected by `wait` |
| `3` | `service_register` | `RDI` name, `RSI` length (1-23) | accept handle |
| `4` | `service_connect` | `RDI` name, `RSI` length | send/receive handle |
| `5` | `service_accept` | `RDI` listener handle | connected endpoint handle, or `-11` |
| `6` | `ipc_send` | `RDI` handle, `RSI` data, `RDX` length (0-64) | bytes sent, or negative error |
| `7` | `ipc_receive` | `RDI` handle, `RSI` output buffer, `RDX` capacity (at least 64) | message length, or `-11` on an invalid handle |
| `8` | `read_char` | `RDI` user buffer pointer, `RSI` capacity (1-256) | `1` byte written, `-1` invalid pointer/capacity, `-11` caller lacks keyboard focus |
| `9` | `kbd_focus` | `RDI != 0` takes focus, `RDI == 0` returns it to the console | `0`, or `-11` if the request was rejected |
| `10` | `wait` | `RDI` child pid (`<= 0` waits for any child), `RSI` user `i32` status output | child pid, `-1` invalid status pointer, `-10` no matching child |
| `11` | `sleep_ms` | `RDI` milliseconds | `0` after the timer wakeup |
| `12` | `spawn` | `RDI` path, `RSI` argv, `RDX` envp (NULL-terminated user arrays) | child pid, or `-1` |
| `13` | `brk` | `RDI` new program break (`0` queries it) | new break, or `-1` |
| `14` | `mmap` | `RDI` address hint, `RSI` length, `RDX` `PROT_*`, `R10` flags | mapping address, or `-1` |

## Spawning programs

`spawn` loads an ELF64 image from a NVMFS file and starts it as a child of the
caller. NVMFS is a flat namespace, so `path` is a node name; a leading `/` is
ignored. `argv` and `envp` are NULL-terminated arrays of user pointers; either
may be null for an empty vector. The kernel validates and copies every string,
maps the ELF segments into a fresh address space, and builds the System V AMD64
initial stack:

```
%rsp -> argc
        argv[0] ... argv[argc-1], NULL
        envp[0] ... envp[envc-1], NULL
        auxv pairs (AT_PAGESZ, AT_ENTRY, AT_EXECFN, ..., AT_NULL)
        argument and environment strings
```

`%rsp` is 16-byte aligned on entry. The returned pid can be collected with
`wait`. The kernel embeds a small `args` program and installs it into NVMFS at
boot; `spawn args alpha beta` prints the argument and environment strings and
exits with `argc` as its status. The console command `spawn PATH [ARGS...]`
waits for the child and prints its exit status.

## User memory

Each address space owns a private PML4 slot. The ELF image and the initial
stack occupy the bottom of the slot; above them the kernel reserves a `brk`
heap followed by an anonymous `mmap` region. Both regions are demand-paged:
the kernel records the range and permissions, and the first access to a page
raises `#PF`, which allocates a zero-filled page and retries the instruction.

`brk` moves the program break within `[brk_start, brk_limit]`. `brk(0)` returns
the current break. Growing the break only extends the range; shrinking it
unmaps and frees the pages above the new break. `mmap` allocates anonymous
pages only: `PROT_WRITE` (`2`) and `PROT_EXEC` (`4`) are honored, the address
hint and flags are accepted for ABI compatibility, and `fd`/`offset` are not
used. `mmap(length)` with the length in `RDI` and a zero `RSI` is also accepted.
Both calls return `-1` when the request lies outside the process slot.

`copy_from_user` and `copy_to_user` validate every page before touching it,
resolve demand-zero pages through the same fault path, enforce write access for
`copy_to_user`, and reject kernel or unmapped addresses. A bad user pointer
therefore returns an error instead of faulting the kernel. A real Ring 3 fault
outside a recorded anonymous range terminates the task; kernel-mode faults
still panic.

## Process lifecycle

A task moves through `ready`, `running`, `blocked` and `zombie` states. `exit`
records the status and leaves a zombie record so the parent can collect it.
`wait` blocks until a matching child becomes a zombie, copies its exit status to
the user buffer, then reclaims the child's PML4, user pages, kernel stack and
task record. Tasks whose parent is gone are reclaimed automatically at the next
scheduling round. `ps` shows `parent=` and, for zombies, `status=`.

Blocking syscalls park the caller instead of spinning. `ipc_receive` registers
the task on the endpoint queue and is woken by a matching `ipc_send`;
`read_char` is woken by IRQ1; and `sleep_ms` is woken when the PIT advances past
its deadline. The scheduler keeps its dynamic task index separate from a
configurable slab cache for task records (`scheduler::configure` before
`scheduler::init`), so task creation/exit cycles reuse slab objects.

The kernel validates user ranges by walking the current address space before
copying data. Service names are indexed in NVMFS, while IPC handles are bound to
the owning task and revoked when it exits. Each `service_connect` creates its
own client endpoint, and each `service_accept` creates a matching server
endpoint, so every connection has a private pair of bounded eight-message
queues. Multiple clients can therefore talk to one service without sharing a
channel. The demo connects to its own `echo` service to exercise the
request/reply path. There are no file or pathname syscalls; `spawn` accepts a
flat NVMFS node name, and user memory is managed through `brk`/`mmap` only.
This code is not a security boundary suitable for untrusted software.

## Keyboard input

`read_char` (syscall 8) blocks the caller until a typed character is available,
then writes exactly one byte to the user buffer and returns `1`. Arrow keys,
Home/End and Delete events are discarded; only `Key::character` events are
returned. A task must hold keyboard focus (syscall 9) before calling
`read_char`; otherwise it fails immediately with `-11`. Taking focus flushes
keys typed earlier for the console, and releasing focus wakes the console.

Several tasks may wait at once: each reader registers a waiter node on its own
kernel stack before blocking, so the list has no fixed capacity and a keypress
wakes every waiter losslessly, even if the interrupt arrives between the queue
check and the sleep. Only the task that owns focus consumes events, and each
event is delivered to exactly one reader. IPC receive waiters use the same
stack-linked approach per endpoint, so connection queues are not limited to a
fixed number of blocked receivers. Events dropped because the ring buffer
overflowed are counted and shown by the `sys` shell command.