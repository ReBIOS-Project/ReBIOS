# SPDX-License-Identifier: GPL-2.0-only
#
# This file is part of ReBIOS.
# Copyright (C) 2026 Mintsuki and contributors
#
# ReBIOS is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License version 2 as published by the Free
# Software Foundation.
#
# ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along
# with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

# Nuke built-in rules.
.SUFFIXES:

# Delete the target of a failed recipe.
.DELETE_ON_ERROR:

# This is the name that our final executable will have.
# Change as needed.
override OUTPUT := rebios

# Install prefix; /usr/local is a good, standard default pick.
PREFIX := /usr/local

# Default user QEMU flags. These are appended to the QEMU command calls.
# ReBIOS needs the host's VMX or SVM, which nested KVM passes on, and an IOMMU,
# here without the interrupt remapping KVM's in-kernel irqchip cannot take.
QEMUFLAGS := -m 2G -accel kvm -cpu host -device intel-iommu,intremap=off

# Internal QEMU flags that should not be changed by the user.
override QEMU_MACHINE_FLAGS := \
    -M q35
override QEMU_UEFI_FLAGS := \
    -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on

# User controllable toolchain and toolchain prefix.
TOOLCHAIN :=
TOOLCHAIN_PREFIX :=
ifneq ($(TOOLCHAIN),)
    ifeq ($(TOOLCHAIN_PREFIX),)
        TOOLCHAIN_PREFIX := $(TOOLCHAIN)-
    endif
endif

# User controllable C compiler command.
ifneq ($(TOOLCHAIN_PREFIX),)
    CC := $(TOOLCHAIN_PREFIX)gcc
else
    CC := cc
endif

# User controllable objcopy command.
OBJCOPY := $(TOOLCHAIN_PREFIX)objcopy

# Defaults overrides for variables if using "llvm" as toolchain.
ifeq ($(TOOLCHAIN),llvm)
    CC := clang
endif

# User controllable toolchain prefix for SeaBIOS, which only builds with GCC
# and GNU binutils, so the "llvm" toolchain falls back to the host's.
ifeq ($(TOOLCHAIN),llvm)
    SEABIOS_TOOLCHAIN_PREFIX :=
else
    SEABIOS_TOOLCHAIN_PREFIX := $(TOOLCHAIN_PREFIX)
endif

# User controllable commands for building SeaBIOS.
SEABIOS_CC := $(SEABIOS_TOOLCHAIN_PREFIX)gcc
SEABIOS_LD := $(SEABIOS_TOOLCHAIN_PREFIX)ld
SEABIOS_OBJCOPY := $(SEABIOS_TOOLCHAIN_PREFIX)objcopy
SEABIOS_OBJDUMP := $(SEABIOS_TOOLCHAIN_PREFIX)objdump
SEABIOS_STRIP := $(SEABIOS_TOOLCHAIN_PREFIX)strip

# User controllable toolchain prefix for the video BIOS, which only builds with
# GCC and GNU binutils too, so the "llvm" toolchain falls back to the host's.
ifeq ($(TOOLCHAIN),llvm)
    VGABIOS_TOOLCHAIN_PREFIX :=
else
    VGABIOS_TOOLCHAIN_PREFIX := $(TOOLCHAIN_PREFIX)
endif

# User controllable commands for building the video BIOS.
VGABIOS_CC := $(VGABIOS_TOOLCHAIN_PREFIX)gcc
VGABIOS_OBJCOPY := $(VGABIOS_TOOLCHAIN_PREFIX)objcopy

# User controllable flags for the video BIOS, 16-bit code that is not built
# with CFLAGS, CPPFLAGS, and LDFLAGS.
VGABIOS_CFLAGS := -g -O2 -pipe
VGABIOS_CPPFLAGS :=
VGABIOS_LDFLAGS :=

# User controllable host C compiler, which builds SeaBIOS's tools.
HOST_CC := cc

# User controllable C flags.
CFLAGS := -g -O2 -pipe

# User controllable C preprocessor flags. We set none by default.
CPPFLAGS :=

# User controllable nasm flags.
NASMFLAGS := -g

# User controllable linker flags. We set none by default.
LDFLAGS :=

