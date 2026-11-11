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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/vga_draw.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/time.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/simd.h>
#include <sys/x86.h>

// What each build of the kernels gets timed on: lines of a 1920-pixel display,
// of 1024 source pixels where scaled, and a text and a graphics line of 80
// character clocks.
#define BENCH_PIXELS 1920
#define BENCH_SOURCE 1024
#define BENCH_COLUMNS 80
#define BENCH_TILE 64
// How many times each kernel runs in a round, and the rounds, of which the
// fastest counts.
#define BENCH_RUNS 4
#define BENCH_ROUNDS 3

// The kernels that each level does better, none for SSE4.2's.
static const struct vga_draw_ops *const betters[SIMD_LEVELS] = {
    &vga_draw_sse2, &vga_draw_ssse3, &vga_draw_sse4_1, NULL, &vga_draw_avx, &vga_draw_avx2
};

// Each level's kernels: its own, else those of the level below.
static struct vga_draw_ops levels[SIMD_LEVELS];

// The buffers the kernels run on, the planes of a VGA frame, and the
// framebuffer's rows that the copies to it write back as they were.
static struct {
    uint8_t *a;
    uint8_t *b;
    uint8_t *dst;
    uint32_t *pixels;
    uint8_t *plane;
    uint32_t palette[256];
    struct vga_draw_scale scale;
    struct vga_draw_frame frame;
    uint64_t spread[256];
    uint8_t *fb;
    uint8_t *rows;
    size_t fb_bytes;
} bench;

static struct cpu *copier;

void vga_draw_filter_position(
    size_t i,
    size_t n,
    size_t count,
    uint32_t *index,
    uint32_t *weight
) {
    // (i + 1/2) * count / n - 1/2, in steps of 1 / VGA_DRAW_FILTER_ONE.
    uint64_t at = ((uint64_t)i * 2 + 1) * count * VGA_DRAW_FILTER_ONE / ((uint64_t)n * 2);
    at = at >= VGA_DRAW_FILTER_ONE / 2 ? at - VGA_DRAW_FILTER_ONE / 2 : 0;
    *index = (uint32_t)(at / VGA_DRAW_FILTER_ONE);
    *weight = (uint32_t)(at % VGA_DRAW_FILTER_ONE);
    if (*index + (size_t)1 >= count) {
        *index = (uint32_t)count - 1;
        *weight = 0;
    }
}

void vga_draw_scale_alloc(struct vga_draw_scale *scale, size_t columns) {
    size_t groups = columns / VGA_DRAW_GROUP + 1;
    scale->columns = 0;
    scale->xmap = page_alloc(page_count(columns * sizeof(uint32_t)));
    scale->xweight = page_alloc(page_count(columns));
    scale->weights = page_alloc(page_count(columns * 4 * sizeof(uint16_t)));
    scale->window = page_alloc(page_count(groups * sizeof(uint32_t)));
    scale->picks = page_alloc(page_count(groups * 4 * VGA_DRAW_GROUP));
    scale->fits = page_alloc(page_count(groups * sizeof(bool)));
}

// For each group of size columns: a window of as many source pixels from the
// first column's, and whether it holds every column's pixel, and the one after
// each where filtering, within the source line's width.
static void windows(
    struct vga_draw_scale *scale,
    size_t width,
    size_t size,
    uint32_t *window,
    uint8_t *picks,
    bool *fits
) {
    size_t extra = scale->filter ? 1 : 0;
    for (size_t g = 0; g < scale->columns / size; g++) {
        uint32_t base = scale->xmap[g * size];
        bool fit = base + size + extra <= width;
        for (size_t k = 0; k < size && fit; k++) {
            uint32_t at = scale->xmap[g * size + k] - base;
            if (at >= size) {
                fit = false;
                break;
            }
            for (size_t c = 0; c < 4; c++) {
                picks[(g * size + k) * 4 + c] = (uint8_t)(at * 4 + c);
            }
        }
        window[g] = base;
        fits[g] = fit;
    }
}

