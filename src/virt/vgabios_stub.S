// SPDX-License-Identifier: GPL-2.0-only
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License version 2 as published by the Free
// Software Foundation.
//
// ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

// The start of the emulated VGA's video BIOS: the code guests run, which asks
// the hypervisor what to do through vgabios.c's ports, and carries it out
// here, in the caller's context: operating systems virtualise the VGA by
// trapping what a video BIOS does, and software emulators run this with memory
// and registers of their own. The calls programs make all the time for text
// are done here whole, as each word from the hypervisor costs an exit. It
// keeps to the 8086's simplest instructions, as those emulators have been seen
// to get others wrong. This is data to the hypervisor.

#include <virt/bda.h>

.section .rodata.vgabios_stub, "a"
.code16
.arch i8086

#define PORT_ALLOCATE 0x1d0
#define PORT_VGA_ALLOCATE 0x3de
#define KNOCK 0x5242
#define KNOCK_INIT 0x5243
#define VGA_PORTS 0x3b0

#define FRAME_AX 0
#define FRAME_BX 2
#define FRAME_BH 3
#define FRAME_CX 4
#define FRAME_DX 6

.globl vgabios_stub
vgabios_stub:
.byte 0x55, 0xaa
// The ROM's size in 512-byte blocks, which the hypervisor fills in.
.byte 0
// The initialisation entry point, which the BIOS calls far at offset 3.
.byte 0xe9
.word init_entry - (. + 2)
// Software looks for an IBM video BIOS by this signature.
.org 0x1e
.ascii "IBM"

// The registers vgabios.c's FRAME_ offsets find.
.macro save
    pushw %es
    pushw %ds
    pushw %bp
    pushw %di
    pushw %si
    pushw %dx
    pushw %cx
    pushw %bx
    pushw %ax
    movw %sp, %bp
    cld
.endm

.macro restore
    popw %ax
    popw %bx
    popw %cx
    popw %dx
    popw %si
    popw %di
    popw %bp
    popw %ds
    popw %es
.endm

init_entry:
    save
    movw $KNOCK_INIT, %ax
    callw knock
    restore
    lretw

.globl vgabios_stub_int10
vgabios_stub_int10:
    save
    cmpb $0x10, %ah
    jae door
    movb %ah, %bl
    xorb %bh, %bh
    shlw $1, %bx
    movw $BDA, %si
    movw %si, %ds
    jmpw *%cs:(calls_here - vgabios_stub)(%bx)

door:
    callw run
finish:
    restore
    iretw

to_door:
    jmp door
to_finish:
    jmp finish

// AH=0Eh, in text modes, but for what scrolls the screen.
teletype:
    callw text_segment
    jc to_door
    movb BDA_PAGE, %al
    cmpb $8, %al
    jae to_door
    callw page_cursor
    movw %cx, %dx
    movb FRAME_AX(%bp), %al
    cmpb $7, %al
    je to_finish
    cmpb $8, %al
    je .Lteletype_2
    cmpb $10, %al
    je .Lteletype_3
    cmpb $13, %al
    je .Lteletype_4
    incb %dl
    cmpb BDA_COLUMNS, %dl
    jb .Lteletype_1
    xorb %dl, %dl
    incb %dh
.Lteletype_1:
    callw on_screen
    jnc to_door
    callw text_offset
    movb FRAME_AX(%bp), %al
    stosb
    jmp .Lteletype_6
.Lteletype_2:
    testb %dl, %dl
    jz .Lteletype_5
    decb %dl
    jmp .Lteletype_5
.Lteletype_3:
    incb %dh
    jmp .Lteletype_5
.Lteletype_4:
    xorb %dl, %dl
.Lteletype_5:
    callw on_screen
    jnc to_door
.Lteletype_6:
    movw %dx, BDA_CURSOR(%bx)
    movw %dx, %ax
    jmp cursor_shown

