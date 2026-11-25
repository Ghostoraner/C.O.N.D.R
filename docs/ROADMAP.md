# C.O.N.D.R Kernel Roadmap

This roadmap tracks the progression of **C.O.N.D.R** from a single-core kernel prototype to a self-hosted CLI operating system. Items are planned unless explicitly marked as implemented.

---

## Implemented Foundation
* **Boot & Core Architecture:** Limine boot on `x86_64`, descriptor tables (GDT/IDT/TSS), RSP0 switching, Ring 3 entry, user exception containment, and IST for double-fault handling.
* **Memory Management:** Bitmap PMM, kernel heap, per-process PML4 page tables with private user mappings and shared supervisor-only kernel mappings, NX page protections, task-stack guard pages, and symbol-resolved panic dumps.
* **Scheduling & IPC:** Timer-preemptive single-CPU round-robin scheduler (100 Hz PIT tick, 8259 PIC), ready/running/blocked/zombie lifecycle with `exit(status)`/`wait` and full address-space/stack reclamation, task blocking/wakeup, deferred stack reclamation, FPU/SSE state saving, preemption-safe spinlocks, and capability-based IPC with bounded endpoint pairs.
* **Execution & Storage:** Embedded ELF64 loader, `int 0x80` syscalls, PCI enumeration, legacy VirtIO-blk I/O, and two-slot CRC NVMFS snapshots.
* **I/O & Tooling:** Framebuffer console, COM1 serial logging, PS/2 keyboard input queue (IRQ1), kernel-mode shell, dynamically sized task/NVMFS tables, and QEMU integration/boundary tests (PMM/heap exhaustion, IPC saturation, snapshot corruption).

---

## Current Gap to CLI-OS
The kernel handles preemption, process isolation, IPC, and double faults, but cannot yet load and run external user programs from disk.

| Layer | Current State | Missing for CLI-OS |
| :--- | :--- | :--- |
| **Scheduling** | Preemptive round-robin, zombie/wait lifecycle, dynamic task table, spinlocks, FPU/SSE, IST | Priorities, `sleep`, signals, SMP |
| **Memory** | Bitmap PMM, heap, per-process PML4, NX, guard pages | `mmap` / `brk` for user space, copy-on-write |
| **Programs** | Embedded ELF64 loader, `int 0x80` syscalls, `exit`/`wait` | External ELF loading from disk, `spawn`, `argv`/`envp`, `libc` |
| **Files** | Flat NVMFS object/tag registry, 2-slot CRC snapshots | Path hierarchy, VFS, file descriptors, permissions, large files |
| **I/O** | PS/2 keyboard, framebuffer console, serial port | TTY with line editing, pipes, redirection, scrollback, USB keyboard |
| **Shell** | Kernel-mode shell with built-in commands | User-space shell (`/bin/sh`), core utilities (`ls`, `cat`, `cp`, editor), scripting |
| **Devices** | Legacy VirtIO-blk, PCI, PIC, PIT | AHCI/NVMe, APIC, ACPI, USB, networking |

---

## Development Phases & Gates

Minimum viable **CLI-OS** is complete at **Gate 5**. Phases 6 and 7 are post-CLI-OS extensions.

### Phase 1: Kernel Stabilization
*Goal: Make the core kernel trustworthy before building user-space abstractions.*
- [x] Fix `fpu::reset`: capture clean state template via `fninit` + `fxsave64` (x87 tag word `0x00`, not `0xFF`) and copy into new tasks.
- [x] FPU context test: two tasks with distinct XMM register values yielding to each other.
- [x] IST test: deliberate kernel stack overflow triggers double-fault handler instead of triple-fault reboot.
- [x] NX bit on data/stack pages and guard pages beneath task stacks.
- [x] Symbol-resolved panic dumps (registers, call stack, and function names via ELF symbol table).
- [x] Remove hardcoded `MAX_TASKS = 8` and 128-node NVMFS limits.
- [x] Boundary tests for PMM/heap exhaustion, IPC queue saturation, and corrupted disk snapshots.

