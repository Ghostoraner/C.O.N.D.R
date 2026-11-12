# Kernel Roadmap

This roadmap describes the intended progression from the current single-core
kernel prototype to a usable object-oriented operating system. Items are
planned unless marked as implemented.

## Implemented foundation

- Limine boot on x86_64, descriptor tables, physical memory allocator and heap
- 8259 PIC remapping with a 100 Hz PIT tick
- PS/2 keyboard events delivered through IRQ1 into a bounded input queue
- Timer-preemptive single-CPU round-robin tasks with independent kernel stacks
- Task blocking/wakeup and deferred reclamation of exited task stacks
- Per-process PML4 roots with private user mappings and shared supervisor-only kernel mappings
- TSS `RSP0` switching, Ring 3 entry, and user exception containment
- Embedded ELF64 loader and `int 0x80` write/exit syscalls
- Named NVMFS services with task-owned capability handles and a private bounded endpoint pair per connection
- Preemption-safe spinlocks around PMM, NVMFS, and IPC state
- PCI enumeration, legacy VirtIO block sector I/O, and two-slot CRC NVMFS snapshots
- QEMU integration test for short IDs, IPC, and persistence across reboot
- Framebuffer console, serial logging, and NVMFS object registry

## Kernel execution model

- Add ACPI discovery and local APIC support, while retaining a tested legacy
  PIC fallback for emulators and older machines
- Make the single-CPU spinlocks SMP-ready (IRQ-safe acquisition and per-CPU state) before enabling multiple CPUs
- Add NX page protections, guard pages, and stronger user-copy validation

## User mode and services

- Move the interactive shell into a user process
- Expand IPC into blocking receive, process lifecycle APIs, and capability policy tests

## Devices and persistent data

- Add VirtIO console and USB mass-storage support for physical USB data persistence
- Add a block-device layer and a pathname VFS without replacing NVMFS's
  object/tag model
- Add network drivers and a basic packet/socket interface after storage and
  process isolation are stable

## Validation

- Keep `make` warning-clean for C++ and run the kernel in QEMU after each
  interrupt, memory, or scheduler change
- Expand automated tests for allocator boundaries, user-fault isolation,
  capability revocation, snapshot corruption recovery, and queue saturation
- Test BIOS and UEFI boot paths in QEMU before treating a USB image as a release

The next milestones are blocking IPC, an ELF loader for external programs,
SMP-safe synchronization, and NX/guard-page support before adding SMP.
The current ELF demo is embedded in the kernel image; IPC queues and snapshots
have fixed capacities, and persistent storage currently requires VirtIO.