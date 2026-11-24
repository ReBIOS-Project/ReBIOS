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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#ifndef VIRT__MMIO_H
#define VIRT__MMIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/vcpu.h>

// A device the hypervisor emulates at guest-physical addresses, which it keeps
// unmapped, or mapped without the access it emulates.
struct mmio_ops {
    // Accesses of 1, 2, 4, or 8 bytes, naturally aligned, offset into the
    // region. Reads may have effects, as the VGA's latches loading.
    uint64_t (*read)(struct vcpu *v, void *ctx, uint64_t offset, size_t size);
    void (*write)(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value);
    // Whether its reads, and its writes, stay within the hypervisor, reaching
    // none of the host's devices: an instruction that the hypervisor does not
    // carry out runs against the region, single-stepped, only where each
    // access it makes there does.
    bool reads_virtual;
    bool writes_virtual;
    // Whether writes go nowhere, so that INS may be emulated into it: the
    // processor may have read the port already.
    bool writes_ignored;
    // Whether an access to it begins a run (virt/run.c).
    bool runs;
    // Whether reads reach the memory it maps, which runs then read too.
    bool reads_mapped;
    // Optional, for a region that stands in front of memory only until the
    // guest reaches it: called at a fault on the region, before anything of
    // the instruction is carried out, true where the region is gone, so that
    // the instruction runs again natively.
    bool (*fault)(struct vcpu *v, void *ctx, uint64_t offset);
    // Optional, for repeated string instructions: count accesses of size bytes
    // each, the first at offset and each next one step bytes on, all within
    // the region, with the same effects as read() and write() making them a
    // byte at a time in ascending order. Element n is at data + n * size, or
    // is all at data where same is true. A move reads each element, then
    // writes it.
    void (*read_string)(
        struct vcpu *v,
        void *ctx,
        uint64_t offset,
        size_t size,
        int64_t step,
        uint8_t *data,
        size_t count
    );
    void (*write_string)(
        struct vcpu *v,
        void *ctx,
        uint64_t offset,
        size_t size,
        int64_t step,
        const uint8_t *data,
        bool same,
        size_t count
    );
    void (*move_string)(
        struct vcpu *v,
        void *ctx,
        uint64_t source,
        uint64_t destination,
        size_t size,
        int64_t step,
        size_t count
    );
};

// What the guest reaches where nothing decodes: reads of all ones, writes
// dropped, as other regions' writes may be too.
extern const struct mmio_ops mmio_absent_ops;
void mmio_absent_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value);

// Emulates a page-aligned region, for every vCPU: its number for mmio_move(),
// or -1 where there are too many.
int mmio_register(uintptr_t base, size_t size, const struct mmio_ops *ops, void *ctx);
// Moves a region, nowhere where size is 0, as when the BAR it is part of
// moves.
void mmio_move(int region, uintptr_t base, size_t size);

// Where a region was when an access found it, the access's offsets taken from
// there, whatever moves it meanwhile.
struct mmio_place {
    const void *region;
    uint64_t base;
    uint64_t size;
};

// A host register at hpa, which the hypervisor maps, as a region's access
// reaches it: size 1, 2, 4, or 8.
uint64_t mmio_read_host(uintptr_t hpa, size_t size);
void mmio_write_host(uintptr_t hpa, size_t size, uint64_t value);

// Guest memory at a linear address as the guest's instructions reach it: RAM,
// emulated regions, or nothing. Accesses are of at most 8 bytes; they return
// false where the access faulted, having raised the fault.
bool mmio_read_linear(struct vcpu *v, uintptr_t linear, void *buf, size_t size);
bool mmio_write_linear(struct vcpu *v, uintptr_t linear, const void *buf, size_t size);
// The same at a guest-physical address, within a page.
void mmio_phys_access(struct vcpu *v, uintptr_t gpa, void *buf, size_t len, bool write);

struct insn;