**Gate 1:** `make test` passes in CI; deliberate kernel stack overflow and user-mode write to kernel memory are caught cleanly without rebooting the machine.

---s

### Phase 2: Processes & System Calls
*Goal: Transition from embedded kernel demos to a full process lifecycle.*
- [x] Process lifecycle: `ready`, `running`, `blocked`, `zombie` states; `exit(status)` and `wait`/`waitpid`; full PML4, page, and kernel stack reclamation.
- [x] Configurable dynamic task table (list or slab allocator).
- [x] Blocking calls: `ipc_receive`, `read_char`, and `sleep_ms` transition tasks to `blocked` and wake via IRQ/IPC rather than busy-waiting.
- [ ] `spawn(path, argv, envp)`: load ELF from storage and initialize user stack per System V ABI.
- [ ] User memory: `brk`/`mmap` (anonymous pages) and safe pointer validation (`copy_from_user`/`copy_to_user`) with page-fault handling.
- [ ] Per-process file descriptor table: `open`, `close`, `read`, `write`, `dup` (backing `stdin`/`stdout`/`stderr`).
- [ ] Migrate from `int 0x80` to `syscall`/`sysret` (`MSR_STAR`/`MSR_LSTAR`), keeping `int 0x80` temporarily during migration.
- [ ] Minimal signals: `kill` and `Ctrl+C` termination for foreground processes.
- [ ] Versioned syscall specification in `docs/USER_MODE.md` and shared `syscall.h` header.

**Gate 2:** A user program loaded from disk executes with arguments, reads `stdin`, writes `stdout`, exits with a status code collected by `wait`, and survives 100 `spawn`/`exit` cycles with zero memory leaks.

---

### Phase 3: Storage & Filesystem
*Goal: Hierarchical paths, directories, and crash-safe disk persistence.*
- [ ] Unified block-device layer (`read_blocks`/`write_blocks`), page cache, and interrupt-driven VirtIO-blk I/O.
- [ ] Partition table support (MBR/GPT) and disk image expansion from 4 MiB to 64–256 MiB.
- [ ] Pathname VFS: inodes, directories, path resolution (`/a/b/c`), mount table, root FS, and `/dev`.
- [ ] Persistent writable disk FS (object/tag FS preserving NVMFS semantics or FAT32) with crash consistency (journaling or copy-on-write).
- [ ] Mount-time `fsck` verification and randomized power-loss testing (`QEMU quit` mid-write).
- [ ] Filesystem syscalls: `open`, `read`, `write`, `close`, `lseek`, `stat`, `readdir`, `mkdir`, `unlink`, `rename`.
- [ ] Move user binaries out of kernel image into `/bin` during disk image build.
- [ ] Wall-clock and monotonic time via CMOS RTC and PIT.

**Gate 3:** A file created at `/home/test.txt` persists across reboots; programs execute from `/bin`; hundreds of random QEMU power cuts cause zero filesystem corruption.

---

### Phase 4: Terminal & User-Space Shell
*Goal: Move the interactive shell out of kernel space into `/bin/sh`.*
- [ ] TTY subsystem: line discipline (line editing, Backspace, `Ctrl+C`/`Ctrl+D`, `ECHO`) with `canonical` and `raw` modes.
- [ ] Extended keyboard driver: modifiers (`Shift`/`Caps`/`Ctrl`/`Alt`), navigation keys, key repeat, and switchable US/UA layouts.
- [ ] Console improvements: scrollback buffer (`PageUp`/`PageDown`), hardware/software cursor, basic ANSI escape sequences, and dirty-buffer rendering.
- [ ] Mirror `kprintf` kernel diagnostics to `COM1` for CI and `scripts/test-qemu.py` log capture.
- [ ] User-mode `/bin/sh`: line editor, command history, quote/escape parsing, built-ins (`cd`, `pwd`, `exit`, `export`, `help`), `PATH` lookup, and `$HOME`/`$PWD`/`$?` expansion.
- [ ] Kernel pipes (`|`) and I/O redirection (`>`, `>>`, `<`).
- [ ] `init` process (PID 1): executes `/etc/rc`, launches `/bin/sh`, and respawns it on exit.