# Ensure the dependencies have been obtained.
ifneq ($(filter-out clean distclean dist,$(or $(MAKECMDGOALS),default)),)
    ifeq ($(wildcard .deps-obtained),)
        $(error Please run the ./get-deps script first)
    endif
endif

# Stop on a problem the checks below find, unless only cleaning, which needs
# no toolchain.
ifneq ($(filter-out clean distclean dist,$(or $(MAKECMDGOALS),default)),)
    override TOOLCHAIN_ERROR = $(error $(1))
else
    override TOOLCHAIN_ERROR =
endif

# Check if CC is Clang.
override CC_IS_CLANG := $(shell ! $(CC) --version 2>/dev/null | grep -q '^Target: '; echo $$?)

# Macros to check if a flag is supported, used as $(call MACRO,flag), and
# expanding to 1 or 0. A second argument names another spelling.

# The name of a flag, up to any "=", with the dashes after the first made
# optional, as compilers leave the value out or spell the flag differently.
override FLAG_NAME = -$(subst -,-?,$(patsubst -%,%,$(firstword $(subst =, ,$(1)))))

# Look for a diagnostic naming the flag, in the untranslated C locale. The
# name has to stand on its own, as "-pie" would otherwise be found inside
# a complaint about "-no-pie".
override FLAG_IS_NAMED = grep -qE '(error|warning|note):.*[^[:alnum:]]($(call FLAG_NAME,$(1))$(if $(2),|$(call FLAG_NAME,$(2))))([^-[:alnum:]]|$$)'

# Check if the compiler supports a flag when compiling.
override CC_HAS_COMPILE_FLAG = $(shell LC_ALL=C $(CC) $(CFLAGS) $(1) -c -x c /dev/null -o /dev/null 2>&1 >/dev/null | $(call FLAG_IS_NAMED,$(1),$(2)); echo $$?)

# Check if the compiler supports a flag when linking, linking nothing.
override CC_HAS_LINK_FLAG = $(shell LC_ALL=C $(CC) $(CFLAGS) $(LDFLAGS) $(1) -Wl,--version 2>&1 >/dev/null | $(call FLAG_IS_NAMED,$(1),$(2)); echo $$?)

# Check if the linker supports a flag by searching its help, as older GNU
# ld take unknown flags for different ones (-no-pie for -n -o -pie).
override LD_HAS_FLAG = $(shell ! LC_ALL=C $(CC) $(CFLAGS) $(LDFLAGS) -Wl,--help 2>/dev/null | grep -qE '(^|[[:space:]])-?$(1)([[:space:],=[]|$$)'; echo $$?)

# Internal C flags that should not be changed by the user.
override CFLAGS += \
    -Wall \
    -Wextra \
    -std=gnu11 \
    -ffreestanding \
    -fno-common \
    -fno-stack-protector \
    -fno-stack-check \
    -fshort-wchar \
    -fPIE \
    -ffunction-sections \
    -fdata-sections \
    -m64 \
    -march=x86-64 \
    -mno-mmx \
    -mno-sse \
    -mno-red-zone

# Internal C preprocessor flags that should not be changed by the user.
override CPPFLAGS := \
    -nostdinc \
    -I src \
    -I picoefi/inc \
    -I uACPI/include \
    -isystem freestanding-c-hdrs/include \
    $(CPPFLAGS) \
    -MMD \
    -MP

# Internal nasm flags that should not be changed by the user.
override NASMFLAGS := \
    -f elf64 \
    $(patsubst -g,-g -F dwarf,$(NASMFLAGS)) \
    -i src/ \
    -Wall

# If the C compiler is Clang, set the target as needed.
ifeq ($(CC_IS_CLANG),1)
    override CC += \
        -target x86_64-unknown-none-elf
    # Use LLD unless LDFLAGS picks another linker.
    override LDFLAGS := \
        -fuse-ld=lld \
        $(LDFLAGS)
endif

# Internal linker flags that should not be changed by the user.
override LDFLAGS += \
    -nostdlib \
    -Wl,-pie \
    -Wl,-z,text \
    -Wl,-z,max-page-size=0x1000 \
    -Wl,-z,noexecstack \
    -Wl,--gc-sections \
    -Wl,--build-id=none \
    -Wl,--hash-style=gnu \
    -Wl,-T,picoefi/x86_64/link_script.lds \
    -Wl,-m,elf_x86_64

