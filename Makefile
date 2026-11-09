CXX     := $(shell command -v x86_64-elf-g++ 2>/dev/null || command -v g++)
LD      := $(shell command -v x86_64-elf-ld 2>/dev/null || command -v ld)
AS      := nasm
ISO_TOOL := $(shell command -v xorriso 2>/dev/null || command -v genisoimage 2>/dev/null)

ifeq ($(notdir $(ISO_TOOL)),xorriso)
ISO_ARGS := -as mkisofs -R -r -J \
	-b boot/limine/limine-bios-cd.bin \
	-no-emul-boot -boot-load-size 4 -boot-info-table \
	-hfsplus -apm-block-size 2048 \
	--efi-boot boot/limine/limine-uefi-cd.bin \
	-efi-boot-part --efi-boot-image --protective-msdos-label
else
ISO_ARGS := -R -r -J \
	-b boot/limine/limine-bios-cd.bin \
	-no-emul-boot -boot-load-size 4 -boot-info-table \
	-eltorito-alt-boot -eltorito-platform 0xEF \
	-b boot/limine/limine-uefi-cd.bin -no-emul-boot
endif

BUILD   := build
KERNEL  := $(BUILD)/condr.elf
ISO     := $(BUILD)/condr.iso
USB_ISO := $(BUILD)/condr-usb.iso
LIMINE_DIR := limine
LIMINE_BIN := $(LIMINE_DIR)/limine

CXXFLAGS := \
	-std=c++23 -O2 -g \
	-ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
	-fno-stack-check -fno-PIC -fno-pie -fno-lto \
	-mno-red-zone -mcmodel=kernel \
	-mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-Wall -Wextra -Wno-unused-parameter \
	-Ikernel -Ikernel/include

ASFLAGS  := -f elf64 -g -F dwarf

LDFLAGS  := \
	-nostdlib -static -z max-page-size=0x1000 \
	-T kernel/linker.ld

# Flags for user-mode ELF images. noexecstack marks the GNU stack
# non-executable, so no objcopy post-processing is needed.
USER_LDFLAGS := -nostdlib -static -z noexecstack -T user/linker.ld

CXX_SRC := $(shell find kernel -name '*.cpp')
ASM_SRC := $(shell find kernel -name '*.asm')
FONT_SRC := kernel/drivers/video/font12x22.bin
USER_ELF := $(BUILD)/user/demo.elf
USER_OBJ := $(BUILD)/user/demo.elf.o
KBD_ELF := $(BUILD)/user/kbd_demo.elf
KBD_OBJ := $(BUILD)/user/kbd_demo.elf.o
OBJ     := $(CXX_SRC:%.cpp=$(BUILD)/%.o) $(ASM_SRC:%.asm=$(BUILD)/%.o) $(BUILD)/$(FONT_SRC).o $(USER_OBJ) $(KBD_OBJ)
DEPS    := $(CXX_SRC:%.cpp=$(BUILD)/%.d)

.PHONY: all kernel iso usb-iso run debug test clean distclean limine

all: kernel

kernel: $(KERNEL)

$(KERNEL): $(OBJ) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo "  LD    $@"

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@
	@echo "  CXX   $<"

# Also builds user/*.asm into build/user/*.o
$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@
	@echo "  AS    $<"

$(BUILD)/%.bin.o: %.bin
	@mkdir -p $(dir $@)
	@$(LD) -r -b binary -o $@ $<
	@echo "  BIN   $<"

$(USER_ELF): $(BUILD)/user/demo.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $<

$(USER_OBJ): $(USER_ELF)
	@mkdir -p $(dir $@)
	$(LD) -r -b binary -o $@ $<

$(KBD_ELF): $(BUILD)/user/kbd_demo.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $<

$(KBD_OBJ): $(KBD_ELF)
	@mkdir -p $(dir $@)
	$(LD) -r -b binary -o $@ $<

-include $(DEPS)

$(LIMINE_BIN):
	git clone https://github.com/limine-bootloader/limine.git \
		--branch=v8.x-binary --depth=1 $(LIMINE_DIR)
	$(MAKE) -C $(LIMINE_DIR)

limine: $(LIMINE_BIN)

iso: $(KERNEL) $(LIMINE_BIN)
	@rm -rf $(BUILD)/iso_root
	@mkdir -p $(BUILD)/iso_root/boot/limine
	@mkdir -p $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/condr.elf
	cp boot/limine.conf $(BUILD)/iso_root/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
		$(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
	$(ISO_TOOL) $(ISO_ARGS) -o $(ISO) $(BUILD)/iso_root
	$(LIMINE_BIN) bios-install $(ISO)
	@echo "  ISO   $(ISO)"

usb-iso: $(KERNEL) $(LIMINE_BIN)
	@if ! command -v xorriso >/dev/null 2>&1; then \
		echo "error: xorriso is required to build a hybrid BIOS/UEFI USB image" >&2; \
		exit 1; \
	fi
	@$(MAKE) iso ISO_TOOL="$$(command -v xorriso)"
	cp $(ISO) $(USB_ISO)
	@echo "  USB ISO   $(USB_ISO)"

run: iso
	./scripts/run-qemu.sh $(ISO)

debug: iso
	./scripts/run-qemu.sh $(ISO) -s -S

test: iso
	python3 scripts/test-qemu.py $(ISO)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(LIMINE_DIR)