**Gate 4:** Clean boot drops directly into a user-mode shell prompt; `echo hi | cat > /tmp/a` followed by `cat /tmp/a` succeeds; `Ctrl+C` terminates a foreground child without crashing the shell.

---

### Phase 5: Userland & Core Utilities *(CLI-OS Complete)*
*Goal: Deliver a self-contained, usable command-line environment.*
- [ ] Dedicated `userland/` build tree (`x86_64-elf-gcc` or `clang --target`) with automated disk image packaging.
- [ ] Minimal `libc` (ported `musl` or `newlib`): `crt0`, `printf`, `malloc`/`free` over `brk`/`mmap`, `string.h`, `stdio`, `errno`, `exit`.
- [ ] Core utilities: `ls`, `cat`, `echo`, `mkdir`, `rmdir`, `rm`, `cp`, `mv`, `touch`, `pwd`, `clear`, `head`, `tail`, `wc`, `grep`.
- [ ] System utilities: `ps`, `kill`, `free`, `uptime`, `date`, `dmesg`, `reboot`, `shutdown`.
- [ ] Terminal text editor (`nano`-level: open, edit, save).
- [ ] Shell scripting support: variables, `if`, `for`, and `#!` shebang execution.
- [ ] Minimal package archive utility (`tar`-style extraction into `/bin`) and native `tag` CLI utility for NVMFS metadata queries.

**Gate 5:** Boot into QEMU, write and save a script with a loop in the text editor, reboot, reopen, and execute it; all utilities covered by automated tests in `test-qemu.py`.

---

## Post-CLI-OS Extensions

### Phase 6: Hardware Enablement & Bare Metal
- [ ] ACPI table parsing (`RSDP`/`XSDT`/`MADT`/`FADT`) for clean shutdown and reboot.
- [ ] Local APIC and I/O APIC replacing legacy 8259 PIC; APIC timer / HPET calibrated against PIT.
- [ ] Full PCI bus enumeration, MSI/MSI-X interrupts, and MMIO BAR mapping.
- [ ] Modern VirtIO 1.0 (non-legacy) block driver, followed by AHCI or NVMe storage drivers.
- [ ] USB xHCI controller, HID keyboard, and USB mass-storage support.
- [ ] UEFI boot via Limine in VirtualBox and on real target hardware.
- [ ] SMP bring-up via Limine MP, per-CPU structures, IPIs, and atomic IRQ-safe spinlocks.

**Gate 6:** Boots and runs reliably in QEMU/OVMF with APIC, VirtualBox, and on real bare-metal hardware with USB keyboard and storage.

---

### Phase 7: Networking
- [ ] `virtio-net` driver (RX/TX virtqueues and buffer pools).
- [ ] Network stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP) and BSD-style socket API (`socket`, `connect`, `send`, `recv`, `bind`, `listen`, `accept`), optionally isolated as user-mode capability-IPC services.
- [ ] DHCP client, DNS resolver, and diagnostic tools (`ping`, `ifconfig`, `nslookup`).
- [ ] Basic HTTP client (`wget`/`curl`-style) and multi-user permission model (`uid`/`gid`, file modes) prior to exposing network services.

**Gate 7:** `ping` and `nslookup` work under QEMU `user-net`; `wget` fetches a page from a host HTTP server and saves it to disk.

---

## Engineering & Testing Discipline
* **Incremental Commits:** One checklist item per commit; every commit must remain warning-clean (`make`), pass `make test` in CI, and include a regression test.
* **Strict Gate Enforcement:** Do not start a new phase until the current phase's gate report is verified and committed under `docs/gates/`.
* **Debugging Readiness:** Maintain GDB remote debugging (`-s -S`) scripts before starting user-space process execution in Phase 2, and back up disk images prior to filesystem changes in Phase 3.