# Check that the compiler works with the given flags, as the checks below
# would otherwise blame a missing feature.
override CC_COMPILE_ERROR := $(shell LC_ALL=C $(CC) $(CFLAGS) -c -x c /dev/null -o /dev/null 2>&1 >/dev/null | grep -i 'error:' | head -n 1)
ifneq ($(CC_COMPILE_ERROR),)
    $(call TOOLCHAIN_ERROR,The compiler does not work with the given CFLAGS: $(CC_COMPILE_ERROR))
endif

# Check that the compiler emits ELF objects. Go by the macro for ELF targets,
# as GNU as cannot write an object to standard output. Clang is given an ELF
# target, and before version 17 did not define the macro for all of them.
ifneq ($(CC_IS_CLANG),1)
    override CC_EMITS_ELF := $(shell ! $(CC) $(CFLAGS) -dM -E -x c /dev/null 2>/dev/null | grep -q 'define __ELF__ 1'; echo $$?)
    ifneq ($(CC_EMITS_ELF),1)
        $(call TOOLCHAIN_ERROR,The compiler does not emit ELF objects)
    endif
endif

ifeq ($(CC_IS_CLANG),1)
    # Clang hands freestanding links over to another compiler driver on
    # some targets. Look for the linker flags being passed on as they
    # are, which a linker Clang runs itself would be given translated,
    # as the driver it picks need not be named "gcc" at all. The check
    # is made again below, so it runs on each use. The print flag is
    # kept in a variable, as older makes take a "#" in a call for a
    # comment.
    override CLANG_PRINT_COMMANDS := -\#\#\#
    override CC_LINKS_VIA_DRIVER = $(shell ! $(CC) $(CFLAGS) $(LDFLAGS) -Wl,--version $(CLANG_PRINT_COMMANDS) 2>&1 | grep '^ "' | tail -n 1 | grep -q -- '-Wl,'; echo $$?)

    # If it does, link as for the Linux target, where Clang runs the
    # linker itself, and keep that target's configuration files out. The
    # flag is looked for by compiling, as linking is not checked yet.
    ifeq ($(CC_LINKS_VIA_DRIVER),1)
        override LDFLAGS += \
            -target x86_64-linux-gnu
        ifeq ($(call CC_HAS_COMPILE_FLAG,--no-default-config),1)
            override LDFLAGS += \
                --no-default-config
        endif

        # Check that this worked, as the linker flags would otherwise
        # reach a compiler driver that knows nothing of them.
        ifeq ($(CC_LINKS_VIA_DRIVER),1)
            $(call TOOLCHAIN_ERROR,The compiler still hands the link over to another compiler driver)
        endif
    endif
endif

# The same for linking, once Clang has a linker it can run.
override CC_LINK_ERROR := $(shell LC_ALL=C $(CC) $(CFLAGS) $(LDFLAGS) -Wl,--version 2>&1 >/dev/null | grep -i 'error:' | head -n 1)
ifneq ($(CC_LINK_ERROR),)
    $(call TOOLCHAIN_ERROR,The linker does not work with the given LDFLAGS: $(CC_LINK_ERROR))
endif

# Some drivers ignore -fuse-ld on some targets, so check that the banner
# of the linker that runs names the one asked for, in any case.
override CC_FUSE_LD := $(lastword $(filter -fuse-ld=%,$(CFLAGS) $(LDFLAGS)))
ifneq ($(CC_FUSE_LD),)
    override CC_FUSE_LD_NAME := $(firstword $(subst -, ,$(patsubst ld.%,%,$(notdir $(patsubst -fuse-ld=%,%,$(CC_FUSE_LD))))))
    # GNU ld calls itself ld, not bfd.
    override CC_LD_BANNER_NAME := $(patsubst bfd,ld,$(CC_FUSE_LD_NAME))
    override CC_LD_VERSION := $(shell LC_ALL=C $(CC) $(CFLAGS) $(LDFLAGS) -Wl,--version 2>/dev/null | head -n 1)
    override CC_RUNS_FUSE_LD := $(shell ! printf '%s\n' '$(subst ',,$(CC_LD_VERSION))' | grep -qiE '(^|[^[:alnum:]])$(CC_LD_BANNER_NAME)([^[:alnum:]]|$$)'; echo $$?)
    ifneq ($(CC_RUNS_FUSE_LD),1)
        $(call TOOLCHAIN_ERROR,The compiler ignores $(CC_FUSE_LD) and runs another linker: $(CC_LD_VERSION))
    endif