// AH=02h.
set_cursor:
    movb FRAME_BH(%bp), %al
    callw page_cursor
    movw FRAME_DX(%bp), %ax
    movw %ax, BDA_CURSOR(%bx)
    movw %di, %cx
    cmpb BDA_PAGE, %cl
    jne to_finish

// Shows the cursor at the row in AH and the column in AL of the active page.
cursor_shown:
    movb %al, %cl
    xorb %ch, %ch
    movb %ah, %al
    xorb %ah, %ah
    mulw BDA_COLUMNS
    addw %cx, %ax
    movw BDA_PAGE_START, %cx
    shrw $1, %cx
    addw %cx, %ax
    movw %ax, %cx
    movw BDA_CRTC, %dx
    movb $0x0e, %al
    movb %ch, %ah
    outw %ax, %dx
    movb $0x0f, %al
    movb %cl, %ah
    outw %ax, %dx
    jmp finish

// AH=03h.
get_cursor:
    movb FRAME_BH(%bp), %al
    callw page_cursor
    movw %cx, FRAME_DX(%bp)
    movw BDA_CURSOR_TYPE, %ax
    movw %ax, FRAME_CX(%bp)
    jmp finish

// AH=0Fh.
get_mode:
    movb BDA_VIDEO_CONTROL, %al
    andb $0x80, %al
    orb BDA_MODE, %al
    movb BDA_COLUMNS, %ah
    movw %ax, FRAME_AX(%bp)
    movb BDA_PAGE, %al
    movb %al, FRAME_BH(%bp)
    jmp finish

// AH=08h, in text modes.
read_char:
    callw text_cell
    jc to_door
    movw %es:(%di), %ax
    movw %ax, FRAME_AX(%bp)
    jmp finish

// AH=09h, in text modes.
write_char_attribute:
    callw text_cell
    jc to_door
    movw FRAME_AX(%bp), %ax
    movb FRAME_BX(%bp), %ah
    movw FRAME_CX(%bp), %cx
    rep stosw
    jmp finish

// AH=0Ah, in text modes.
write_char:
    callw text_cell
    jc to_door
    movb FRAME_AX(%bp), %al
    movw FRAME_CX(%bp), %cx
    jcxz .Lwrite_char_2
.Lwrite_char_1:
    stosb
    incw %di
    loop .Lwrite_char_1
.Lwrite_char_2:
    jmp finish

// AH=0Ch, in mode 13h.
write_pixel:
    cmpb $0x13, BDA_MODE
    jne to_door_pixel
    callw pixel_place
    movb FRAME_AX(%bp), %al
    stosb
    jmp finish

// AH=0Dh, in mode 13h.
read_pixel:
    cmpb $0x13, BDA_MODE
    jne to_door_pixel
    callw pixel_place
    movb %es:(%di), %al
    movb %al, FRAME_AX(%bp)
    jmp finish

to_door_pixel:
    jmp door

// ES:DI at the pixel in the saved CX and DX.
pixel_place:
    movw $0xa000, %ax
    movw %ax, %es
    movw FRAME_DX(%bp), %ax
    movw $320, %dx
    mulw %dx
    addw FRAME_CX(%bp), %ax
    movw %ax, %di
    retw

// ES:DI at the cursor of the page in the saved BH, carry set if there is no
// text mode.
text_cell:
    callw text_segment
    jc .Ltext_cell_1
    movb FRAME_BH(%bp), %al
    callw page_cursor
    callw text_offset
    clc
.Ltext_cell_1:
    retw

// ES at the text of the current mode, carry set if it has none.
text_segment:
    movb BDA_MODE, %al
    andb $0x7f, %al
    movw $0xb800, %si
    cmpb $4, %al
    jb .Ltext_segment_1
    movw $0xb000, %si
    cmpb $7, %al
    stc
    jne .Ltext_segment_2
.Ltext_segment_1:
    movw %si, %es
    clc
.Ltext_segment_2:
    retw

