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
QEMUFLAGS := -m 2G
# The processor that ReBIOS gets, emulated with every extension of it that
# ReBIOS can use: vmx, Intel's, behind VT-d, or svm, AMD's, behind AMD-Vi, each
# IOMMU with interrupt remapping and its x2APIC mode.
QEMU_VIRT := vmx
QEMU_MACHINE := -M q35 -accel tcg
QEMU_DEVICES := \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-mouse

override EMPTY :=
override SPACE := $(EMPTY) $(EMPTY)
override COMMA := ,
# Intel's family 6, model 198, with all of TCG's extensions and VMX with all of
# its controls.
override QEMU_CPU_vmx := max,vendor=GenuineIntel,family=6,model=198,+vmx,-svm
override QEMU_IOMMU_vmx := intel-iommu,intremap=on,eim=on
# AMD's EPYC-Genoa with SVM and all of SVM's extensions, less what TCG cannot
# run: AVX-512, GFNI, VPCLMULQDQ, AutoIBRS, and LFENCE's serialising control.
override QEMU_SVM_ADDED := \
    svm npt nrip-save vmcb-clean flushbyasid decodeassists pause-filter \
    pfthreshold vgif vnmi lbrv v-vmsave-vmload avic x2avic x2apic extapic \
    invlpgb tlbictl rogpt pml tsc-scale gmet svm-lock idle-hlt \
    bus-lock-threshold host-mce-override eraps sbpb ibpb-brtype cet-ss fred \
    lkgs bpext addrmaskext bus-lock-detect
override QEMU_SVM_REMOVED := \
    avx512f avx512dq avx512ifma avx512cd avx512bw avx512vl avx512vbmi \
    avx512vbmi2 avx512vnni avx512bitalg avx512-vpopcntdq avx512-bf16 gfni \
    vpclmulqdq auto-ibrs lfence-always-serializing
override QEMU_CPU_svm := EPYC-Genoa,$(subst $(SPACE),$(COMMA),$(strip \
    $(addprefix +,$(QEMU_SVM_ADDED)) $(addprefix -,$(QEMU_SVM_REMOVED))))
override QEMU_IOMMU_svm := amd-iommu,dma-remap=on,intremap=on,xtsup=on
ifeq ($(QEMU_CPU_$(QEMU_VIRT)),)
    $(error QEMU_VIRT must be vmx or svm)
endif

override QEMUFLAGS := \
    $(QEMU_MACHINE) \
    -cpu $(QEMU_CPU_$(QEMU_VIRT)) \
    -device $(QEMU_IOMMU_$(QEMU_VIRT)) \
    $(QEMU_DEVICES) \
    $(QEMUFLAGS)
override QEMU_UEFI_FW := \
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

# The linker that LDFLAGS picks, if any, which links SeaBIOS too.
override FUSE_LD := $(lastword $(filter -fuse-ld=%,$(CFLAGS) $(LDFLAGS)))

# User controllable toolchain prefix for SeaBIOS.
SEABIOS_TOOLCHAIN_PREFIX := $(TOOLCHAIN_PREFIX)

# User controllable commands for building SeaBIOS. -fuse-ld names a linker the
# way a compiler driver finds it: a path, or the suffix of "ld".
SEABIOS_CC := $(SEABIOS_TOOLCHAIN_PREFIX)gcc
ifeq ($(TOOLCHAIN),llvm)
    SEABIOS_CC := clang -target x86_64-unknown-none-elf
endif
ifneq ($(findstring /,$(FUSE_LD)),)
    SEABIOS_LD := $(patsubst -fuse-ld=%,%,$(FUSE_LD))
else ifeq ($(TOOLCHAIN),llvm)
    SEABIOS_LD := ld.$(if $(FUSE_LD),$(patsubst -fuse-ld=%,%,$(FUSE_LD)),lld)
else
    SEABIOS_LD := $(SEABIOS_TOOLCHAIN_PREFIX)ld$(patsubst -fuse-ld=%,.%,$(FUSE_LD))
endif
SEABIOS_OBJCOPY := $(SEABIOS_TOOLCHAIN_PREFIX)objcopy
SEABIOS_STRIP := $(SEABIOS_TOOLCHAIN_PREFIX)strip

# User controllable host C compiler, which builds SeaBIOS's tools. It is given
# the linker when compiling too, which Clang would warn of.
HOST_CC := cc
ifeq ($(TOOLCHAIN),llvm)
    HOST_CC := clang $(or $(FUSE_LD),-fuse-ld=lld) -Qunused-arguments