endif

ifeq ($(call CC_HAS_COMPILE_FLAG,-fno-stack-clash-protection),1)
    override CFLAGS += \
        -fno-stack-clash-protection
endif

ifeq ($(call CC_HAS_COMPILE_FLAG,-fcf-protection=none),1)
    override CFLAGS += \
        -fcf-protection=none
endif

# GCC needs this flag for interrupt handlers and Clang below 3.9 has no
# name for it, where only a long double could reach the x87.
ifeq ($(call CC_HAS_COMPILE_FLAG,-mno-80387),1)
    override CFLAGS += \
        -mno-80387
endif

# Tell the compiler as well if it takes the flag, as it otherwise passes
# its own default on to the linker.
ifeq ($(call CC_HAS_LINK_FLAG,-pie),1)
    override LDFLAGS += \
        -pie
endif

# Use "find" to glob all *.c, *.S, and *.asm files in the tree and obtain the
# object and header dependency file names.
override SRCFILES := $(shell find -L src cc-runtime/src picoefi/x86_64 uACPI/source -type f 2>/dev/null | LC_ALL=C sort)
override CFILES := $(filter %.c,$(SRCFILES))
override ASFILES := $(filter %.S,$(SRCFILES))
override NASMFILES := $(filter %.asm,$(SRCFILES))
override OBJ := $(addprefix obj/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(NASMFILES:.asm=.asm.o))
override HEADER_DEPS := $(addprefix obj/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d) $(NASMFILES:.asm=.asm.d))

# Default target. This must come first, before header dependencies.
# SeaBIOS and the video BIOS go first, as the core carries them.
.PHONY: all
all:
	$(MAKE) seabios
	$(MAKE) vgabios
	$(MAKE) bin/$(OUTPUT).efi

# Include header dependencies.
-include $(HEADER_DEPS)

# Each kind of file depends on records of the variables it is built with,
# named after them. Each record gets its variable as set here, as target
# specific values, like the -fno-lto below, would otherwise reach it
# through the files depending on it.
override VARSFILES := $(addprefix obj/,CC OBJCOPY CFLAGS CPPFLAGS LDFLAGS NASMFLAGS)
$(foreach f,$(VARSFILES),$(eval $(f): override $(notdir $(f)) := $$($(notdir $(f)))))

# Remake the records that differ, compared here rather than by a recipe run
# every time, so that make can still tell when there is nothing to be done.
override CHANGED_VARSFILES := $(foreach f,$(VARSFILES),$(shell printf '%s\n' '$(subst ','\'',$($(notdir $(f))))' | cmp -s - $(f) 2>/dev/null || echo $(f)))
.PHONY: FORCE
FORCE:
$(CHANGED_VARSFILES): FORCE
$(VARSFILES):
	@mkdir -p "$(dir $@)"
	@printf '%s\n' '$(subst ','\'',$($(notdir $@)))' > $@

# Rule to convert the final ELF executable to a .EFI PE executable.
bin/$(OUTPUT).efi: bin/$(OUTPUT) GNUmakefile obj/OBJCOPY
	mkdir -p "$(dir $@)"
	$(OBJCOPY) -O binary $< $@
	dd if=/dev/null of=$@ bs=4096 seek=$$(( ($$(wc -c < $@) + 4095) / 4096 )) 2>/dev/null

# Link rules for the final executable.
bin/$(OUTPUT): GNUmakefile obj/CC obj/CFLAGS obj/LDFLAGS picoefi/x86_64/link_script.lds $(OBJ)
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) -o $@

# The compiler may emit calls to the memory functions and the compiler
# runtime that LTO does not account for, so never build those with LTO.
obj/src/memory.c.o obj/cc-runtime/src/cc-runtime.c.o: override CFLAGS += -fno-lto

# Only the printf implementation sees the nanoprintf header.
obj/src/lib/printf.c.o: override CPPFLAGS += \
    -I nanoprintf

# Only the CSM boot code sees the video BIOS's interface to the hypervisor.
obj/src/csm/csm_boot.c.o: override CPPFLAGS += \
    -I vgabios/src

