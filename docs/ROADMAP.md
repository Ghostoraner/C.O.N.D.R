# Kernel Roadmap

This roadmap describes the intended progression from the current single-core
kernel prototype to a usable object-oriented operating system. Items are
planned unless marked as implemented.

## Implemented foundation

- Limine boot on x86_64, descriptor tables, physical memory allocator and heap
- 8259 PIC remapping with a 100 Hz PIT tick
- PS/2 keyboard events delivered through IRQ1 into a bounded input queue
- Cooperative single-CPU round-robin tasks with independent kernel stacks
- Framebuffer console, serial logging, and volatile NVMFS object registry

## Kernel execution model

- Add ACPI discovery and local APIC support, while retaining a tested legacy
  PIC fallback for emulators and older machines
- Add timer-driven preemption after task switching is safe at interrupt boundaries
- Establish explicit interrupt-safe locking before enabling multiple CPUs
- Add page-table management, isolated address spaces, and a TSS with per-task
  kernel stacks

## User mode and services

- Add a syscall ABI and controlled transitions between Ring 3 and Ring 0
- Load and run statically linked ELF64 programs in isolated address spaces
- Add process lifecycle management, handles, and message queues
- Make NVMFS nodes discoverable services and implement capability-based access
- Move the interactive shell into a user process

## Devices and persistent data

- Enumerate PCI devices and add VirtIO block and console drivers
- Add a block-device layer and a pathname VFS without replacing NVMFS's
  object/tag model
- Persist object data through a disk-backed store with recovery checks
- Add network drivers and a basic packet/socket interface after storage and
  process isolation are stable

## Validation

- Keep `make` warning-clean for C++ and run the kernel in QEMU after each
  interrupt, memory, or scheduler change
- Add focused self-tests for allocator boundaries, timer progress, task
  switching, address-space isolation, IPC, and storage recovery
- Test BIOS and UEFI boot paths in QEMU before treating a USB image as a release

The first planned milestone after this foundation is a single-CPU scheduler;
user-mode execution depends on stable task switching, page tables, and TSS
kernel stacks.