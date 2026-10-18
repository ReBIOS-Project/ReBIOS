; SPDX-License-Identifier: GPL-2.0-only
;
; This file is part of ReBIOS.
; Copyright (C) 2026 Mintsuki and contributors
;
; ReBIOS is free software: you can redistribute it and/or modify it under the
; terms of the GNU General Public License version 2 as published by the Free
; Software Foundation.
;
; ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
; WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
; FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
; details.
;
; You should have received a copy of the GNU General Public License along
; with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

; Dumps, for each standard BIOS video mode, the VGA registers, the BIOS data
; area's video fields, and the DAC that the BIOS's mode set leaves, to COM1.
; Run under plain QEMU (SeaBIOS + QEMU's VGA BIOS) to record what a VGA BIOS
; programs. Its body is in the sectors after it.
bits 16
org 0x7c00

SECTORS equ 8
COM1 equ 0x3f8

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc .fail
    jmp 0:main
.fail:
    hlt
    jmp .fail

dap:
    db 16, 0
    dw SECTORS
    dw 0x7e00, 0
    dq 1

    times 510 - ($ - $$) db 0
    dw 0xaa55

main:
    mov si, modes
.next_mode:
    lodsb
    cmp al, 0xff
    je .done
    push si
    mov [cur_mode], al
    ; QEMU's VGA BIOS programs a mode through the CRT controller and
    ; attribute flip-flop ports of the mode before it, which miss the
    ; registers when monochrome and colour addressing change: the second
    ; set starts from the mode itself.
    xor ah, ah
    int 0x10
    mov al, [cur_mode]
    xor ah, ah
    int 0x10
    call dump
    pop si
    jmp .next_mode
.done:
    mov ax, 3
    int 0x10
    mov si, msg_done
    call puts
.halt:
    hlt
    jmp .halt

; Prints the current mode's registers.
dump:
    mov si, msg_mode
    call puts
    mov al, [cur_mode]
    call puthex
    mov si, msg_misc
    call puts
    mov dx, 0x3cc
    in al, dx
    mov [misc], al
    call puthex

    mov si, msg_seq
    call puts
    xor cx, cx
.seq:
    mov dx, 0x3c4
    mov al, cl
    out dx, al
    inc dx
    in al, dx
    call puthex_sp
    inc cx
    cmp cx, 5
    jb .seq

    mov si, msg_crtc
    call puts
    mov bx, 0x3b4
    test byte [misc], 1
    jz .mono
    mov bx, 0x3d4
.mono:
    mov [crtc], bx
    xor cx, cx
.crtc:
    mov dx, bx
    mov al, cl
    out dx, al
    inc dx
    in al, dx
    call puthex_sp
    inc cx
    cmp cx, 0x19
    jb .crtc

    mov si, msg_gc
    call puts
    xor cx, cx
.gc:
    mov dx, 0x3ce
    mov al, cl
    out dx, al
    inc dx
    in al, dx
    call puthex_sp
    inc cx
    cmp cx, 9
    jb .gc

    mov si, msg_ac
    call puts
    xor cx, cx
.ac:
    mov dx, [crtc]
    add dx, 6
    in al, dx
    mov dx, 0x3c0
    mov al, cl
    or al, 0x20
    out dx, al
    inc dx
    in al, dx
    call puthex_sp
    inc cx
    cmp cx, 0x15
    jb .ac
    mov dx, [crtc]
    add dx, 6
    in al, dx
    mov dx, 0x3c0
    mov al, 0x20
    out dx, al

    mov si, msg_bda
    call puts
    push ds
    mov ax, 0x40
    mov ds, ax
    mov si, 0x49
    mov cx, 0x8b - 0x49
.bda:
    lodsb
    call puthex_sp
    loop .bda
    pop ds

    mov si, msg_dac
    call puts
    mov dx, 0x3c6
    in al, dx
    call puthex_sp
    mov dx, 0x3c7
    xor al, al
    out dx, al
    mov dx, 0x3c9
    mov cx, 768
.dac:
    in al, dx
    call puthex
    loop .dac
    mov si, msg_nl
    call puts
    ret

puts:
    lodsb
    test al, al
    jz .end
    call putc
    jmp puts
.end:
    ret

puthex_sp:
    call puthex
    mov al, ' '
    jmp putc

puthex:
    push ax
    shr al, 4
    call .nibble
    pop ax
    and al, 0x0f
.nibble:
    add al, '0'
    cmp al, '9'
    jbe putc
    add al, 'a' - '0' - 10
putc:
    push dx
    push ax
    mov dx, COM1 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    mov dx, COM1
    out dx, al
    pop dx
    ret

modes: db 0, 1, 2, 3, 4, 5, 6, 7, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0xff
cur_mode: db 0
misc: db 0
crtc: dw 0
msg_mode: db 'MODE ', 0
msg_misc: db 13, 10, 'MISC ', 0
msg_seq: db 13, 10, 'SEQ ', 0
msg_crtc: db 13, 10, 'CRTC ', 0
msg_gc: db 13, 10, 'GC ', 0
msg_ac: db 13, 10, 'AC ', 0
msg_bda: db 13, 10, 'BDA ', 0
msg_dac: db 13, 10, 'DAC ', 0
msg_nl: db 13, 10, 0
msg_done: db 'VGADUMP DONE', 13, 10, 0

    times 512 * (SECTORS + 1) - ($ - $$) db 0