endif

# User controllable C flags.
CFLAGS := -g -O2 -pipe

# User controllable C preprocessor flags. We set none by default.
CPPFLAGS :=

# User controllable nasm flags.
NASMFLAGS := -g

# User controllable linker flags. We set none by default.
LDFLAGS :=

# The goals other than cleaning, uninstalling, and making a release tarball,
# which need neither the dependencies nor a toolchain.
override BUILD_GOALS := $(filter-out clean distclean dist uninstall,$(or $(MAKECMDGOALS),all))

# Ensure the dependencies have been obtained, and set up the toolchain
# helper, which records the flags as given, before the internal ones.
ifneq ($(BUILD_GOALS),)
    ifeq ($(wildcard .deps-obtained),)
        $(error Please run the ./get-deps script first)
    endif
    override TCH_ARCH := x86_64
    override TCH_TARGET_MACRO := __x86_64__
    override TCH_CLANG_TARGET := x86_64-unknown-none-elf
    override TCH_SELECTORS := TOOLCHAIN TOOLCHAIN_PREFIX
    override TCH_CROSS_HINT := use TOOLCHAIN=llvm or a cross compiler for x86_64 with TOOLCHAIN_PREFIX
    include gmake-toolchain-helper/init.mk
endif

# Internal C flags that should not be changed by the user.
override CFLAGS += \
    -Wall \
    -Wextra \
    -Wshadow \
    -Wvla \
    -std=gnu11 \
    -ffreestanding \
    -fno-builtin \
    -fno-exceptions \
    -fno-asynchronous-unwind-tables \
    -fno-unwind-tables \
    -fno-common \
    -fno-strict-aliasing \
    -fno-stack-protector \
    -fno-stack-check \
    -fshort-wchar \
    -fPIE \
    -fvisibility=hidden \
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
    -include hidden.h \
    -I src \
    -I picoefi/include \
    -I uACPI/include \
    -isystem freestanding-c-hdrs/include \
    $(CPPFLAGS) \
    -MD \
    -MP

# Internal nasm flags that should not be changed by the user.
override NASMFLAGS := \
    -f elf64 \
    $(patsubst -g,-g -F dwarf,$(NASMFLAGS)) \
    -i src/ \
    -Wall

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
    -Wl,-T,picoefi/src/x86_64/link_script.lds \
    -Wl,-m,elf_x86_64

# Check the toolchain with the internal flags, which also makes Clang target
# the architecture and link with LLD unless LDFLAGS picks another linker.
ifneq ($(BUILD_GOALS),)
    include gmake-toolchain-helper/check.mk
endif

# Flags that depend on the toolchain. When nothing is to be built, the
# helper is not included: its macros expand to nothing, and TCH_CC_IS_CLANG
# is empty.
override CFLAGS += \
    $(call TCH_DRIVER_HAS_COMPILE_FLAG,CC,-fno-stack-clash-protection)
override CFLAGS += \
    $(call TCH_DRIVER_HAS_COMPILE_FLAG,CC,-fcf-protection=none)

# GCC needs this flag for interrupt handlers and Clang below 3.9 has no
# name for it, where only a long double could reach the x87.
override CFLAGS += \
    $(call TCH_DRIVER_HAS_COMPILE_FLAG,CC,-mno-80387)

# Older GCCs need this for ms_abi calls. Clang has no such flag.
ifneq ($(TCH_CC_IS_CLANG),1)
    override CFLAGS += \
        -maccumulate-outgoing-args
endif

# Pack relative relocations where the linker can, as RELR tables take a
# fraction of the room. LLD before 15 only knows --pack-dyn-relocs.
ifneq ($(call TCH_LINKER_HAS_Z_FLAG,pack-relative-relocs),)
    override LDFLAGS += \
        -Wl,-z,pack-relative-relocs
else
    ifneq ($(call TCH_LINKER_HAS_FLAG,--pack-dyn-relocs),)
        override LDFLAGS += \
            -Wl,--pack-dyn-relocs=relr
    endif
endif

# Tell the compiler as well if it takes the flag, as it otherwise passes
# its own default on to the linker.
override LDFLAGS += \
    $(call TCH_DRIVER_HAS_LINK_FLAG,-pie)

