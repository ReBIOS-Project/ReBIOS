# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of ReBIOS.
# Copyright (C) 2026 Mintsuki and contributors
#
# ReBIOS is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
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

# Target architecture to build for. Default to x86_64.
ARCH := x86_64

# Install prefix; /usr/local is a good, standard default pick.
PREFIX := /usr/local

# Check if the architecture is supported.
ifeq ($(filter $(ARCH),x86_64),)
    $(error Architecture $(ARCH) not supported)
endif

# Default user QEMU flags. These are appended to the QEMU command calls.
QEMUFLAGS := -m 2G

# Internal architecture specific variables that should not be changed by the
# user.
override QEMU_ARCH := $(ARCH)
override EFI_BOOT_FILE := BOOTX64.EFI
override QEMU_MACHINE_FLAGS := \
    -M q35
override QEMU_UEFI_FLAGS := \
    -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-$(ARCH).fd,readonly=on

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

# User controllable C flags.
CFLAGS := -g -O2 -pipe

# User controllable C preprocessor flags. We set none by default.
CPPFLAGS :=

# User controllable nasm flags.
NASMFLAGS := -g

# User controllable linker flags. We set none by default.
LDFLAGS :=

# Ensure the dependencies have been obtained.
ifneq ($(filter-out clean distclean,$(or $(MAKECMDGOALS),default)),)
    ifeq ($(wildcard .deps-obtained),)
        $(error Please run the ./get-deps script first)
    endif
endif

# Stop on a problem the checks below find, unless only cleaning, which needs
# no toolchain.
ifneq ($(filter-out clean distclean,$(or $(MAKECMDGOALS),default)),)
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
    -fdata-sections

# Internal C preprocessor flags that should not be changed by the user.
override CPPFLAGS := \
    -nostdinc \
    -I src \
    -I picoefi/inc \
    -isystem freestanding-c-hdrs/include \
    $(CPPFLAGS) \
    -MMD \
    -MP

# Internal nasm flags that should not be changed by the user.
override NASMFLAGS := \
    $(patsubst -g,-g -F dwarf,$(NASMFLAGS)) \
    -i src/ \
    -Wall

# Make Clang target the freestanding ELF triple of the architecture.
ifeq ($(CC_IS_CLANG),1)
    override CC += \
        -target $(ARCH)-unknown-none-elf
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
    -Wl,-T,picoefi/$(ARCH)/link_script.lds

# Architecture specific internal flags.
ifeq ($(ARCH),x86_64)
    override CFLAGS += \
        -m64 \
        -march=x86-64 \
        -mno-mmx \
        -mno-sse \
        -mno-red-zone
    override LDFLAGS += \
        -Wl,-m,elf_x86_64
    override NASMFLAGS := \
        -f elf64 \
        $(NASMFLAGS)
endif

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
            -target $(ARCH)-linux-gnu
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

# Architecture specific checks.
ifeq ($(ARCH),x86_64)
    ifeq ($(call CC_HAS_COMPILE_FLAG,-fcf-protection=none),1)
        override CFLAGS += \
            -fcf-protection=none
    endif
    # GCC needs this flag for interrupt handlers and Clang below 3.9 has
    # no name for it, where only a long double could reach the x87.
    ifeq ($(call CC_HAS_COMPILE_FLAG,-mno-80387),1)
        override CFLAGS += \
            -mno-80387
    endif
endif

# Tell the compiler as well if it takes the flag, as it otherwise passes
# its own default on to the linker.
ifeq ($(call CC_HAS_LINK_FLAG,-pie),1)
    override LDFLAGS += \
        -pie
endif

# Use "find" to glob all *.c, *.S, and *.asm files in the tree
# (except the src/arch/* directories, as those are gonna be added
# in the next step).
override SRCFILES := $(shell find -L src cc-runtime/src picoefi/$(ARCH) -type f ! -path 'src/arch/*' 2>/dev/null | LC_ALL=C sort)
# Add architecture specific files, if they exist.
override SRCFILES += $(shell find -L src/arch/$(ARCH) -type f 2>/dev/null | LC_ALL=C sort)
# Obtain the object and header dependencies file names.
override CFILES := $(filter %.c,$(SRCFILES))
override ASFILES := $(filter %.S,$(SRCFILES))
override NASMFILES := $(filter %.asm,$(SRCFILES))
override OBJ := $(addprefix obj-$(ARCH)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(NASMFILES:.asm=.asm.o))
override HEADER_DEPS := $(addprefix obj-$(ARCH)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d) $(NASMFILES:.asm=.asm.d))

# Default target. This must come first, before header dependencies.
.PHONY: all
all: bin-$(ARCH)/$(OUTPUT).efi

# Include header dependencies.
-include $(HEADER_DEPS)

# Rule to convert the final ELF executable to a .EFI PE executable.
bin-$(ARCH)/$(OUTPUT).efi: bin-$(ARCH)/$(OUTPUT) GNUmakefile
	mkdir -p "$(dir $@)"
	$(OBJCOPY) -O binary $< $@
	dd if=/dev/null of=$@ bs=4096 seek=$$(( ($$(wc -c < $@) + 4095) / 4096 )) 2>/dev/null

# Link rules for the final executable.
bin-$(ARCH)/$(OUTPUT): GNUmakefile picoefi/$(ARCH)/link_script.lds $(OBJ)
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) -o $@

# The compiler may emit calls to the memory functions and the compiler
# runtime that LTO does not account for, so never build those with LTO.
obj-$(ARCH)/src/memory.c.o obj-$(ARCH)/cc-runtime/src/cc-runtime.c.o: override CFLAGS += -fno-lto

# Compilation rules for *.c files.
obj-$(ARCH)/%.c.o: %.c GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compilation rules for *.S files.
obj-$(ARCH)/%.S.o: %.S GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compilation rules for *.asm (nasm) files.
obj-$(ARCH)/%.asm.o: %.asm GNUmakefile
	mkdir -p "$(dir $@)"
	nasm $(NASMFLAGS) -MD $(@:.o=.d) -MP $< -o $@

# Rules to download the UEFI firmware per architecture for testing.
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
	cp bin-$(ARCH)/$(OUTPUT).efi boot/EFI/BOOT/$(EFI_BOOT_FILE)
	qemu-system-$(QEMU_ARCH) \
		$(QEMU_MACHINE_FLAGS) \
		$(QEMU_UEFI_FLAGS) \
		-drive file=fat:rw:boot \
		$(QEMUFLAGS)
	rm -rf boot

# Remove object files and the final executable.
.PHONY: clean
clean:
	rm -rf bin-$(ARCH) obj-$(ARCH)

# Remove everything built and generated including downloaded dependencies.
.PHONY: distclean
distclean:
	rm -rf bin-* obj-* .deps-obtained .cache compile_commands.json freestanding-c-hdrs cc-runtime picoefi edk2-ovmf-bins edk2-ovmf-bins.tar.gz

# Install the final built executable to its final on-root location. Not
# install(1), which takes other arguments on illumos and Solaris.
.PHONY: install
install: all
	mkdir -p "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
	cp bin-$(ARCH)/$(OUTPUT).efi "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(ARCH).efi"
	chmod 644 "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(ARCH).efi"

# Try to undo whatever the "install" target did.
.PHONY: uninstall
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(ARCH).efi"
	-rmdir "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