// struct emulate_mem's string() for guest memory: runs of repeated MOVS and
// STOS whose operands are regions that take string accesses, or RAM, and of
// OUTS from RAM.
size_t mmio_string(
    struct vcpu *v,
    const struct insn *insn,
    size_t size,
    int64_t step,
    size_t count
);

// The most bytes of elements that a run takes, which a buffer on the stack
// holds.
#define MMIO_RUN_BYTES 4096

// Runs that ReBIOS carries out in guest memory itself, as REP MOVS and REP
// STOS do, for the video BIOS's calls (virt/vgabios.c) and for the guest's own
// instructions (virt/run.c): count elements of size bytes, the first at linear
// address to, and from, and each next step bytes on. How many elements from
// the first a run may take: as far as a page and MMIO_RUN_BYTES allow, 0 where
// the first crosses a page.
size_t mmio_run_room(uintptr_t linear, size_t size, int64_t step, size_t count);
// Moves within a region that takes string moves, and stores, of data's
// elements, or of its first for every element where same is true, to a region
// that takes string stores: the whole run at once, or nothing, false, where it
// is elsewhere or breakpoints are armed, for the caller to make each access.
bool mmio_move_run(
    struct vcpu *v,
    uintptr_t to,
    uintptr_t from,
    size_t size,
    int64_t step,
    size_t count
);
bool mmio_store_run(
    struct vcpu *v,
    uintptr_t to,
    const uint8_t *data,
    bool same,
    size_t size,
    int64_t step,
    size_t count
);

// What the page at gpa is to a run (virt/run.c), for reads, or for writes
// where write is true: memory that the guest has mapped write-back, as RAM and
// the ROMs below 1 MiB are, and that only RAM is for writes, its host address
// in *host, which it marks written for writes; a region that begins runs,
// holding all of the page, where *place says; or anything else.
#define MMIO_PLACE_OTHER 0
#define MMIO_PLACE_RAM 1
#define MMIO_PLACE_RUN 2
int mmio_page(uintptr_t gpa, bool write, uint8_t **host, struct mmio_place *place);
// An access of len bytes within a page of the place mmio_page() gave, of
// value's low bytes, or reading what it returns.
uint64_t mmio_region_access(
    struct vcpu *v,
    const struct mmio_place *place,
    uintptr_t gpa,
    size_t len,
    bool write,
    uint64_t value
);
// Runs within a page of that region, all at once: count elements of size
// bytes, the first at gpa, or to and from, and each next step bytes on. Stores
// of data's elements, or of its first for every element where same is true,
// loads into data, in the order the elements are reached, and moves within the
// region. False where the region takes no such runs, for the caller to make
// each access.
bool mmio_region_store(
    struct vcpu *v,
    const struct mmio_place *place,
    uintptr_t gpa,
    const uint8_t *data,
    bool same,
    size_t size,
    int64_t step,
    size_t count
);
bool mmio_region_load(
    struct vcpu *v,
    const struct mmio_place *place,
    uintptr_t gpa,
    uint8_t *data,
    size_t size,
    int64_t step,
    size_t count
);
bool mmio_region_move(
    struct vcpu *v,
    const struct mmio_place *place,
    uintptr_t to,
    uintptr_t from,
    size_t size,
    int64_t step,
    size_t count
);

// An EPT violation or a nested page fault at gpa, for an access other than a
// fetch: emulates the instruction that made it, where nothing decodes as
// memory that is not there, or single-steps it where the step reaches no more
// than its operand, within the hypervisor, else stops the machine. Returns
// false where gpa is mapped after all, as another vCPU may just have mapped
// it: the guest then runs the access again.
bool mmio_fault(struct vcpu *v, uintptr_t gpa);
// One for a fetch, of code there: where nothing decodes, the #UD of the all
// ones a PC reads there; elsewhere a panic, as the hypervisor runs no code
// from a device.
bool mmio_fetch(struct vcpu *v, uintptr_t gpa);

// The end of a single-step mmio_fault() set up for an instruction it does not
// emulate itself: the step's monitor trap, single-step trap, or fault.
void mmio_step_done(struct vcpu *v);

#endif