void vga_draw_scale_set(struct vga_draw_scale *scale, size_t width, size_t columns, bool filter) {
    scale->columns = columns;
    scale->filter = filter;
    for (size_t x = 0; x < columns; x++) {
        uint32_t weight = 0;
        if (filter) {
            vga_draw_filter_position(x, columns, width, &scale->xmap[x], &weight);
        } else {
            scale->xmap[x] = (uint32_t)((uint64_t)x * width / columns);
        }
        scale->xweight[x] = (uint8_t)weight;
        for (size_t c = 0; c < 4; c++) {
            scale->weights[x * 4 + c] = (uint16_t)weight;
        }
    }
    windows(scale, width, VGA_DRAW_GROUP, scale->window, scale->picks, scale->fits);
}

// The kernels' runs on the benchmark's buffers.

// A line that matches, as most do, and one that differs half way.
static void run_differ(const struct vga_draw_ops *ops) {
    size_t line = BENCH_PIXELS * 4;
    size_t first;
    size_t end;
    ops->differ(bench.a, bench.b, line, &first, &end);
    ops->differ(bench.a + line, bench.b + line, line, &first, &end);
}

static void run_copy(const struct vga_draw_ops *ops) {
    ops->copy(bench.dst, bench.a, BENCH_PIXELS * 4);
}

static void run_copy_fb(const struct vga_draw_ops *ops) {
    ops->copy_fb(bench.fb, bench.rows, bench.fb_bytes);
}

static void run_fill(const struct vga_draw_ops *ops) {
    ops->fill((uint32_t *)bench.dst, 0x123456, BENCH_PIXELS);
}

static void run_convert8(const struct vga_draw_ops *ops) {
    ops->convert8(bench.pixels, bench.a, BENCH_PIXELS, bench.palette);
}

static void run_convert15(const struct vga_draw_ops *ops) {
    ops->convert15(bench.pixels, bench.a, BENCH_PIXELS);
}

static void run_convert16(const struct vga_draw_ops *ops) {
    ops->convert16(bench.pixels, bench.a, BENCH_PIXELS);
}

static void run_convert24(const struct vga_draw_ops *ops) {
    ops->convert24(bench.pixels, bench.a, BENCH_PIXELS);
}

static void run_convert32(const struct vga_draw_ops *ops) {
    ops->convert32(bench.pixels, bench.a, BENCH_PIXELS);
}

static void run_scale(const struct vga_draw_ops *ops) {
    ops->scale(bench.pixels, (const uint32_t *)bench.a, &bench.scale, 0, BENCH_PIXELS);
}

static void run_blend(const struct vga_draw_ops *ops) {
    const uint32_t *a = (const uint32_t *)bench.a;
    const uint32_t *b = (const uint32_t *)bench.b;
    ops->blend(bench.pixels, a, b, 100, BENCH_PIXELS);
}

static void run_to_xbgr(const struct vga_draw_ops *ops) {
    ops->to_xbgr((uint32_t *)bench.dst, bench.pixels, BENCH_PIXELS);
}

static void run_to_rgb565(const struct vga_draw_ops *ops) {
    ops->to_rgb565((uint16_t *)bench.dst, bench.pixels, BENCH_PIXELS);
}

static void run_to_rgb555(const struct vga_draw_ops *ops) {
    ops->to_rgb555((uint16_t *)bench.dst, bench.pixels, BENCH_PIXELS);
}

static void run_to_bgr24(const struct vga_draw_ops *ops) {
    ops->to_bgr24(bench.dst, bench.pixels, BENCH_PIXELS);
}

static void run_transpose(const struct vga_draw_ops *ops) {
    ops->transpose(
        (uint32_t *)bench.dst, BENCH_TILE, (const uint32_t *)bench.a, BENCH_TILE, BENCH_TILE,
        BENCH_TILE
    );
}

static void run_text_line(const struct vga_draw_ops *ops) {
    struct vga_draw_scan scan = { BENCH_COLUMNS * 9, BENCH_COLUMNS, 0, 3, 9, 1, true, true };
    ops->text_line(bench.pixels, &bench.frame, &scan);
}