# The core carries SeaBIOS.
obj/src/csm/seabios_blob.S.o: seabios/out/Csm16.bin

# And the video BIOS of the VGA it emulates.
obj/src/virt/vgabios_blob.S.o: vgabios/bin/vgabios.bin

# Compilation rules for *.c files.
obj/%.c.o: %.c GNUmakefile obj/CC obj/CFLAGS obj/CPPFLAGS
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compilation rules for *.S files.
obj/%.S.o: %.S GNUmakefile obj/CC obj/CFLAGS obj/CPPFLAGS
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compilation rules for *.asm (nasm) files.
obj/%.asm.o: %.asm GNUmakefile obj/NASMFLAGS
	mkdir -p "$(dir $@)"
	nasm $(NASMFLAGS) -MD $(@:.o=.d) -MP $< -o $@

# The USB stick's boot sector, outside the executable.
obj/usb/mbr.bin: usb/mbr.S GNUmakefile obj/CC obj/OBJCOPY
	mkdir -p "$(dir $@)"
	$(CC) -c -x assembler-with-cpp $< -o $(@:.bin=.o)
	$(OBJCOPY) -O binary -j .text $(@:.bin=.o) $@

# An image for a USB stick that runs ReBIOS: 8 MiB, its FAT16 partition from
# 1 MiB holding ReBIOS as the removable media boot file, and the stick's
# configuration.
.PHONY: usb
usb:
	$(MAKE) all
	$(MAKE) bin/$(OUTPUT)-usb.img

bin/$(OUTPUT)-usb.img: GNUmakefile obj/usb/mbr.bin usb/rebios.ini bin/$(OUTPUT).efi
	rm -f $@.tmp
	dd if=/dev/zero of=$@.tmp bs=1048576 count=8
	dd if=obj/usb/mbr.bin of=$@.tmp conv=notrunc
	mformat -i $@.tmp@@1M -T 14336 -h 255 -s 63 -H 2048 -c 1 -N 52424f53 -v REBIOS ::
	mmd -i $@.tmp@@1M ::/EFI ::/EFI/BOOT
	mcopy -m -i $@.tmp@@1M bin/$(OUTPUT).efi ::/EFI/BOOT/BOOTX64.EFI
	mcopy -m -i $@.tmp@@1M usb/rebios.ini ::/EFI/BOOT/rebios.ini
	mv $@.tmp $@

# Rules to download the UEFI firmware for testing.
.INTERMEDIATE: edk2-ovmf-bins.tar.gz
edk2-ovmf-bins.tar.gz:
	curl -fL -o $@ https://github.com/osdev0/edk2-ovmf-stable-bins/releases/latest/download/edk2-ovmf-bins.tar.gz

edk2-ovmf-bins: edk2-ovmf-bins.tar.gz
	rm -rf edk2-ovmf-bins
	gunzip < edk2-ovmf-bins.tar.gz | tar -xf -

# Rules for running our executable in QEMU.
.PHONY: run
run: all edk2-ovmf-bins
	mkdir -p boot/EFI/BOOT
	cp bin/$(OUTPUT).efi boot/EFI/BOOT/BOOTX64.EFI
	qemu-system-x86_64 \
		$(QEMU_MACHINE_FLAGS) \
		$(QEMU_UEFI_FLAGS) \
		-drive file=fat:rw:boot \
		$(QEMUFLAGS)
	rm -rf boot

# Remove object files and the final executable.
.PHONY: clean
clean:
	if test -f seabios/.config; then $(call SEABIOS_CALL,clean); fi
	$(call VGABIOS_CALL,clean)
	rm -rf bin obj

# Remove everything built and generated including downloaded dependencies.
.PHONY: distclean
distclean: clean
	rm -rf .deps-obtained .cache compile_commands.json freestanding-c-hdrs cc-runtime picoefi nanoprintf uACPI seabios edk2-ovmf-bins edk2-ovmf-bins.tar.gz