// For the page in AL: DI that page, BX twice that, and CX its cursor.
page_cursor:
    andw $7, %ax
    movw %ax, %di
    shlw $1, %ax
    movw %ax, %bx
    movw BDA_CURSOR(%bx), %cx
    retw

// DI at the cell of the row in CH and the column in CL of the page in DI.
text_offset:
    pushw %dx
    movb %ch, %al
    xorb %ah, %ah
    mulw BDA_COLUMNS
    movb %cl, %dl
    xorb %dh, %dh
    addw %dx, %ax
    shlw $1, %ax
    xchgw %ax, %di
    mulw BDA_PAGE_SIZE
    addw %ax, %di
    popw %dx
    retw

// Carry set if the row in DH is on the screen.
on_screen:
    movb BDA_ROWS, %al
    incb %al
    cmpb %al, %dh
    retw

// By AH, what is done here, up to the last the table has.
calls_here:
.word door - vgabios_stub
.word door - vgabios_stub
.word set_cursor - vgabios_stub
.word get_cursor - vgabios_stub
.word door - vgabios_stub
.word door - vgabios_stub
.word door - vgabios_stub
.word door - vgabios_stub
.word read_char - vgabios_stub
.word write_char_attribute - vgabios_stub
.word write_char - vgabios_stub
.word door - vgabios_stub
.word write_pixel - vgabios_stub
.word read_pixel - vgabios_stub
.word teletype - vgabios_stub
.word get_mode - vgabios_stub

// VBE's window function, which takes AX=4F05h's registers but for AX.
.globl vgabios_stub_window
vgabios_stub_window:
    save
    movw $0x4f05, (%bp)
    callw run
    restore
    lretw

// A call gets a port of its own, so that one interrupted by another, as a
// virtual machine by the next, carries on afterwards. 0 means it is over, in
// real mode, and all ones that the port does not reach the hypervisor, as from
// Windows NT's DOS boxes, which let a program have only the VGA's ports.
run:
    movw $KNOCK, %ax
knock:
    movw %ax, %si
    movw $PORT_ALLOCATE, %dx
    outw %ax, %dx
    inw %dx, %ax
    cmpw $0xffff, %ax
    jne .Lknock_1
    movw %si, %ax
    movw $PORT_VGA_ALLOCATE, %dx
    outw %ax, %dx
    inw %dx, %ax
.Lknock_1:
    testw %ax, %ax
    jz op_done
    cmpw $0xffff, %ax
    je op_done
    movw %ax, %dx

// What to do next, with the port in DX: a word that is all ones if the call
// was taken away, then in AL an operation, or a VGA port and whether to read
// it or write AH to it. An operation takes more words from the port, in the
// order its code here reads them.
next:
    inw %dx, %ax
    cmpw $0xffff, %ax
    je op_done
    cmpb $0x80, %al
    jae vga_port
    cmpb $0x20, %al
    jae vga_indexed
    movb %al, %bl
    xorb %bh, %bh
    shlw $1, %bx
    jmpw *%cs:(operations - vgabios_stub)(%bx)

// An index in AL and its value in AH to the register set AL also tells: in one
// instruction, as nothing should come between them, and Windows NT's VGA
// driver puts the sequencer's index back when a read comes during a reset.
vga_indexed:
    movw %dx, %si
    movb %al, %bl
    andb $0x1f, %al
    movw $0x3d4, %dx
    cmpb $0x40, %bl
    jb .Lvga_indexed_1
    movw $0x3b4, %dx
    cmpb $0x60, %bl
    jb .Lvga_indexed_1
    andb $0x0f, %al
    movw $0x3c4, %dx
    cmpb $0x70, %bl
    jb .Lvga_indexed_1
    movw $0x3ce, %dx
.Lvga_indexed_1:
    outw %ax, %dx
    movw %si, %dx
    jmp next