static void run_graphics_line(const struct vga_draw_ops *ops) {
    struct vga_draw_scan scan = { BENCH_COLUMNS * 8, BENCH_COLUMNS, 0, 0, 8, 1, true, true };
    ops->graphics_line(bench.pixels, &bench.frame, &scan);
}

struct kernel {
    const char *name;
    size_t offset;
    void (*run)(const struct vga_draw_ops *ops);
};

static const struct kernel kernels[] = {
    { "differ", offsetof(struct vga_draw_ops, differ), run_differ },
    { "copy", offsetof(struct vga_draw_ops, copy), run_copy },
    { "copy_fb", offsetof(struct vga_draw_ops, copy_fb), run_copy_fb },
    { "fill", offsetof(struct vga_draw_ops, fill), run_fill },
    { "convert8", offsetof(struct vga_draw_ops, convert8), run_convert8 },
    { "convert15", offsetof(struct vga_draw_ops, convert15), run_convert15 },
    { "convert16", offsetof(struct vga_draw_ops, convert16), run_convert16 },
    { "convert24", offsetof(struct vga_draw_ops, convert24), run_convert24 },
    { "convert32", offsetof(struct vga_draw_ops, convert32), run_convert32 },
    { "scale", offsetof(struct vga_draw_ops, scale), run_scale },
    { "blend", offsetof(struct vga_draw_ops, blend), run_blend },
    { "to_xbgr", offsetof(struct vga_draw_ops, to_xbgr), run_to_xbgr },
    { "to_rgb565", offsetof(struct vga_draw_ops, to_rgb565), run_to_rgb565 },
    { "to_rgb555", offsetof(struct vga_draw_ops, to_rgb555), run_to_rgb555 },
    { "to_bgr24", offsetof(struct vga_draw_ops, to_bgr24), run_to_bgr24 },
    { "transpose", offsetof(struct vga_draw_ops, transpose), run_transpose },
    { "text_line", offsetof(struct vga_draw_ops, text_line), run_text_line },
    { "graphics_line", offsetof(struct vga_draw_ops, graphics_line), run_graphics_line }
};

typedef void (*any_kernel)(void);

static any_kernel slot(const struct vga_draw_ops *ops, const struct kernel *k) {
    any_kernel fn;
    memcpy(&fn, (const uint8_t *)ops + k->offset, sizeof(any_kernel));
    return fn;
}

static void levels_set(void) {
    for (size_t level = 0; level < SIMD_LEVELS; level++) {
        if (level != 0) {
            levels[level] = levels[level - 1];
        }
        for (size_t i = 0; i < ARRAY_COUNT(kernels) && betters[level] != NULL; i++) {
            any_kernel fn = slot(betters[level], &kernels[i]);
            if (fn != NULL) {
                memcpy((uint8_t *)&levels[level] + kernels[i].offset, &fn, sizeof(any_kernel));
            }
        }
    }
}

// The level whose own kernel is the fastest of a kind on this processor, and
// its time, in TSC ticks. A level that has it from the level below is no other
// build.
static size_t fastest(const struct kernel *k, uint64_t *ticks) {
    size_t best = SIMD_SSE2;
    uint64_t best_ticks = UINT64_MAX;
    any_kernel timed = NULL;
    for (size_t level = 0; level < SIMD_LEVELS && simd_usable(level); level++) {
        if (slot(&levels[level], k) == timed) {
            continue;
        }
        timed = slot(&levels[level], k);
        uint64_t least = UINT64_MAX;
        for (size_t round = 0; round < BENCH_ROUNDS; round++) {
            uint64_t start = rdtsc();
            for (size_t i = 0; i < BENCH_RUNS; i++) {
                k->run(&levels[level]);
            }
            uint64_t taken = rdtsc() - start;
            if (taken < least) {
                least = taken;
            }
        }
        // A later level wins only by being faster: no wider registers for
        // nothing.
        if (least < best_ticks) {
            best = level;
            best_ticks = least;
        }
    }
    *ticks = best_ticks;
    return best;
}