# Use "find" to glob all *.c, *.S, and *.asm files in the tree and obtain the
# object and header dependency file names.
override SRCFILES := $(shell find -L src freestanding-lgrt/src picoefi/src/x86_64 flanterm/src uACPI/source tendril/src -type f 2>/dev/null | LC_ALL=C sort)
override CFILES := $(filter %.c,$(SRCFILES))
override ASFILES := $(filter %.S,$(SRCFILES))
override NASMFILES := $(filter %.asm,$(SRCFILES))
override OBJ := $(addprefix obj/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(NASMFILES:.asm=.asm.o))
override HEADER_DEPS := $(addprefix obj/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d) $(NASMFILES:.asm=.asm.d))

# Default target. This must come first, before header dependencies.
# SeaBIOS goes first, as the core carries it.
.PHONY: all
all:
	$(MAKE) seabios
	$(MAKE) bin/$(OUTPUT).efi

# Include header dependencies.
-include $(HEADER_DEPS)

# Each kind of file depends on records of the variables it is built with,
# named after them, which the toolchain helper remakes when they change.
override TCH_RECORDS := $(addprefix obj/,CC OBJCOPY CFLAGS CPPFLAGS LDFLAGS NASMFLAGS)
ifneq ($(BUILD_GOALS),)
    include gmake-toolchain-helper/records.mk
endif

# Rule to convert the final ELF executable to a .EFI PE executable.
bin/$(OUTPUT).efi: bin/$(OUTPUT) GNUmakefile obj/OBJCOPY
	mkdir -p "$(dir $@)"
	$(OBJCOPY) -O binary $< $@

# Link rules for the final executable.
bin/$(OUTPUT): GNUmakefile obj/CC obj/CFLAGS obj/LDFLAGS picoefi/src/x86_64/link_script.lds $(OBJ)
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) -o $@

# The compiler may emit calls to the memory functions and the compiler
# runtime that LTO does not account for, so never build those with LTO.
obj/src/memory.c.o obj/freestanding-lgrt/src/freestanding-lgrt.c.o: override CFLAGS += -fno-lto

# The emulated VGA's drawing kernels for each SIMD level, among which each
# processor picks at start (src/sys/simd.h), are built with that level's
# instructions.
obj/src/virt/vga_draw_sse2.c.o: override CFLAGS += -msse2
obj/src/virt/vga_draw_ssse3.c.o: override CFLAGS += -mssse3
obj/src/virt/vga_draw_sse4_1.c.o: override CFLAGS += -msse4.1
obj/src/virt/vga_draw_avx.c.o: override CFLAGS += -mavx
obj/src/virt/vga_draw_avx2.c.o: override CFLAGS += -mavx2

# Only the printf implementation sees the nanoprintf header.
obj/src/lib/printf.c.o: override CPPFLAGS += \
    -I nanoprintf

# Only the log and the console it writes to see Flanterm's headers.
obj/src/lib/log.c.o obj/src/loader/console.c.o: override CPPFLAGS += \
    -I flanterm/src

# Flanterm's memory comes from the console.
obj/flanterm/src/flanterm_backends/fb.c.o: override CPPFLAGS += \
    -DFLANTERM_FB_DISABLE_BUMP_ALLOC

# Only Tendril and what uses it see its headers.
obj/tendril/%: override CPPFLAGS += \
    -I tendril/include
obj/src/virt/usb.c.o obj/src/virt/usb_disk.c.o obj/src/virt/usb_legacy.c.o: override CPPFLAGS += \
    -I tendril/include

# The core carries SeaBIOS.
obj/src/csm/seabios_blob.S.o: seabios/out/Csm16.bin

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

# Rules for running our executable in Nephrite, the fork of QEMU that ReBIOS is
# tested on, as QEMU itself has bugs that ReBIOS runs into.
.PHONY: run
run: all edk2-ovmf-bins
	mkdir -p boot/EFI/BOOT
	cp bin/$(OUTPUT).efi boot/EFI/BOOT/BOOTX64.EFI
	nephrite-system-x86_64 \
		-drive file=fat:rw:boot \
		$(QEMUFLAGS) \
		$(QEMU_UEFI_FW)
	rm -rf boot

# Remove object files and the final executable.
.PHONY: clean
clean:
	rm -rf seabios/out
	rm -rf bin obj