vga_port:
    movw %dx, %si
    movb %al, %cl
    andb $0x3f, %al
    movb %ah, %bl
    xorb %ah, %ah
    addw $VGA_PORTS, %ax
    movw %ax, %dx
    cmpb $0xc0, %cl
    jae .Lvga_port_1
    inb %dx, %al
    movw %si, %dx
    outw %ax, %dx
    jmp next
.Lvga_port_1:
    movb %bl, %al
    outb %al, %dx
    movw %si, %dx
    jmp next

op_done:
    retw

op_registers:
    movw (%bp), %ax
    outw %ax, %dx
    movw 2(%bp), %ax
    outw %ax, %dx
    movw 4(%bp), %ax
    outw %ax, %dx
    movw 6(%bp), %ax
    outw %ax, %dx
    movw 10(%bp), %ax
    outw %ax, %dx
    movw 12(%bp), %ax
    outw %ax, %dx
    movw 16(%bp), %ax
    outw %ax, %dx
    jmp next

// AH is the register's offset in the saved ones.
op_set_register:
    movb %ah, %bl
    xorb %bh, %bh
    movw %bx, %si
    inw %dx, %ax
    movw %ax, (%bp, %si)
    jmp next

// AH is an offset in the BIOS data area.
op_bda_read8:
    movb %ah, %bl
    xorb %bh, %bh
    movw $0x40, %ax
    movw %ax, %es
    movb %es:(%bx), %al
    outw %ax, %dx
    jmp next

op_bda_read16:
    movb %ah, %bl
    xorb %bh, %bh
    movw $0x40, %ax
    movw %ax, %es
    movw %es:(%bx), %ax
    outw %ax, %dx
    jmp next

op_bda_write8:
    movb %ah, %bl
    xorb %bh, %bh
    movw $0x40, %ax
    movw %ax, %es
    inw %dx, %ax
    movb %al, %es:(%bx)
    jmp next

op_bda_write16:
    movb %ah, %bl
    xorb %bh, %bh
    movw $0x40, %ax
    movw %ax, %es
    inw %dx, %ax
    movw %ax, %es:(%bx)
    jmp next

op_read8:
    inw %dx, %ax
    movw %ax, %es
    inw %dx, %ax
    movw %ax, %bx
    movb %es:(%bx), %al
    outw %ax, %dx
    jmp next

op_read16:
    inw %dx, %ax
    movw %ax, %es
    inw %dx, %ax
    movw %ax, %bx
    movw %es:(%bx), %ax
    outw %ax, %dx
    jmp next

// AH is the value.
op_write8:
    movb %ah, %cl
    inw %dx, %ax
    movw %ax, %es
    inw %dx, %ax
    movw %ax, %bx
    movb %cl, %es:(%bx)
    jmp next

op_write16:
    inw %dx, %ax
    movw %ax, %es
    inw %dx, %ax
    movw %ax, %bx
    inw %dx, %ax
    movw %ax, %es:(%bx)
    jmp next

// AH is the value.
op_fill8:
    movb %ah, %bl
    inw %dx, %ax
    movw %ax, %es
    inw %dx, %ax
    movw %ax, %di
    inw %dx, %ax
    movw %ax, %cx
    movb %bl, %al
    rep stosb
    jmp next

op_fill16:
    inw %dx, %ax
    movw %ax, %es
    inw %dx, %ax
    movw %ax, %di
    inw %dx, %ax
    movw %ax, %cx
    inw %dx, %ax
    rep stosw
    jmp next

// AH says whether it runs backwards, from the last bytes, as an overlap needs.
op_copy:
    movb %ah, %bl
    inw %dx, %ax
    movw %ax, %es
    inw %dx, %ax
    movw %ax, %di
    inw %dx, %ax
    movw %ax, %ds
    inw %dx, %ax
    movw %ax, %si
    inw %dx, %ax
    movw %ax, %cx
    testb %bl, %bl
    jz .Lop_copy_1
    std
.Lop_copy_1:
    rep movsb
    cld
    jmp next

