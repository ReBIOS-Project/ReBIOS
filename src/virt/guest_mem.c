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

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <memory.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/guest_mem.h>
#include <virt/nvmx.h>
#include <virt/vcpu.h>
#include <virt/vmx.h>

#define PTE_P (1ull << 0)
#define PTE_W (1ull << 1)
#define PTE_U (1ull << 2)
#define PTE_A (1ull << 5)
#define PTE_D (1ull << 6)
#define PTE_PS (1ull << 7)
#define PTE_NX (1ull << 63)
#define PTE_ADDR 0x000ffffffffff000ull
#define PTE_PKEY_SHIFT 59
#define MSR_IA32_PKRS 0x6e1
#define CPUID_1_EDX_PSE36 (1u << 17)
#define CPUID_80000001_EDX_1G_PAGES (1u << 26)

// The deepest walk has 5 levels, PAE paging's PDPTE besides.
#define MAX_LEVELS 5

static bool guest_phys_access(uint64_t gpa, void *buf, size_t len, bool write) {
    uint8_t *p = buf;
    while (len != 0) {
        uint64_t hpa;
        uint32_t prot;
        if (!gpt_translate(gpa, &hpa, &prot, NULL) || !(prot & (write ? GPT_W : GPT_R))) {
            return false;
        }
        size_t chunk = PAGE_SIZE - (gpa & (PAGE_SIZE - 1));
        if (chunk > len) {
            chunk = len;
        }
        if (write) {
            memcpy((void *)(uintptr_t)hpa, p, chunk);
        } else {
            memcpy(p, (void *)(uintptr_t)hpa, chunk);
        }
        gpa += chunk;
        p += chunk;
        len -= chunk;
    }
    return true;
}

bool guest_phys_read(uint64_t gpa, void *buf, size_t len) {
    return guest_phys_access(gpa, buf, len, false);
}

bool guest_phys_write(uint64_t gpa, const void *buf, size_t len) {
    return guest_phys_access(gpa, (void *)buf, len, true);
}

// gpa as the processor reaches it for an access, given as an EPT violation's
// qualification would: through the guest's EPT where the vCPU runs the
// guest's guest, else past the A20 gate.
static bool reach(struct vcpu *v, uint64_t gpa, uint64_t access, uint64_t linear, uint64_t *out,
                  uint32_t *pf_error) {
    if (nvmx_ept_active(v)) {
        if (!nvmx_ept_translate(v, gpa, access, linear, out)) {
            *pf_error = PF_ERROR_EXIT;
            return false;
        }
        return true;
    }
    *out = gpt_cpu_gpa(gpa);
    return true;
}

static bool read_entry(struct vcpu *v, uint64_t gpa, uint64_t linear, uint64_t *entry, bool wide,
                       uint32_t *pf_error) {
    if (!reach(v, gpa, EPT_QUAL_READ | EPT_QUAL_LINEAR, linear, &gpa, pf_error)) {
        return false;
    }
    if (wide) {
        return guest_phys_read(gpa, entry, 8);
    }
    uint32_t e;
    if (!guest_phys_read(gpa, &e, 4)) {
        return false;
    }
    *entry = e;
    return true;
}

