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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/guest_mem.h>
#include <memory.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/mmio.h>
#include <virt/nsvm.h>
#include <virt/nvmx.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vmx.h>

#define MSR_IA32_PKRS 0x6e1
#define CPUID_1_EDX_PSE36 ((uint32_t)1 << 17)
#define CPUID_80000001_EDX_1G_PAGES ((uint32_t)1 << 26)

// The deepest walk has 5 levels, PAE paging's PDPTE besides.
#define MAX_LEVELS 5

static bool guest_phys_access(uintptr_t gpa, void *buf, size_t len, bool write) {
    uint8_t *p = buf;
    while (len != 0) {
        uintptr_t hpa;
        uint32_t prot;
        if (!gpt_translate(gpa, &hpa, &prot, NULL) || (prot & (write ? GPT_W : GPT_R)) == 0) {
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

bool guest_mem_phys_read(uintptr_t gpa, void *buf, size_t len) {
    return guest_phys_access(gpa, buf, len, false);
}

bool guest_mem_phys_write(uintptr_t gpa, const void *buf, size_t len) {
    return guest_phys_access(gpa, (void *)buf, len, true);
}

// gpa as the processor reaches it for an access, given as an EPT violation's
// qualification would, and whether the access is a user-mode one, which those
// to page tables are for nested paging (APM 24593 rev. 3.46, 15.25.5): through
// the guest's EPT or nested page tables where the vCPU runs the guest's guest,
// else past the A20 gate.
static bool reach(
    struct vcpu *v, uintptr_t gpa, uint64_t access, bool user, uintptr_t linear, uint64_t *out,
    uint32_t *pf_error
) {
    if (nvmx_ept_active(v)) {
        if (!nvmx_ept_translate(v, gpa, access, linear, out)) {
            *pf_error = GUEST_MEM_PF_ERROR_EXIT;
            return false;
        }
        return true;
    }
    if (nsvm_npt_active(v)) {
        bool walk = (access & EPT_QUAL_LINEAR) != 0 && (access & EPT_QUAL_TRANSLATION) == 0;
        bool translated = nsvm_npt_translate(
            v, gpa, (access & EPT_QUAL_WRITE) != 0, (access & EPT_QUAL_FETCH) != 0, walk, user, out
        );
        if (!translated) {
            *pf_error = GUEST_MEM_PF_ERROR_EXIT;
            return false;
        }
        return true;
    }
    *out = gpt_cpu_gpa(gpa);
    return true;
}

static bool read_entry(
    struct vcpu *v, uintptr_t gpa, uintptr_t linear, uint64_t *entry, bool wide, uint32_t *pf_error
) {
    bool reached = reach(v, gpa, EPT_QUAL_READ | EPT_QUAL_LINEAR, true, linear, &gpa, pf_error);
    if (!reached) {
        return false;
    }
    if (wide) {
        return guest_mem_phys_read(gpa, entry, 8);
    }
    uint32_t e;
    if (!guest_mem_phys_read(gpa, &e, 4)) {
        return false;
    }
    *entry = e;
    return true;
}

// Sets the accessed flag, and dirty where given, in an entry the walk found as
// it was: false where the entry changed meanwhile, for the walk to start
// again. Entries the guest cannot write are left as they are.
static bool mark_entry(
    struct vcpu *v, uintptr_t gpa, uintptr_t linear, uint64_t was, uint64_t set, bool wide,
    uint32_t *pf_error
) {
    uintptr_t hpa;
    uint32_t prot;
    bool reached = reach(v, gpa, EPT_QUAL_WRITE | EPT_QUAL_LINEAR, true, linear, &gpa, pf_error);
    if (!reached) {
        return false;
    }
    if (!gpt_translate(gpa, &hpa, &prot, NULL) || (prot & GPT_W) == 0) {
        return true;
    }
    if (wide) {
        uint64_t expected = was;
        return atomic_compare_exchange_strong_explicit(
            (_Atomic(uint64_t) *)(uintptr_t)hpa, &expected, was | set, memory_order_seq_cst,
            memory_order_seq_cst
        );
    }
    uint32_t expected = (uint32_t)was;
    return atomic_compare_exchange_strong_explicit(
        (_Atomic(uint32_t) *)(uintptr_t)hpa, &expected, (uint32_t)(was | set),
        memory_order_seq_cst, memory_order_seq_cst
    );
}

// The guest's PKRU, which the processor keeps while the hypervisor runs, but
// reads only with CR4.PKE set: the hypervisor's pages being supervisor-mode
// ones, PKRU never governs them.
static uint32_t guest_pkru(void) {
    uintptr_t cr4 = read_cr4();
    write_cr4(cr4 | CR4_PKE);
    uint32_t pkru;
    uint32_t edx;
    asm volatile ("rdpkru" : "=a"(pkru), "=d"(edx) : "c"(0));
    write_cr4(cr4);
    return pkru;
}

// As Linux 7.3's KVM finds them (mmu.c, __reset_rsvds_bits_mask()), but for
// 32-bit paging's 4 MiB pages, which reserve bit 21 and the address bits past
// MAXPHYADDR, up to 40, alone.
uint64_t guest_mem_pte_reserved(bool wide, bool pae, size_t level, bool large, bool nx) {
    static unsigned int phys_bits;
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
        unsigned int bits = phys_bits > 40 ? 40 : phys_bits;
        unsigned int low = pse36 && bits > 32 ? 13 + (bits - 32) : 13;
        return large ? (((uint64_t)1 << 22) - 1) & ~(((uint64_t)1 << low) - 1) : 0;
    }
    // Address bits past MAXPHYADDR, up to 51 (62 with PAE paging), and NX
    // where IA32_EFER.NXE is clear.
    uint64_t high = ~(((uint64_t)1 << phys_bits) - 1)
                  & ((pae ? (uint64_t)1 << 63 : (uint64_t)1 << 52) - 1);
    if (!nx) {
        high |= (uint64_t)1 << 63;
    }
    if (pae && level == 2) {
        return high | ((uint64_t)1 << 63) | (uint64_t)0x1e6;
    }
    switch (level) {
        case 0: {
            return high;
        }
        case 1: {
            return high | (large ? (uint64_t)0x1fe000 : 0);
        }
        case 2: {
            return high | (gb_pages ? 0 : PTE_PS) | (large ? (uint64_t)0x3fffe000 : 0);
        }
        default: {
            // PML4Es and PML5Es map no pages; AMD's reserve bit 8 too.
            return high | PTE_PS | (virt_vmx ? 0 : (uint64_t)1 << 8);
        }
    }
}

bool guest_mem_pdpte_valid(uint64_t pdpte) {
    return (pdpte & PTE_P) == 0
        || (pdpte & guest_mem_pte_reserved(true, true, 2, false, true)) == 0;
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
static bool linear_to_phys(
    struct vcpu *v, uintptr_t linear, bool write, bool fetch, bool user, bool implicit,
    uint64_t *gpa, uint32_t *pf_error
) {
    uint64_t cr0 = vcpu_cr0(v);
    uint64_t cr4 = vcpu_cr4(v);
    uint64_t efer = vcpu_efer(v);

    // The fetch flag is defined only with SMEP or with PAE paging's NX.
    bool fetch_flag = fetch
                   && ((cr4 & CR4_SMEP) != 0 || ((cr4 & CR4_PAE) != 0 && (efer & EFER_NXE) != 0));
retry: *pf_error = (write ? PF_ERROR_W : 0) | (user ? PF_ERROR_U : 0)
                 | (fetch_flag ? PF_ERROR_I : 0);

    // Without paging, every linear address is a user-mode one, writable and
    // executable (SDM 325462-093 Vol. 3C, Table 30-7).
    if ((cr0 & CR0_PG) == 0) {
        uint64_t access = data_access(write, fetch) | EPT_QUAL_USER | EPT_QUAL_WRITABLE;
        return reach(v, linear & 0xffffffff, access, user, linear, gpa, pf_error);
    }

    size_t levels;
    bool wide;
    bool pae = false;
    uintptr_t table;
    if ((efer & EFER_LMA) != 0) {
        levels = (cr4 & CR4_LA57) != 0 ? 5 : 4;
        wide = true;
        table = vcpu_cr3(v) & PTE_ADDRESS;
    } else if ((cr4 & CR4_PAE) != 0) {
        // The PDPT, of 4 entries, is reached through CR3 bits 31:5; with EPT,
        // VMX keeps the PDPTE registers in the VMCS.
        size_t i = (linear >> 30) & 3;
        uint64_t pdpte;
        if (virt_vmx) {
            pdpte = vmx_read(VMCS_GUEST_PDPTE0 + 2 * i);
        } else {
            bool fetched = read_entry(
                v, (vcpu_cr3(v) & 0xffffffe0) + i * 8, linear, &pdpte, true, pf_error
            );
            if (!fetched) {
                return false;
            }
        }
        if ((pdpte & PTE_P) == 0) {
            return false;
        }
        if ((pdpte & guest_mem_pte_reserved(true, true, 2, false, (efer & EFER_NXE) != 0)) != 0) {
            *pf_error |= PF_ERROR_P | PF_ERROR_RSVD;
            return false;
        }
        levels = 2;
        wide = true;
        pae = true;
        table = pdpte & PTE_ADDRESS;
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
    unsigned int path = 0;
    for (size_t level = levels; level-- > 0; ) {
        unsigned int shift = wide ? 12 + 9 * level : 12 + 10 * level;
        size_t index = (linear >> shift) & (wide ? 511 : 1023);
        uintptr_t entry_gpa = table + index * (wide ? 8 : 4);
        uint64_t entry;
        if (!read_entry(v, entry_gpa, linear, &entry, wide, pf_error)) {
            return false;
        }
        if ((entry & PTE_P) == 0) {
            return false;
        }
        bool large = level > 0 && (entry & PTE_PS) != 0 && (wide || (cr4 & CR4_PSE) != 0);
        if ((entry & guest_mem_pte_reserved(wide, pae, (unsigned int)level, large, nx)) != 0) {
            *pf_error |= PF_ERROR_P | PF_ERROR_RSVD;
            return false;
        }
        entries[path] = entry;
        entry_gpas[path++] = entry_gpa;
        writable &= (entry & PTE_W) != 0;
        user_ok &= (entry & PTE_U) != 0;
        if (wide && nx && (entry & PTE_NX) != 0) {
            executable = false;
        }

        if (level == 0 || large) {
            uint64_t page_mask = ((uint64_t)1 << shift) - 1;
            uintptr_t base;
            if (wide) {
                base = entry & PTE_ADDRESS & ~page_mask;
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
            if (write && !writable && (user || (cr0 & CR0_WP) != 0)) {
                return false;
            }
            if (fetch && !executable) {
                return false;
            }
            if (fetch && !user && user_ok && (cr4 & CR4_SMEP) != 0) {
                return false;
            }
            if (!fetch && !user && user_ok && (cr4 & CR4_SMAP) != 0
             && (implicit || (vcpu_rflags(v) & RFLAGS_AC) == 0)) {
                return false;
            }
            // Protection keys, of 4-level and 5-level paging alone, govern
            // data accesses (SDM 325462-093 Vol. 3A, 5.6.2).
            if (!fetch && (efer & EFER_LMA) != 0 && (cr4 & (user_ok ? CR4_PKE : CR4_PKS)) != 0) {
                uint64_t rights = user_ok ? guest_pkru() : rdmsr(MSR_IA32_PKRS);
                rights >>= ((entry >> PTE_PKEY_SHIFT) & 0xf) * 2;
                bool write_disabled = (rights & 2) != 0 && write
                                   && ((user_ok && user) || (cr0 & CR0_WP) != 0);
                if ((rights & 1) != 0 || write_disabled) {
                    *pf_error |= PF_ERROR_PK;
                    return false;
                }
            }
            // The accessed flags, and the dirty one of a page written.
            for (size_t i = 0; i < path; i++) {
                bool leaf = i == path - 1;
                uint64_t set = PTE_A | (leaf && write ? PTE_D : 0);
                bool unmarked = (entries[i] & set) != set
                             && !mark_entry(
                                    v, entry_gpas[i], linear, entries[i], set, wide, pf_error
                                );
                if (unmarked) {
                    if ((*pf_error & GUEST_MEM_PF_ERROR_EXIT) != 0) {
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
            return reach(v, base | (linear & page_mask), access, user, linear, gpa, pf_error);
        }
        table = wide ? entry & PTE_ADDRESS : entry & 0xfffff000;
    }
    return false;
}

bool guest_mem_linear_to_phys(
    struct vcpu *v, uintptr_t linear, bool write, bool fetch, uint64_t *gpa, uint32_t *pf_error
) {
    struct vcpu_segment ss;
    vcpu_get_seg(v, VCPU_SEG_SS, &ss);
    bool user = ((ss.attrib >> 5) & 3) == 3;
    return linear_to_phys(v, linear, write, fetch, user, false, gpa, pf_error);
}

// What the processor reads and writes for itself, such as descriptor tables
// and the TSS, it does with supervisor rights at any CPL.
static bool system_access(
    struct vcpu *v, uintptr_t linear, void *buf, size_t len, bool write, uint32_t *pf_error,
    uint64_t *pf_linear
) {
    uint8_t *p = buf;
    while (len != 0) {
        uintptr_t gpa;
        bool mapped = linear_to_phys(v, linear, write, false, false, true, &gpa, pf_error);
        if (!mapped) {
            *pf_linear = linear;
            return false;
        }
        size_t chunk = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (chunk > len) {
            chunk = len;
        }
        mmio_phys_access(v, gpa, p, (unsigned int)chunk, write);
        linear += chunk;
        p += chunk;
        len -= chunk;
    }
    return true;
}

bool guest_mem_system_read(
    struct vcpu *v, uintptr_t linear, void *buf, size_t len, uint32_t *pf_error,
    uint64_t *pf_linear
) {
    return system_access(v, linear, buf, len, false, pf_error, pf_linear);
}

bool guest_mem_system_write(
    struct vcpu *v, uintptr_t linear, const void *buf, size_t len, uint32_t *pf_error,
    uint64_t *pf_linear
) {
    return system_access(v, linear, (void *)buf, len, true, pf_error, pf_linear);
}

bool guest_mem_linear_read(struct vcpu *v, uintptr_t linear, void *buf, size_t len, bool fetch) {
    uint8_t *p = buf;
    while (len != 0) {
        uintptr_t gpa;
        uint32_t pf_error;
        if (!guest_mem_linear_to_phys(v, linear, false, fetch, &gpa, &pf_error)) {
            return false;
        }
        size_t chunk = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (chunk > len) {
            chunk = len;
        }
        if (!guest_mem_phys_read(gpa, p, chunk)) {
            return false;
        }
        linear += chunk;
        p += chunk;
        len -= chunk;
    }
    return true;
}
