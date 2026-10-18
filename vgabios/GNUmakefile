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

# The emulated VGA's video BIOS: real-mode code the guest runs from segment
# C000h, as a ROM image. It takes GCC, for its named address spaces, and GNU
# binutils.

# Nuke built-in rules.
.SUFFIXES:

# Delete the target of a failed recipe.
.DELETE_ON_ERROR:

# This is the name that our final ROM image will have.
override OUTPUT := vgabios

# User controllable toolchain prefix.
TOOLCHAIN_PREFIX :=

# User controllable C compiler command.
CC := $(TOOLCHAIN_PREFIX)gcc

# User controllable objcopy command.
OBJCOPY := $(TOOLCHAIN_PREFIX)objcopy

# User controllable C flags.
CFLAGS := -g -O2 -pipe

# User controllable C preprocessor flags. We set none by default.
CPPFLAGS :=

# User controllable linker flags. We set none by default.
LDFLAGS :=

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

# Internal C flags that should not be changed by the user. The ROM has to fit
# in 32 KiB, whatever optimisation level is asked for. It reads its own data
# through FS, so it has neither jump tables nor tables of switch values, which
# it would read through DS, and no calls for loops to library functions it
# does not have. It keeps to the 386's instructions, as software emulators run
# video BIOSes too, Windows' on x86-64 among them.
override CFLAGS += \
    -Os \
    -Wall \
    -Wextra \
    -std=gnu11 \
    -ffreestanding \
    -fno-common \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-PIC \
    -fno-jump-tables \
    -fno-tree-switch-conversion \
    -fno-tree-loop-distribute-patterns \
    -ffunction-sections \
    -fdata-sections \
    -m16 \
    -march=i386 \
    -mregparm=3 \
    -mno-mmx

# Internal C preprocessor flags that should not be changed by the user.
override CPPFLAGS := \
    -nostdinc \
    -I src \
    -isystem ../freestanding-c-hdrs/include \
    $(CPPFLAGS) \
    -MMD \
    -MP

# Internal linker flags that should not be changed by the user.
override LDFLAGS += \
    -nostdlib \
    -static \
    -Wl,--gc-sections \
    -Wl,--build-id=none \
    -Wl,-T,vgabios.lds \
    -Wl,-m,elf_i386

ifeq ($(call CC_HAS_COMPILE_FLAG,-fno-stack-clash-protection),1)
    override CFLAGS += \
        -fno-stack-clash-protection
endif

ifeq ($(call CC_HAS_COMPILE_FLAG,-fcf-protection=none),1)
    override CFLAGS += \
        -fcf-protection=none
endif

ifeq ($(call CC_HAS_COMPILE_FLAG,-mno-80387),1)
    override CFLAGS += \
        -mno-80387
endif

# Use "find" to glob all *.c and *.S files in the tree and obtain the object
# and header dependency file names.
override SRCFILES := $(shell find -L src -type f 2>/dev/null | LC_ALL=C sort)
override CFILES := $(filter %.c,$(SRCFILES))
override ASFILES := $(filter %.S,$(SRCFILES))
override OBJ := $(addprefix obj/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o))
override HEADER_DEPS := $(addprefix obj/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))

# Default target. This must come first, before header dependencies.
.PHONY: all
all: bin/$(OUTPUT).bin

# Include header dependencies.
-include $(HEADER_DEPS)

# Each kind of file depends on records of the variables it is built with,
# named after them. Each record gets its variable as set here, as target
# specific values would otherwise reach it through the files depending on
# it.
override VARSFILES := $(addprefix obj/,CC OBJCOPY CFLAGS CPPFLAGS LDFLAGS)
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

# Rule to convert the final ELF executable to the flat ROM image.
bin/$(OUTPUT).bin: bin/$(OUTPUT) GNUmakefile obj/OBJCOPY
	mkdir -p "$(dir $@)"
	$(OBJCOPY) -O binary $< $@

# Link rules for the final executable.
bin/$(OUTPUT): GNUmakefile obj/CC obj/CFLAGS obj/LDFLAGS vgabios.lds $(OBJ)
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) -o $@

# Compilation rules for *.c files.
obj/%.c.o: %.c GNUmakefile obj/CC obj/CFLAGS obj/CPPFLAGS
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compilation rules for *.S files.
obj/%.S.o: %.S GNUmakefile obj/CC obj/CFLAGS obj/CPPFLAGS
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Remove object files and the final ROM image.
.PHONY: clean
clean:
	rm -rf bin obj