# Remove everything built and generated including downloaded dependencies.
.PHONY: distclean
distclean: clean
	rm -rf .deps-obtained .cache compile_commands.json freestanding-c-hdrs freestanding-lgrt gmake-toolchain-helper picoefi nanoprintf flanterm uACPI tendril seabios edk2-ovmf-bins edk2-ovmf-bins.tar.gz

# Release tarballs are named after the commit's tag, without its "v", or else
# after its abbreviated hash, and their files are dated as the commit is.
ifneq ($(filter dist,$(MAKECMDGOALS)),)
    override DIST_OUTPUT := $(OUTPUT)-$(shell git describe --exact-match --tags 2>/dev/null | sed 's/^v//' | grep . || printf 'g%s' "$$(git rev-parse --short HEAD)")
    override DIST_DATE := $(shell TZ=UTC0 git -c log.showSignature=false log -1 --pretty=%cd --date='format-local:%Y%m%d%H%M')
endif

# Create release tarballs of the committed tree, with the dependencies in them.
# There is no .git in them, so ReBIOS and SeaBIOS read .version files instead.
.PHONY: dist
dist:
	rm -rf "$(DIST_OUTPUT)"
	mkdir -p "$(DIST_OUTPUT)"
	git archive HEAD | tar -xf - -C "$(DIST_OUTPUT)"
	git describe --always --tags | sed 's/^v//' >"$(DIST_OUTPUT)/.version"
	cd "$(DIST_OUTPUT)" && ./get-deps
	git -C "$(DIST_OUTPUT)/seabios" describe --always --tags --long >"$(DIST_OUTPUT)/seabios/.version"
	cd "$(DIST_OUTPUT)" && rm -rf .gitignore .github
	cd "$(DIST_OUTPUT)" && for d in freestanding-c-hdrs freestanding-lgrt gmake-toolchain-helper picoefi nanoprintf flanterm uACPI tendril seabios; do rm -rf "$$d/.git" "$$d/.github" "$$d/.gitignore"; done
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
	rmdir "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)" 2>/dev/null || :

# Our version, for SeaBIOS's banner, found as SeaBIOS finds its own: from Git,
# or else from the .version file of a release tarball.
override REBIOS_VERSION := $(shell if test -e .git; then git describe --always --tags --dirty 2>/dev/null | sed 's/^v//'; elif test -f .version; then head -n 1 .version; fi)

# SeaBIOS is built as a CSM, but not handed our flags: it ignores CFLAGS and
# LDFLAGS, and CPPFLAGS would replace its own.
override SEABIOS_EXTRAVERSION := -ReBIOS$(if $(REBIOS_VERSION),-$(REBIOS_VERSION))
override define SEABIOS_CALL
	$(MAKE) -C seabios $(1) \
		HOSTCC="$(HOST_CC)" \
		CC="$(SEABIOS_CC)" \
		LD="$(SEABIOS_LD)" \
		OBJCOPY="$(SEABIOS_OBJCOPY)" \
		STRIP="$(SEABIOS_STRIP)" \
		EXTRAVERSION="$(SEABIOS_EXTRAVERSION)"
endef

# SeaBIOS build targets. SeaBIOS reads its version only to lay out its ROM, so
# have it do that again when ours changes, kept in a record as the flags are.
# It does not track its tools, so have it start over when they change.
override SEABIOS_TOOLS := $(SEABIOS_CC) $(SEABIOS_LD) $(SEABIOS_OBJCOPY) $(SEABIOS_STRIP)
.PHONY: seabios
seabios: seabios/.config
	@mkdir -p obj
	@if ! printf '%s\n' '$(SEABIOS_TOOLS)' | cmp -s - obj/SEABIOS_TOOLS 2>/dev/null; then \
		rm -rf seabios/out; \
		printf '%s\n' '$(SEABIOS_TOOLS)' > obj/SEABIOS_TOOLS; \
	fi
	@if ! printf '%s\n' '$(SEABIOS_EXTRAVERSION)' | cmp -s - obj/SEABIOS_EXTRAVERSION 2>/dev/null; then \
		rm -f seabios/out/romlayout16.lds; \
		printf '%s\n' '$(SEABIOS_EXTRAVERSION)' > obj/SEABIOS_EXTRAVERSION; \
	fi
	$(call SEABIOS_CALL,)

seabios/.config: GNUmakefile seabios-config
	cp seabios-config seabios/.config
	$(call SEABIOS_CALL,olddefconfig)