// Sets the accessed flag, and dirty where given, in an entry the walk found as
// it was: false where the entry changed meanwhile, for the walk to start
// again. Entries the guest cannot write are left as they are.
static bool mark_entry(struct vcpu *v, uint64_t gpa, uint64_t linear, uint64_t was, uint64_t set,
                       bool wide, uint32_t *pf_error) {
    uint64_t hpa;
    uint32_t prot;
    if (!reach(v, gpa, EPT_QUAL_WRITE | EPT_QUAL_LINEAR, linear, &gpa, pf_error)) {
        return false;
    }
    if (!gpt_translate(gpa, &hpa, &prot, NULL) || !(prot & GPT_W)) {
        return true;
    }
    if (wide) {
        uint64_t expected = was;
        return __atomic_compare_exchange_n((uint64_t *)(uintptr_t)hpa, &expected, was | set, false,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    uint32_t expected = (uint32_t)was;
    return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)hpa, &expected,
                                       (uint32_t)(was | set), false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

// The guest's PKRU, which the processor keeps while the hypervisor runs, but
// reads only with CR4.PKE set: the hypervisor's pages being supervisor-mode
// ones, PKRU never governs them.
static uint32_t guest_pkru(void) {
    uintptr_t cr4 = read_cr4();
    write_cr4(cr4 | CR4_PKE);
    uint32_t pkru;
    uint32_t edx;
    __asm__ volatile("rdpkru" : "=a"(pkru), "=d"(edx) : "c"(0));
    write_cr4(cr4);
    return pkru;
}

// The bits an entry at level, 0 for the page table's, reserves where it maps a
// page or not (SDM 325462-093 Vol. 3A, Tables 5-4 to 5-20), as Linux 7.3's KVM
// finds them (mmu.c, __reset_rsvds_bits_mask()); but 32-bit paging's 4 MiB
// pages reserve bit 21 and the address bits past MAXPHYADDR, up to 40, alone.
// PAE paging's PDPTEs are level 2 with pae.
static uint64_t reserved_bits(bool wide, bool pae, unsigned level, bool large, bool nx) {
    static unsigned phys_bits;
    static bool gb_pages;
    static bool pse36;
    if (phys_bits == 0) {
        phys_bits = 36;
        if (cpuid(0x80000000, 0).eax >= 0x80000008) {
            phys_bits = cpuid(0x80000008, 0).eax & 0xff;
        }
        gb_pages = (cpuid(0x80000001, 0).edx & CPUID_80000001_EDX_1G_PAGES) != 0;
        pse36 = (cpuid(1, 0).edx & CPUID_1_EDX_PSE36) != 0;
    }
    if (!wide) {
        unsigned bits = phys_bits > 40 ? 40 : phys_bits;
        unsigned low = pse36 && bits > 32 ? 13 + (bits - 32) : 13;
        return large ? ((1ull << 22) - 1) & ~((1ull << low) - 1) : 0;
    }
    // Address bits past MAXPHYADDR, up to 51 (62 with PAE paging), and NX
    // where IA32_EFER.NXE is clear.
    uint64_t high = ~((1ull << phys_bits) - 1) & ((pae ? 1ull << 63 : 1ull << 52) - 1);
    if (!nx) {
        high |= 1ull << 63;
    }
    if (pae && level == 2) {
        return high | (1ull << 63) | 0x1e6ull;
    }
    switch (level) {
        case 0: {
            return high;
        }
        case 1: {
            return high | (large ? 0x1fe000ull : 0);
        }
        case 2: {
            return high | (gb_pages ? 0 : PTE_PS) | (large ? 0x3fffe000ull : 0);
        }
        default: {
            // PML4Es and PML5Es map no pages; AMD's reserve bit 8 too.
            return high | PTE_PS | (virt_vmx ? 0 : 1ull << 8);
        }
    }
}

static uint64_t data_access(bool write, bool fetch) {
    uint64_t access = EPT_QUAL_LINEAR | EPT_QUAL_TRANSLATION;
    if (write) {
        return access | EPT_QUAL_WRITE;
    }
    return access | (fetch ? EPT_QUAL_FETCH : EPT_QUAL_READ);
}

// The walk of SDM 325462-093 Vol. 3A, chapter 5, as Linux 7.3's KVM makes it
// (paging_tmpl.h, walk_addr_generic(); mmu.h, permission_fault()): reserved
// bits, the access rights, SMEP, SMAP, and protection keys checked, and the
// accessed and dirty flags set where the access goes ahead. An implicit access
// is a supervisor-mode one the processor makes for itself, as to descriptor
// tables, which SMAP cannot be lifted for.
static bool linear_to_phys(struct vcpu *v, uint64_t linear, bool write, bool fetch, bool user,
                           bool implicit, uint64_t *gpa, uint32_t *pf_error) {
    uint64_t cr0 = vcpu_cr0(v);
    uint64_t cr4 = vcpu_cr4(v);
    uint64_t efer = vcpu_efer(v);

    // The fetch flag is defined only with SMEP or with PAE paging's NX.
    bool fetch_flag = fetch && ((cr4 & CR4_SMEP) || ((cr4 & CR4_PAE) && (efer & EFER_NXE)));
retry:
    *pf_error = (write ? PF_ERROR_W : 0) | (user ? PF_ERROR_U : 0) | (fetch_flag ? PF_ERROR_I : 0);

    // Without paging, every linear address is a user-mode one, writable and
    // executable (SDM 325462-093 Vol. 3C, Table 30-7).
    if (!(cr0 & CR0_PG)) {
        uint64_t access = data_access(write, fetch) | EPT_QUAL_USER | EPT_QUAL_WRITABLE;
        return reach(v, linear & 0xffffffff, access, linear, gpa, pf_error);
    }

    int levels;
    bool wide;
    bool pae = false;
    uint64_t table;
    if (efer & EFER_LMA) {
        levels = (cr4 & CR4_LA57) ? 5 : 4;
        wide = true;
        table = vcpu_cr3(v) & PTE_ADDR;
    } else if (cr4 & CR4_PAE) {
        // The PDPT, of 4 entries, is reached through CR3 bits 31:5; with EPT,
        // VMX keeps the PDPTE registers in the VMCS.
        unsigned i = (linear >> 30) & 3;
        uint64_t pdpte;
        if (virt_vmx) {
            pdpte = vmx_read(VMCS_GUEST_PDPTE0 + 2 * i);
        } else if (!read_entry(v, (vcpu_cr3(v) & 0xffffffe0) + i * 8, linear, &pdpte, true,
                               pf_error)) {
            return false;
        }
        if (!(pdpte & PTE_P)) {
            return false;
        }
        if (pdpte & reserved_bits(true, true, 2, false, (efer & EFER_NXE) != 0)) {
            *pf_error |= PF_ERROR_P | PF_ERROR_RSVD;
            return false;
        }
        levels = 2;
        wide = true;
        pae = true;
        table = pdpte & PTE_ADDR;
    } else {
        levels = 2;
        wide = false;
        table = vcpu_cr3(v) & 0xfffff000;
        linear &= 0xffffffff;
    }

    bool writable = true;
    bool user_ok = true;
    bool executable = true;
    bool nx = (efer & EFER_NXE) != 0;
    uint64_t entries[MAX_LEVELS];
    uint64_t entry_gpas[MAX_LEVELS];
    unsigned path = 0;
    for (int level = levels - 1; level >= 0; level--) {
        unsigned shift = wide ? 12 + 9 * level : 12 + 10 * level;
        unsigned index = (linear >> shift) & (wide ? 511 : 1023);
        uint64_t entry_gpa = table + index * (wide ? 8 : 4);
        uint64_t entry;
        if (!read_entry(v, entry_gpa, linear, &entry, wide, pf_error)) {
            return false;
        }
        if (!(entry & PTE_P)) {
            return false;
        }
        bool large = level > 0 && (entry & PTE_PS) && (wide || (cr4 & CR4_PSE));
        if (entry & reserved_bits(wide, pae, (unsigned)level, large, nx)) {
            *pf_error |= PF_ERROR_P | PF_ERROR_RSVD;
            return false;
        }
        entries[path] = entry;
        entry_gpas[path++] = entry_gpa;
        writable &= (entry & PTE_W) != 0;
        user_ok &= (entry & PTE_U) != 0;
        if (wide && nx && (entry & PTE_NX)) {
            executable = false;
        }

        if (level == 0 || large) {
            uint64_t page_mask = (1ull << shift) - 1;
            uint64_t base;
            if (wide) {
                base = entry & PTE_ADDR & ~page_mask;
            } else if (large) {
                // PSE-36 puts physical address bits 39:32 in PDE bits 20:13.
                base = (entry & 0xffc00000) | ((entry >> 13) & 0xff) << 32;
            } else {
                base = entry & 0xfffff000;
            }

            *pf_error |= PF_ERROR_P;
            if (user && !user_ok) {
                return false;
            }
            if (write && !writable && (user || (cr0 & CR0_WP))) {
                return false;
            }
            if (fetch && !executable) {
                return false;
            }
            if (fetch && !user && user_ok && (cr4 & CR4_SMEP)) {
                return false;
            }
            if (!fetch && !user && user_ok && (cr4 & CR4_SMAP)
                && (implicit || !(vcpu_rflags(v) & RFLAGS_AC))) {
                return false;
            }
            // Protection keys, of 4-level and 5-level paging alone, govern data
            // accesses (SDM 325462-093 Vol. 3A, 5.6.2).
            if (!fetch && (efer & EFER_LMA) && (cr4 & (user_ok ? CR4_PKE : CR4_PKS))) {
                uint64_t rights = user_ok ? guest_pkru() : rdmsr(MSR_IA32_PKRS);
                rights >>= ((entry >> PTE_PKEY_SHIFT) & 0xf) * 2;
                bool write_disabled = (rights & 2) && write
                                      && ((user_ok && user) || (cr0 & CR0_WP));
                if ((rights & 1) || write_disabled) {
                    *pf_error |= PF_ERROR_PK;
                    return false;
                }
            }
            // The accessed flags, and the dirty one of a page written.
            for (unsigned i = 0; i < path; i++) {
                bool leaf = i == path - 1;
                uint64_t set = PTE_A | (leaf && write ? PTE_D : 0);
                if ((entries[i] & set) != set
                    && !mark_entry(v, entry_gpas[i], linear, entries[i], set, wide, pf_error)) {
                    if (*pf_error & PF_ERROR_EXIT) {
                        return false;
                    }
                    goto retry;
                }
            }
            uint64_t access = data_access(write, fetch);
            if (user_ok) {
                access |= EPT_QUAL_USER;
            }
            if (writable) {
                access |= EPT_QUAL_WRITABLE;
            }
            if (!executable) {
                access |= EPT_QUAL_XD;
            }
            return reach(v, base | (linear & page_mask), access, linear, gpa, pf_error);
        }
        table = wide ? entry & PTE_ADDR : entry & 0xfffff000;
    }
    return false;
}

bool guest_linear_to_phys(struct vcpu *v, uint64_t linear, bool write, bool fetch,
                          uint64_t *gpa, uint32_t *pf_error) {
    struct segment ss;
    vcpu_get_seg(v, SEG_SS, &ss);
    bool user = ((ss.attrib >> 5) & 3) == 3;
    return linear_to_phys(v, linear, write, fetch, user, false, gpa, pf_error);
}

// What the processor reads and writes for itself, such as descriptor tables
// and the TSS, it does with supervisor rights at any CPL.
static bool system_access(struct vcpu *v, uint64_t linear, void *buf, size_t len, bool write,
                          uint32_t *pf_error, uint64_t *pf_linear) {
    uint8_t *p = buf;
    while (len != 0) {
        uint64_t gpa;
        if (!linear_to_phys(v, linear, write, false, false, true, &gpa, pf_error)) {
            *pf_linear = linear;
            return false;
        }
        size_t chunk = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (chunk > len) {
            chunk = len;
        }
        bool ok = write ? guest_phys_write(gpa, p, chunk) : guest_phys_read(gpa, p, chunk);
        if (!ok) {
            *pf_error = 0;
            *pf_linear = linear;
            return false;
        }
        linear += chunk;
        p += chunk;
        len -= chunk;
    }
    return true;
}

bool guest_system_read(struct vcpu *v, uint64_t linear, void *buf, size_t len, uint32_t *pf_error,
                       uint64_t *pf_linear) {
    return system_access(v, linear, buf, len, false, pf_error, pf_linear);
}

bool guest_system_write(struct vcpu *v, uint64_t linear, const void *buf, size_t len,
                        uint32_t *pf_error, uint64_t *pf_linear) {
    return system_access(v, linear, (void *)buf, len, true, pf_error, pf_linear);
}

bool guest_linear_read(struct vcpu *v, uint64_t linear, void *buf, size_t len, bool fetch) {
    uint8_t *p = buf;
    while (len != 0) {
        uint64_t gpa;
        uint32_t pf_error;
        if (!guest_linear_to_phys(v, linear, false, fetch, &gpa, &pf_error)) {
            return false;
        }
        size_t chunk = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (chunk > len) {
            chunk = len;
        }
        if (!guest_phys_read(gpa, p, chunk)) {
            return false;
        }
        linear += chunk;
        p += chunk;
        len -= chunk;
    }
    return true;
}