static void pick(struct cpu *cpu) {
    struct vga_draw_ops *picked = kmalloc(sizeof(struct vga_draw_ops));
    size_t level = SIMD_SSE2;
    size_t top = SIMD_SSE2;
    for (size_t l = 0; l < SIMD_LEVELS; l++) {
        if (simd_usable(l)) {
            top = l;
        }
    }
    memcpy(bench.rows, bench.fb, bench.fb_bytes);
    simd_begin(top);
    for (size_t i = 0; i < ARRAY_COUNT(kernels); i++) {
        uint64_t ticks;
        size_t chosen = fastest(&kernels[i], &ticks);
        any_kernel fn = slot(&levels[chosen], &kernels[i]);
        memcpy((uint8_t *)picked + kernels[i].offset, &fn, sizeof(any_kernel));
        if (chosen > level) {
            level = chosen;
        }
        if (kernels[i].run == run_copy_fb) {
            cpu->draw_copy_ticks = ticks;
        }
    }
    simd_end();
    cpu->draw_ops = picked;
    cpu->draw_level = level;
    if (copier == NULL || cpu->draw_copy_ticks < copier->draw_copy_ticks) {
        copier = cpu;
    }
    uint64_t bytes = (uint64_t)bench.fb_bytes * BENCH_RUNS;
    uint64_t rate = cpu->draw_copy_ticks != 0
                  ? bytes * time_tsc_hz() / cpu->draw_copy_ticks >> 20
                  : 0;
    printf(
        "Processor %#x draws the emulated VGA with up to %s, copying to the framebuffer at %llu "
        "MiB/s\n", cpu->apic_id, simd_level_name(level), (unsigned long long)rate
    );
}

void vga_draw_init(uint8_t *fb, size_t pitch, size_t rows) {
    size_t line_pages = page_count(BENCH_PIXELS * 4 * 2);
    bench.fb = fb;
    bench.fb_bytes = pitch * rows;
    bench.rows = page_alloc(page_count(bench.fb_bytes));
    bench.a = page_alloc(line_pages);
    bench.b = page_alloc(line_pages);
    bench.dst = page_alloc(line_pages);
    bench.pixels = page_alloc(line_pages);
    bench.plane = page_alloc(page_count(0x10000));
    uint32_t seed = 0x2545f491;
    for (size_t i = 0; i < BENCH_PIXELS * 4 * 2; i++) {
        seed = seed * 1103515245 + 12345;
        bench.a[i] = (uint8_t)(seed >> 16);
        bench.b[i] = bench.a[i];
    }
    memcpy(bench.plane, bench.a, BENCH_PIXELS * 4 * 2);
    // The second line differs from the first half way.
    bench.b[BENCH_PIXELS * 2] ^= 1;
    for (size_t i = 0; i < 256; i++) {
        bench.palette[i] = (uint32_t)i * 0x010203;
        uint64_t spread = 0;
        for (size_t k = 0; k < 8; k++) {
            spread |= (uint64_t)((i >> (7 - k)) & 1) << (8 * k);
        }
        bench.spread[i] = spread;
    }
    vga_draw_scale_alloc(&bench.scale, BENCH_PIXELS);
    vga_draw_scale_set(&bench.scale, BENCH_SOURCE, BENCH_PIXELS, true);
    levels_set();
    struct vga_draw_frame *f = &bench.frame;
    for (size_t p = 0; p < 4; p++) {
        f->planes[p] = bench.plane;
    }
    f->cursor_shown = true;
    f->cursor_start = 13;
    f->cursor_end = 14;
    f->underline = 15;
    f->blink = true;
    f->line_graphics = true;
    f->plane_enable = 0x0f;
    f->spread = bench.spread;
    for (size_t i = 0; i < 256; i++) {
        f->palette[i] = bench.palette[i];
    }
    for (size_t i = 0; i < 16; i++) {
        f->ac_palette[i] = bench.palette[i * 4];
    }
    pick(this_cpu());
}

void vga_draw_cpu_init(void) {
    pick(this_cpu());
}

const struct vga_draw_ops *vga_draw_ops(void) {
    return this_cpu()->draw_ops;
}

size_t vga_draw_level(void) {
    return this_cpu()->draw_level;
}

struct cpu *vga_draw_copier(void) {
    return copier;
}