# Release tarballs are named after the commit's tag, without its "v", or else
# after its abbreviated hash, and their files are dated as the commit is.
ifneq ($(filter dist,$(MAKECMDGOALS)),)
    override DIST_OUTPUT := $(OUTPUT)-$(shell git describe --exact-match --tags 2>/dev/null | sed 's/^v//' | grep . || printf 'g%s' "$$(git rev-parse --short HEAD)")
    override DIST_DATE := $(shell TZ=UTC0 git -c log.showSignature=false log -1 --pretty=%cd --date='format-local:%Y%m%d%H%M')
endif

# Create release tarballs of the committed tree, with the dependencies in them.
# SeaBIOS's version comes from its .version file once its .git is gone.
.PHONY: dist
dist:
	rm -rf "$(DIST_OUTPUT)"
	mkdir -p "$(DIST_OUTPUT)"
	git archive HEAD | tar -xf - -C "$(DIST_OUTPUT)"
	cd "$(DIST_OUTPUT)" && ./get-deps
	git -C "$(DIST_OUTPUT)/seabios" describe --always --tags --long >"$(DIST_OUTPUT)/seabios/.version"
	cd "$(DIST_OUTPUT)" && rm -rf .gitignore .github
	cd "$(DIST_OUTPUT)" && for d in freestanding-c-hdrs cc-runtime picoefi nanoprintf uACPI seabios; do rm -rf "$$d/.git" "$$d/.github" "$$d/.gitignore"; done
	rm -rf "$(DIST_OUTPUT)/nanoprintf/tests" "$(DIST_OUTPUT)/uACPI/tests"
	find "$(DIST_OUTPUT)" -type f -perm -100 -exec chmod 755 '{}' +
	find "$(DIST_OUTPUT)" -type f ! -perm -100 -exec chmod 644 '{}' +
	TZ=UTC0 find "$(DIST_OUTPUT)" -exec touch -h -t $(DIST_DATE) '{}' +
	tar -cf "$(DIST_OUTPUT).tar" $$(find "$(DIST_OUTPUT)" ! -type d | LC_ALL=C sort)
	gzip -n <"$(DIST_OUTPUT).tar" >"$(DIST_OUTPUT).tar.gz"
	( xz <"$(DIST_OUTPUT).tar" >"$(DIST_OUTPUT).tar.xz" || rm -f "$(DIST_OUTPUT).tar.xz" )
	rm -rf "$(DIST_OUTPUT)" "$(DIST_OUTPUT).tar"

# Install the final built executable to its final on-root location. Not
# install(1), which takes other arguments on illumos and Solaris.
.PHONY: install
install: all
	mkdir -p "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
	cp bin/$(OUTPUT).efi "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/"
	chmod 644 "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT).efi"

# Try to undo whatever the "install" target did.
.PHONY: uninstall
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT).efi"
	-rmdir "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"

# SeaBIOS is built as a CSM, but not handed our flags: it ignores CFLAGS and
# LDFLAGS, and CPPFLAGS would replace its own.
override SEABIOS_EXTRAVERSION := -ReBIOS
override define SEABIOS_CALL
	$(MAKE) -C seabios $(1) \
		HOSTCC="$(HOST_CC)" \
		CC="$(SEABIOS_CC)" \
		LD="$(SEABIOS_LD)" \
		OBJCOPY="$(SEABIOS_OBJCOPY)" \
		OBJDUMP="$(SEABIOS_OBJDUMP)" \
		STRIP="$(SEABIOS_STRIP)" \
		EXTRAVERSION="$(SEABIOS_EXTRAVERSION)"
endef

# The emulated VGA's video BIOS, which its own makefile builds. Our command
# line's flags would reach it too, so it is handed its own.
override define VGABIOS_CALL
	$(MAKE) -C vgabios $(1) \
		CC="$(VGABIOS_CC)" \
		OBJCOPY="$(VGABIOS_OBJCOPY)" \
		CFLAGS="$(VGABIOS_CFLAGS)" \
		CPPFLAGS="$(VGABIOS_CPPFLAGS)" \
		LDFLAGS="$(VGABIOS_LDFLAGS)"
endef

# Video BIOS build targets.
.PHONY: vgabios
vgabios:
	$(call VGABIOS_CALL,)

# SeaBIOS build targets.
.PHONY: seabios
seabios: seabios/.config
	$(call SEABIOS_CALL,)

seabios/.config: GNUmakefile seabios-config
	cp seabios-config seabios/.config
	$(call SEABIOS_CALL,olddefconfig)