// A character map to plane 2: AH bytes of each character, from cells 32 bytes
// apart. Its count is of characters.
op_font:
    movb %ah, %bl
    xorb %bh, %bh
    inw %dx, %ax
    movw %ax, %es
    inw %dx, %ax
    movw %ax, %di
    inw %dx, %ax
    movw %ax, %ds
    inw %dx, %ax
    movw %ax, %si
    inw %dx, %ax
.Lop_font_1:
    movw %bx, %cx
    rep movsb
    addw $32, %di
    subw %bx, %di
    decw %ax
    jnz .Lop_font_1
    jmp next

// Bytes to a port, two in each word from the hypervisor, the count being of
// words.
op_out_block:
    inw %dx, %ax
    movw %ax, %bx
    inw %dx, %ax
    movw %ax, %cx
.Lop_out_block_1:
    inw %dx, %ax
    movw %dx, %si
    movw %bx, %dx
    outb %al, %dx
    movb %ah, %al
    outb %al, %dx
    movw %si, %dx
    decw %cx
    jnz .Lop_out_block_1
    jmp next

// Words from the hypervisor to memory, the count being of words.
op_write_block:
    inw %dx, %ax
    movw %ax, %es
    inw %dx, %ax
    movw %ax, %di
    inw %dx, %ax
    movw %ax, %cx
.Lop_write_block_1:
    inw %dx, %ax
    stosw
    decw %cx
    jnz .Lop_write_block_1
    jmp next

// Words from memory to the hypervisor, the count being of words.
op_read_block:
    inw %dx, %ax
    movw %ax, %ds
    inw %dx, %ax
    movw %ax, %si
    inw %dx, %ax
    movw %ax, %cx
.Lop_read_block_1:
    lodsw
    outw %ax, %dx
    decw %cx
    jnz .Lop_read_block_1
    jmp next

// The DAC's colours from the ROM, at the first word's offset, of the second's
// count of bytes, and black for the rest of its 768 bytes.
op_dac_rom:
    inw %dx, %ax
    movw %ax, %si
    inw %dx, %ax
    movw %ax, %cx
    movw %dx, %bx
    movw $0x3c9, %dx
    movw $768, %di
    subw %cx, %di
.Lop_dac_rom_1:
    movb %cs:(%si), %al
    incw %si
    outb %al, %dx
    loop .Lop_dac_rom_1
    xorb %al, %al
    movw %di, %cx
    jcxz .Lop_dac_rom_3
.Lop_dac_rom_2:
    outb %al, %dx
    loop .Lop_dac_rom_2
.Lop_dac_rom_3:
    movw %bx, %dx
    jmp next

// The upper half of ECX, for the one call with a 32-bit register, which only
// comes from callers on a 386 or later: movl %ecx, %eax, then shrl $16, %eax.
op_ecx_high:
.byte 0x66, 0x89, 0xc8
.byte 0x66, 0xc1, 0xe8, 0x10
    outw %ax, %dx
    jmp next

// In vgabios.c's order of OP_ values.
operations:
.word op_done - vgabios_stub
.word next - vgabios_stub
.word op_registers - vgabios_stub
.word op_set_register - vgabios_stub
.word op_bda_read8 - vgabios_stub
.word op_bda_read16 - vgabios_stub
.word op_bda_write8 - vgabios_stub
.word op_bda_write16 - vgabios_stub
.word op_read8 - vgabios_stub
.word op_read16 - vgabios_stub
.word op_write8 - vgabios_stub
.word op_write16 - vgabios_stub
.word op_fill8 - vgabios_stub
.word op_fill16 - vgabios_stub
.word op_copy - vgabios_stub
.word op_font - vgabios_stub
.word op_out_block - vgabios_stub
.word op_write_block - vgabios_stub
.word op_read_block - vgabios_stub
.word op_ecx_high - vgabios_stub
.word op_dac_rom - vgabios_stub

.globl vgabios_stub_end
vgabios_stub_end:
