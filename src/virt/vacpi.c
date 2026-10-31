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
#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/platform/types.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <virt/vacpi.h>
#include <csm/e820.h>
#include <hw/acpi.h>
#include <hw/pci_cfg.h>
#include <hw/pci_host.h>
#include <hw/power.h>
#include <lib/aml.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <memory.h>
#include <sys/mm.h>
#include <virt/gpt.h>
#include <virt/guest_mem.h>
#include <virt/hpet.h>
#include <virt/io.h>
#include <virt/irq.h>
#include <virt/layout.h>
#include <virt/mmio.h>
#include <virt/pci_plan.h>
#include <virt/vcpu.h>

// The mailbox the guest's AML calls the firmware's methods through: a header,
// argument slots, and the result's data, then a page whose writes trap.
#define MBX_FUNCTION 0x00
#define MBX_INDEX 0x04
#define MBX_ARGC 0x08
#define MBX_STATUS 0x0c
#define MBX_RTYPE 0x10
#define MBX_RLENGTH 0x14
#define MBX_RINT 0x18
#define MBX_ELEMENT 0x1c
#define MBX_SLOTS 0x40
#define MBX_SLOT_COUNT 7
#define MBX_SLOT_SIZE 0x210
#define MBX_SLOT_TYPE 0x00
#define MBX_SLOT_LENGTH 0x04
#define MBX_SLOT_INT 0x08
#define MBX_SLOT_DATA 0x10
#define MBX_SLOT_DATA_SIZE 0x200
#define MBX_RDATA 0x1000
#define MBX_RDATA_SIZE 0x1000
#define MBX_SIZE 0x2000
#define DOORBELL_SIZE 0x1000

#define FUNCTION_CALL 1
#define FUNCTION_ELEMENT 2
#define FUNCTION_NOTIFICATION 3

// Where the guest's call of a firmware method is.
#define CALL_NONE 0
#define CALL_RUNNING 1
#define CALL_DONE 2

// ObjectType() numbering, which the results use too.
#define TYPE_NONE 0
#define TYPE_INTEGER 1
#define TYPE_STRING 2
#define TYPE_BUFFER 3
#define TYPE_PACKAGE 4

// GELT's second index when the element is not in a nested package.
#define ELEMENT_NONE 0xffff

// Arguments CALL takes besides the method's index and argument count.
#define CALL_ARGS 5

// The PM1 and GPE bits the guest sees: power button, and GPE 0 for the
// notifications the firmware's AML sends, ACPI 1.0b, 4.7.3.
#define PM1_PWRBTN 0x0100
#define PM1_WAK 0x8000
#define PM1_CNT_SCI_EN 0x0001
#define PM1_CNT_SLP_TYP_SHIFT 10
#define PM1_CNT_SLP_EN 0x2000
#define SLP_TYP_S5 5

#define QUEUE_SIZE 128
#define MAX_PATH_DEPTH 32
#define MAX_PACKAGE 255

#define PS2_PORT_FIRST 0x60
#define PS2_PORT_LAST 0x64

#define PCI_CLASS 0x0a
#define PCI_CLASS_ISA_BRIDGE 0x0601

// The FADT's fields, ACPI 1.0b, Table 5-6.
#define FADT_FIRMWARE_CTRL 36
#define FADT_DSDT 40
#define FADT_INT_MODEL 44
#define FADT_SCI_INT 46
#define FADT_PM1A_EVT_BLK 56
#define FADT_PM1A_CNT_BLK 64
#define FADT_PM_TMR_BLK 76
#define FADT_GPE0_BLK 80
#define FADT_PM1_EVT_LEN 88
#define FADT_PM1_CNT_LEN 89
#define FADT_PM_TM_LEN 91
#define FADT_GPE0_BLK_LEN 92
#define FADT_P_LVL2_LAT 96
#define FADT_P_LVL3_LAT 98
#define FADT_DAY_ALRM 106
#define FADT_MON_ALRM 107
#define FADT_CENTURY 108
#define FADT_FLAGS 112
#define FADT_WBINVD ((uint32_t)1 << 0)
#define FADT_PROC_C1 ((uint32_t)1 << 2)
#define FADT_PWR_BUTTON ((uint32_t)1 << 4)
#define FADT_SLP_BUTTON ((uint32_t)1 << 5)
#define FADT_FIX_RTC ((uint32_t)1 << 6)
#define FADT_TMR_VAL_EXT ((uint32_t)1 << 8)

struct method {
    uacpi_namespace_node *node;
    unsigned int args;
};

struct notification {
    uint32_t target;
    uint32_t value;
};

struct path {
    char segs[MAX_PATH_DEPTH][4];
    size_t count;
};

static struct {
    bool active;
    uintptr_t mailbox;
    uint32_t doorbell;
    bool sci;
    uint32_t sci_gsi;
    uint16_t pm1_evt;
    uint16_t pm1_cnt;
    uint16_t pm_tmr;
    uint16_t gpe0;
    bool pm1;
    bool gpe;
    bool pm_tmr_32;
    bool power_button;

    uint16_t pm1_sts;
    uint16_t pm1_en;
    uint16_t slp_typ;
    uint8_t gpe0_en;

    struct method *methods;
    size_t method_count;
    uint32_t method_capacity;
    uacpi_namespace_node **targets;
    size_t target_count;
    uint32_t target_capacity;
    uacpi_namespace_node **hidden;
    size_t hidden_count;
    uint32_t hidden_capacity;
    uint16_t links;

    struct notification queue[QUEUE_SIZE];
    uint32_t queue_head;
    size_t queue_count;
    bool dropping;
    uacpi_object *last;
    struct spinlock lock;

    uacpi_handle call_work;
    uint32_t call;
    size_t call_index;
    uint32_t call_argc;
} vacpi;

// Not mirrored: what only the firmware's own OSPM, the hypervisor, runs, the
// operating system provides, or the hypervisor replaces.
static const char *const left_out_methods[] = {
    "_INI", "_REG", "_PTS", "_WAK", "_GTS", "_BFS", "_TTS", "_PIC", "_PRW",
    "_PSW", "_DSW", "_S0W", "_S1W", "_S2W", "_S3W", "_S4W", "_OSI", "_OS_",
    "_REV", "_PRT", "_DEP", "_GLK", "_GPE", NULL,
};
static const char *const left_out_names[] = {
    "_PRW", "_S0_", "_S1_", "_S2_", "_S3_", "_S4_", "_S5_", "_PRT", "_DEP",
    "_GPE", "_OS_", "_REV", "_GL_", NULL,
};
// Processor power management stays the hypervisor's.
static const char *const left_out_processor[] = {
    "_PSS", "_PCT", "_PPC", "_CST", "_TSS", "_PTC", "_TPC", "_CPC", "_LPI",
    "_PDC", "_OSC", "_PSD", "_CSD", "_TSD", NULL,
};
// Methods that return the same thing every time, by the specification, and
// become names.
static const char *const constant_methods[] = {
    "_HID", "_CID", "_UID", "_ADR", "_SUN", "_CLS", "_HRV", "_SUB", "_DDN",
    "_STR", "_MLS", "_PXM", "_SEG", "_BBN", "_UPC", "_PLD", "_EJD", "_RMV",
    "_PR0", "_PR1", "_PR2", "_PR3", "_PRE", "_AL0", "_AL1", "_AL2", "_AL3",
    "_AL4", "_AL5", "_AL6", "_AL7", "_AL8", "_AL9", "_PSL", "_TZD", "_EDL",
    "_CCA", NULL,
};
// Methods that return packages whose shape does not change, which their stubs,
// as ACPI 1.0 cannot make packages of a size known only at run time, build to
// the shape they had once.
static const char *const shaped_methods[] = {
    "_BIF", "_BIX", "_BST", "_PIF", "_BCL", "_DOD", "_HPP", "_ALR", "_FIF",
    "_FPS", "_FST", "_BMD", NULL,
};

static bool name_in(const char name[4], const char *const *list) {
    for (size_t i = 0; list[i] != NULL; i++) {
        if (memcmp(name, list[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

static bool hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

// _Lxx, _Exx, _Qxx: the firmware's event handlers.
static bool event_method(const char name[4]) {
    return name[0] == '_'
        && (name[1] == 'L' || name[1] == 'E' || name[1] == 'Q')
        && hex_digit(name[2]) && hex_digit(name[3]);
}

static void *grow(void *array, size_t count, uint32_t *capacity, size_t size) {
    if (count < *capacity) {
        return array;
    }
    uint32_t new_capacity = *capacity != 0 ? *capacity * 2 : 64;
    void *bigger = kmalloc(new_capacity * size);
    if (count != 0) {
        memcpy(bigger, array, count * size);
    }
    kfree(array);
    *capacity = new_capacity;
    return bigger;
}

static bool is_hidden(uacpi_namespace_node *node) {
    for (size_t i = 0; i < vacpi.hidden_count; i++) {
        if (vacpi.hidden[i] == node) {
            return true;
        }
    }
    return false;
}

// Where a node is in the guest's namespace: its hidden ancestors' children sit
// in their parents'. False for hidden nodes themselves.
static bool guest_path(uacpi_namespace_node *node, struct path *out) {
    if (is_hidden(node)) {
        return false;
    }
    char rev[MAX_PATH_DEPTH][4];
    size_t count = 0;
    uacpi_namespace_node *root = uacpi_namespace_root();
    for (uacpi_namespace_node *n = node; n != NULL && n != root;
         n = uacpi_namespace_node_parent(n)) {
        if (is_hidden(n)) {
            continue;
        }
        if (count == MAX_PATH_DEPTH) {
            return false;
        }
        memcpy(rev[count++], uacpi_namespace_node_name(n).text, 4);
    }
    for (size_t i = 0; i < count; i++) {
        memcpy(out->segs[i], rev[count - 1 - i], 4);
    }
    out->count = count;
    return true;
}

static void emit_path(struct aml *a, const struct path *p) {
    aml_name_segs(a, true, (const char (*)[4])p->segs, p->count);
}

static void helper(struct aml *a, const char *name) {
    char path[24] = "\\_SB.RBMB.";
    memcpy(path + 10, name, 4);
    path[14] = '\0';
    aml_name(a, path);
}

static void emit_seg(struct aml *a, const char name[4]) {
    aml_bytes(a, name, 4);
}

// EisaId("PNPxxxx").
static uint32_t eisa_id(const char *id) {
    uint32_t vendor = (uint32_t)(id[0] - '@') << 10
        | (uint32_t)(id[1] - '@') << 5 | (uint32_t)(id[2] - '@');
    uint32_t product = 0;
    for (size_t i = 3; i < 7; i++) {
        char c = id[i];
        product = product << 4 | (uint32_t)(c <= '9' ? c - '0' : c - 'A' + 10);
    }
    return (vendor >> 8 | (vendor & 0xff) << 8)
        | (product >> 8 | (product & 0xff) << 8) << 16;
}

// Constants, from what evaluating the firmware's objects gave. References
// become the referenced object's name.
static void emit_data(
    struct aml *a, uacpi_object *obj, uacpi_namespace_node *scope
) {
    uint64_t value;
    uacpi_data_view view;
    uacpi_object_array package;
    if (uacpi_object_is_aml_namepath(obj)) {
        uacpi_namespace_node *node;
        struct path p;
        if (uacpi_object_resolve_as_aml_namepath(obj, scope, &node)
         == UACPI_STATUS_OK
         && guest_path(node, &p)) {
            emit_path(a, &p);
        } else {
            aml_int(a, 0);
        }
        return;
    }
    switch (uacpi_object_get_type(obj)) {
        case UACPI_OBJECT_INTEGER: {
            uacpi_object_get_integer(obj, &value);
            aml_int(a, (uint32_t)value);
            break;
        }
        case UACPI_OBJECT_STRING: {
            uacpi_object_get_string(obj, &view);
            aml_string(a, view.const_text, (uint32_t)view.length);
            break;
        }
        case UACPI_OBJECT_BUFFER: {
            uacpi_object_get_buffer(obj, &view);
            aml_buffer(a, view.const_bytes, (uint32_t)view.length);
            break;
        }
        case UACPI_OBJECT_PACKAGE: {
            uacpi_object_get_package(obj, &package);
            size_t count = package.count < MAX_PACKAGE
                ? (uint32_t)package.count : MAX_PACKAGE;
            aml_begin(a, AML_PACKAGE);
            aml_byte(a, (uint8_t)count);
            for (size_t i = 0; i < count; i++) {
                emit_data(a, package.objects[i], scope);
            }
            aml_end(a);
            break;
        }
        default: {
            aml_int(a, 0);
            break;
        }
    }
}

static uint32_t method_add(uacpi_namespace_node *node, unsigned int args) {
    vacpi.methods = grow(
        vacpi.methods, vacpi.method_count, &vacpi.method_capacity,
        sizeof(struct method)
    );
    vacpi.methods[vacpi.method_count] = (struct method){ node, args };
    return vacpi.method_count++;
}

static void target_add(uacpi_namespace_node *node) {
    vacpi.targets = grow(
        vacpi.targets, vacpi.target_count, &vacpi.target_capacity,
        sizeof(uacpi_namespace_node *)
    );
    vacpi.targets[vacpi.target_count++] = node;
}

static void hidden_add(uacpi_namespace_node *node) {
    vacpi.hidden = grow(
        vacpi.hidden, vacpi.hidden_count, &vacpi.hidden_capacity,
        sizeof(uacpi_namespace_node *)
    );
    vacpi.hidden[vacpi.hidden_count++] = node;
}

static void put_args(struct aml *a, unsigned int args) {
    for (size_t k = 0; k < args; k++) {
        helper(a, "PUTA");
        aml_int(a, k);
        aml_arg(a, k);
    }
}

static void acquire(struct aml *a) {
    aml_op(a, AML_ACQUIRE);
    helper(a, "MBXM");
    aml_word(a, 0xffff);
}

static void release(struct aml *a) {
    aml_op(a, AML_RELEASE);
    helper(a, "MBXM");
}

// Store (GELT (i, j), Index (target, index)).
static void store_element(
    struct aml *a, size_t i, size_t j, unsigned int target_local, size_t index
) {
    aml_op(a, AML_STORE);
    helper(a, "GELT");
    aml_int(a, i);
    aml_int(a, j);
    aml_op(a, AML_INDEX);
    aml_local(a, target_local);
    aml_int(a, index);
    aml_byte(a, AML_ZERO);
}

// A stub that rebuilds a package of the shape the method's result had.
static void emit_shaped_stub(
    struct aml *a, const char name[4], unsigned int args, size_t index,
    uacpi_object *shape
) {
    uacpi_object_array top;
    uacpi_object_get_package(shape, &top);
    aml_begin(a, AML_METHOD);
    emit_seg(a, name);
    aml_byte(a, (uint8_t)args);
    acquire(a);
    put_args(a, args);
    helper(a, "RCAL");
    aml_int(a, index);
    aml_int(a, args);
    aml_op(a, AML_STORE);
    aml_begin(a, AML_PACKAGE);
    aml_byte(a, (uint8_t)top.count);
    aml_end(a);
    aml_local(a, 1);
    for (size_t i = 0; i < top.count; i++) {
        uacpi_object_array inner;
        if (uacpi_object_get_package(top.objects[i], &inner)
         != UACPI_STATUS_OK) {
            store_element(a, i, ELEMENT_NONE, 1, i);
            continue;
        }
        aml_op(a, AML_STORE);
        aml_begin(a, AML_PACKAGE);
        aml_byte(a, (uint8_t)inner.count);
        aml_end(a);
        aml_local(a, 2);
        for (size_t j = 0; j < inner.count; j++) {
            store_element(a, i, j, 2, j);
        }
        aml_op(a, AML_STORE);
        aml_local(a, 2);
        aml_op(a, AML_INDEX);
        aml_local(a, 1);
        aml_int(a, i);
        aml_byte(a, AML_ZERO);
    }
    release(a);
    aml_op(a, AML_RETURN);
    aml_local(a, 1);
    aml_end(a);
}

// Whether a package has one nesting level at most, and no more elements than
// ACPI 1.0 allows.
static bool shape_usable(uacpi_object *obj) {
    uacpi_object_array top;
    if (uacpi_object_get_package(obj, &top) != UACPI_STATUS_OK
     || top.count > MAX_PACKAGE) {
        return false;
    }
    for (size_t i = 0; i < top.count; i++) {
        uacpi_object_array inner;
        if (uacpi_object_get_package(top.objects[i], &inner)
         != UACPI_STATUS_OK) {
            continue;
        }
        if (inner.count > MAX_PACKAGE) {
            return false;
        }
        for (size_t j = 0; j < inner.count; j++) {
            if (uacpi_object_get_type(inner.objects[j])
             == UACPI_OBJECT_PACKAGE) {
                return false;
            }
        }
    }
    return true;
}

static bool crs_replaced(uacpi_namespace_node *device);

static void emit_method(
    struct aml *a, uacpi_namespace_node *node, const char name[4],
    bool in_processor
) {
    if (name_in(name, left_out_methods) || event_method(name)
     || (in_processor && name_in(name, left_out_processor))
     || (memcmp(name, "_CRS", 4) == 0
      && crs_replaced(uacpi_namespace_node_parent(node))) != 0) {
        return;
    }
    uacpi_namespace_node_info *info;
    if (uacpi_get_namespace_node_info(node, &info) != UACPI_STATUS_OK) {
        return;
    }
    unsigned int args = info->num_params;
    uacpi_free_namespace_node_info(info);
    uacpi_namespace_node *scope = uacpi_namespace_node_parent(node);

    if (args == 0 && name_in(name, constant_methods)) {
        uacpi_object *obj;
        if (uacpi_eval(node, NULL, NULL, &obj) == UACPI_STATUS_OK
         && obj != NULL) {
            aml_op(a, AML_NAME);
            emit_seg(a, name);
            emit_data(a, obj, scope);
            uacpi_object_unref(obj);
        }
        return;
    }

    size_t index = method_add(node, args);
    if (args == 0 && name_in(name, shaped_methods)) {
        uacpi_object *obj;
        if (uacpi_eval(node, NULL, NULL, &obj) == UACPI_STATUS_OK
         && obj != NULL) {
            bool usable = shape_usable(obj);
            if (usable) {
                emit_shaped_stub(a, name, args, index, obj);
            }
            uacpi_object_unref(obj);
            if (usable) {
                return;
            }
        }
    }

    aml_begin(a, AML_METHOD);
    emit_seg(a, name);
    aml_byte(a, (uint8_t)args);
    if (args <= CALL_ARGS) {
        aml_op(a, AML_RETURN);
        helper(a, "CALL");
        aml_int(a, index);
        aml_int(a, args);
        for (size_t k = 0; k < CALL_ARGS; k++) {
            if (k < args) {
                aml_arg(a, k);
            } else {
                aml_int(a, 0);
            }
        }
        aml_end(a);
        return;
    }
    // Too many arguments for CALL to pass on.
    acquire(a);
    put_args(a, args);
    aml_op(a, AML_STORE);
    helper(a, "RCAL");
    aml_int(a, index);
    aml_int(a, args);
    aml_local(a, 0);
    release(a);
    aml_op(a, AML_RETURN);
    aml_local(a, 0);
    aml_end(a);
}

static void emit_name(
    struct aml *a, uacpi_namespace_node *node, const char name[4]
) {
    if (name_in(name, left_out_names)
     || (memcmp(name, "_CRS", 4) == 0
      && crs_replaced(uacpi_namespace_node_parent(node))) != 0) {
        return;
    }
    uacpi_object *obj;
    if (uacpi_eval(node, NULL, NULL, &obj) != UACPI_STATUS_OK || obj == NULL) {
        return;
    }
    aml_op(a, AML_NAME);
    emit_seg(a, name);
    emit_data(a, obj, uacpi_namespace_node_parent(node));
    uacpi_object_unref(obj);
}

static uacpi_iteration_decision link_irq(void *user, uacpi_resource *r) {
    uint32_t *irq = user;
    if (r->type == UACPI_RESOURCE_TYPE_IRQ && r->irq.num_irqs != 0) {
        *irq = r->irq.irqs[0];
    } else if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ
            && r->extended_irq.num_irqs != 0) {
        *irq = r->extended_irq.irqs[0];
    } else {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    return UACPI_ITERATION_DECISION_BREAK;
}

// _PRT, as the hypervisor routes: GSIs in APIC mode, and in PIC mode the ISA
// IRQs the guest's 8259s see them on, through a link device for each.
static void emit_prt(struct aml *a, uacpi_namespace_node *bridge) {
    uacpi_pci_routing_table *prt;
    if (uacpi_get_pci_routing_table(bridge, &prt) != UACPI_STATUS_OK) {
        return;
    }
    size_t count = prt->num_entries < MAX_PACKAGE ? (uint32_t)prt->num_entries
        : MAX_PACKAGE;
    uint32_t *gsis = kmalloc((count + 1) * sizeof(uint32_t));
    size_t pic_count = 0;
    for (size_t i = 0; i < count; i++) {
        const uacpi_pci_routing_table_entry *e = &prt->entries[i];
        gsis[i] = e->index;
        if (e->source != NULL) {
            gsis[i] = UINT32_MAX;
            uacpi_for_each_device_resource(
                e->source, "_CRS", link_irq, &gsis[i]
            );
        }
        if (gsis[i] != UINT32_MAX && irq_isa_irq(gsis[i]) >= 0) {
            pic_count++;
        }
    }

    aml_method(a, "_PRT", 0, 0);
    aml_begin(a, AML_IF);
    helper(a, "PICM");
    aml_op(a, AML_RETURN);
    aml_begin(a, AML_PACKAGE);
    size_t apic_count = 0;
    for (size_t i = 0; i < count; i++) {
        apic_count += gsis[i] != UINT32_MAX;
    }
    aml_byte(a, (uint8_t)apic_count);
    for (size_t i = 0; i < count; i++) {
        if (gsis[i] == UINT32_MAX) {
            continue;
        }
        aml_begin(a, AML_PACKAGE);
        aml_byte(a, 4);
        aml_int(a, prt->entries[i].address);
        aml_int(a, prt->entries[i].pin);
        aml_int(a, 0);
        aml_int(a, gsis[i]);
        aml_end(a);
    }
    aml_end(a);
    aml_end(a);
    aml_op(a, AML_RETURN);
    aml_begin(a, AML_PACKAGE);
    aml_byte(a, (uint8_t)pic_count);
    for (size_t i = 0; i < count; i++) {
        int irq = gsis[i] != UINT32_MAX ? irq_isa_irq(gsis[i]) : -1;
        if (irq < 0) {
            continue;
        }
        vacpi.links |= (uint16_t)((uint32_t)1 << irq);
        char link[] = "\\_SB.LI00";
        static const char hex[] = "0123456789ABCDEF";
        link[7] = hex[irq >> 4];
        link[8] = hex[irq & 0xf];
        aml_begin(a, AML_PACKAGE);
        aml_byte(a, 4);
        aml_int(a, prt->entries[i].address);
        aml_int(a, prt->entries[i].pin);
        aml_name(a, link);
        aml_int(a, 0);
        aml_end(a);
    }
    aml_end(a);
    aml_end(a);
    kfree(gsis);
    uacpi_free_pci_routing_table(prt);
}

static void emit_children(
    struct aml *a, uacpi_namespace_node *parent, bool in_processor
);
static void emit_scopes(struct aml *a, uacpi_namespace_node *parent);

static const char *const link_ids[] = { "PNP0C0F", NULL };
static const char *const ec_ids[] = { "PNP0C09", NULL };
static const char *const hpet_ids[] = { "PNP0103", NULL };
static const char *const host_bridge_ids[] = { "PNP0A03", "PNP0A08", NULL };
static const char *const motherboard_ids[] = { "PNP0C01", "PNP0C02", NULL };

// Resource descriptors, ACPI 6.6, 6.4: small items have their length in bits
// 2:0, large ones in the 16 bits after the first byte.
#define RES_LARGE ((uint32_t)0x80)
#define RES_END_TAG 0x79
#define RES_MEMORY24 0x81
#define RES_MEMORY32 0x85
#define RES_FIXED_MEMORY32 0x86
#define RES_DWORD_ADDRESS 0x87
#define RES_WORD_ADDRESS 0x88
#define RES_QWORD_ADDRESS 0x8a
#define RES_EXTENDED_ADDRESS 0x8b
#define ADDRESS_MEMORY 0
// A producer's fixed window, readable and writable, uncached or cacheable.
#define ADDRESS_FIXED_PRODUCER 0x0c
#define MEMORY_RW 0x01
#define MEMORY_CACHEABLE 0x02
#define CRS_MAX 2048
#define ONE_MIB ((uint64_t)0x100000)

static uint64_t le(const uint8_t *p, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = bytes; i-- > 0; ) {
        value = value << 8 | p[i];
    }
    return value;
}

static void put_le(uint8_t *p, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}

static size_t resource_size(const uint8_t *p, uint32_t left) {
    if ((p[0] & RES_LARGE) == 0) {
        return 1 + (p[0] & (uint32_t)7);
    }
    return left < 3 ? left + 1 : 3 + (uint32_t)le(p + 1, 2);
}

// The memory a descriptor of size bytes describes, if any.
static bool resource_memory(
    const uint8_t *p, size_t size, uint64_t *base, uint64_t *end
) {
    uint64_t min;
    uint64_t max;
    size_t length;
    switch (p[0]) {
        case RES_MEMORY24: {
            if (size < 12) {
                return false;
            }
            min = le(p + 4, 2) << 8;
            max = le(p + 6, 2) << 8;
            length = le(p + 10, 2) << 8;
            break;
        }
        case RES_MEMORY32: {
            if (size < 20) {
                return false;
            }
            min = le(p + 4, 4);
            max = le(p + 8, 4);
            length = le(p + 16, 4);
            break;
        }
        case RES_FIXED_MEMORY32: {
            if (size < 12) {
                return false;
            }
            min = le(p + 4, 4);
            length = le(p + 8, 4);
            max = min + length;
            break;
        }
        case RES_WORD_ADDRESS: {
            if (size < 16 || p[3] != ADDRESS_MEMORY) {
                return false;
            }
            min = le(p + 8, 2);
            max = le(p + 10, 2);
            length = le(p + 14, 2);
            break;
        }
        case RES_DWORD_ADDRESS: {
            if (size < 26 || p[3] != ADDRESS_MEMORY) {
                return false;
            }
            min = le(p + 10, 4);
            max = le(p + 14, 4);
            length = le(p + 22, 4);
            break;
        }
        case RES_QWORD_ADDRESS: {
            if (size < 46 || p[3] != ADDRESS_MEMORY) {
                return false;
            }
            min = le(p + 14, 8);
            max = le(p + 22, 8);
            length = le(p + 38, 8);
            break;
        }
        case RES_EXTENDED_ADDRESS: {
            if (size < 56 || p[3] != ADDRESS_MEMORY) {
                return false;
            }
            min = le(p + 16, 8);
            max = le(p + 24, 8);
            length = le(p + 40, 8);
            break;
        }
        default: {
            return false;
        }
    }
    *base = min;
    *end = length != 0 ? min + length : max + 1;
    return true;
}

static uint32_t put_window(
    uint8_t *out, uintptr_t base, uintptr_t end, bool high
) {
    if (high) {
        uint8_t d[46] = {
            RES_QWORD_ADDRESS, 43, 0, ADDRESS_MEMORY, ADDRESS_FIXED_PRODUCER,
            MEMORY_RW | MEMORY_CACHEABLE
        };
        put_le(d + 14, base, 8);
        put_le(d + 22, end - 1, 8);
        put_le(d + 38, end - base, 8);
        memcpy(out, d, sizeof(d));
        return sizeof(d);
    }
    uint8_t d[26] = {
        RES_DWORD_ADDRESS, 23, 0, ADDRESS_MEMORY, ADDRESS_FIXED_PRODUCER,
        MEMORY_RW
    };
    put_le(d + 10, base, 4);
    put_le(d + 14, end - 1, 4);
    put_le(d + 22, end - base, 4);
    memcpy(out, d, sizeof(d));
    return sizeof(d);
}

static bool overlaps_private(uintptr_t base, uintptr_t end) {
    const struct pci_host *host = pci_host_scan();
    for (size_t i = 0; i < host->private_count; i++) {
        if (base < host->private[i].end && end > host->private[i].base) {
            return true;
        }
    }
    return false;
}

static const struct pci_plan_root *plan_root(uacpi_namespace_node *bridge) {
    const struct pci_host *host = pci_host_scan();
    const struct pci_plan *plan = pci_plan();
    for (size_t i = 0; i < host->bridge_count && plan != NULL; i++) {
        if (host->bridges[i].node != bridge) {
            continue;
        }
        for (size_t r = 0; r < plan->root_count; r++) {
            if (plan->roots[r].segment == host->bridges[i].segment
             && plan->roots[r].bus == host->bridges[i].bus) {
                return &plan->roots[r];
            }
        }
    }
    return NULL;
}

static bool crs_replaced(uacpi_namespace_node *device) {
    return uacpi_device_matches_pnp_id(device, host_bridge_ids)
        || uacpi_device_matches_pnp_id(device, motherboard_ids);
}

// The guest's _CRS of a host bridge or a motherboard device, from the
// firmware's: a host bridge's windows are its root bus's in the guest's PCI
// plan, but for the legacy ones below 1 MiB, and a motherboard device never
// claims the configuration space windows and the IOMMUs, which the guest has
// not at their addresses.
static void emit_crs(struct aml *a, uacpi_namespace_node *device) {
    uacpi_object *obj;
    if (uacpi_eval(device, "_CRS", NULL, &obj) != UACPI_STATUS_OK
     || obj == NULL) {
        return;
    }
    uacpi_data_view view;
    if (uacpi_object_get_type(obj) != UACPI_OBJECT_BUFFER
     || uacpi_object_get_buffer(obj, &view) != UACPI_STATUS_OK) {
        uacpi_object_unref(obj);
        return;
    }
    bool bridge = uacpi_device_matches_pnp_id(device, host_bridge_ids);
    uint8_t *out = kmalloc(CRS_MAX);
    size_t size = 0;
    const uint8_t *in = view.const_bytes;
    uint32_t left = (uint32_t)view.length;
    while (left != 0 && in[0] != RES_END_TAG) {
        size_t n = resource_size(in, left);
        if (n > left) {
            break;
        }
        uintptr_t base;
        uintptr_t end;
        bool keep = true;
        if (resource_memory(in, n, &base, &end)) {
            keep = bridge ? end <= ONE_MIB : !overlaps_private(base, end);
        }
        if (keep && size + n <= CRS_MAX - 2 * 46 - 2) {
            memcpy(out + size, in, n);
            size += n;
        }
        in += n;
        left -= n;
    }
    uacpi_object_unref(obj);
    const struct pci_plan_root *root = bridge ? plan_root(device) : NULL;
    if (root != NULL && root->low.end != root->low.base) {
        size += put_window(out + size, root->low.base, root->low.end, false);
    }
    if (root != NULL && root->high.end != root->high.base) {
        size += put_window(out + size, root->high.base, root->high.end, true);
    }
    out[size++] = RES_END_TAG;
    out[size++] = 0;
    aml_op(a, AML_NAME);
    emit_seg(a, "_CRS");
    aml_buffer(a, out, size);
    kfree(out);
}

// The devices the guest does not get, known before anything refers to them.
static uacpi_iteration_decision hide_found(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth
) {
    (void)user;
    (void)depth;
    if (uacpi_device_matches_pnp_id(node, link_ids)
     || uacpi_device_matches_pnp_id(node, ec_ids)) {
        hidden_add(node);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static bool child_named(uacpi_namespace_node *parent, const char *name) {
    uacpi_namespace_node *child;
    return uacpi_namespace_node_find(parent, name, &child) == UACPI_STATUS_OK;
}

static void emit_object(
    struct aml *a, uacpi_namespace_node *node, bool in_processor
) {
    if (uacpi_namespace_node_is_alias(node)) {
        return;
    }
    uacpi_object_name name = uacpi_namespace_node_name(node);
    uacpi_object_type type;
    if (uacpi_namespace_node_type(node, &type) != UACPI_STATUS_OK) {
        return;
    }
    uacpi_object *obj;
    switch (type) {
        case UACPI_OBJECT_DEVICE: {
            // One HPET is all an in-scope machine has.
            if (uacpi_device_matches_pnp_id(node, link_ids)
             || (hpet_any_hidden()
              && uacpi_device_matches_pnp_id(node, hpet_ids))) {
                return;
            }
            // The hypervisor drives the embedded controller, so the guest gets
            // only the devices in its scope, in its parent's.
            if (uacpi_device_matches_pnp_id(node, ec_ids)) {
                emit_scopes(a, node);
                return;
            }
            target_add(node);
            aml_begin(a, AML_DEVICE);
            emit_seg(a, name.text);
            emit_children(a, node, false);
            if (crs_replaced(node) && child_named(node, "_CRS")) {
                emit_crs(a, node);
            }
            if (child_named(node, "_PRT")) {
                emit_prt(a, node);
            }
            aml_end(a);
            return;
        }
        case UACPI_OBJECT_PROCESSOR: {
            uacpi_processor_info info = { 0 };
            if (uacpi_eval(node, NULL, NULL, &obj) == UACPI_STATUS_OK
             && obj != NULL) {
                uacpi_object_get_processor_info(obj, &info);
                uacpi_object_unref(obj);
            }
            target_add(node);
            aml_begin(a, AML_PROCESSOR);
            emit_seg(a, name.text);
            aml_byte(a, info.id);
            aml_dword(a, 0);
            aml_byte(a, 0);
            emit_children(a, node, true);
            aml_end(a);
            return;
        }
        case UACPI_OBJECT_POWER_RESOURCE: {
            uacpi_power_resource_info info = { 0 };
            if (uacpi_eval(node, NULL, NULL, &obj) == UACPI_STATUS_OK
             && obj != NULL) {
                uacpi_object_get_power_resource_info(obj, &info);
                uacpi_object_unref(obj);
            }
            aml_begin(a, AML_POWER_RESOURCE);
            emit_seg(a, name.text);
            aml_byte(a, info.system_level);
            aml_word(a, info.resource_order);
            emit_children(a, node, false);
            aml_end(a);
            return;
        }
        case UACPI_OBJECT_THERMAL_ZONE: {
            target_add(node);
            aml_begin(a, AML_THERMAL_ZONE);
            emit_seg(a, name.text);
            emit_children(a, node, false);
            aml_end(a);
            return;
        }
        case UACPI_OBJECT_METHOD: {
            emit_method(a, node, name.text, in_processor);
            return;
        }
        case UACPI_OBJECT_INTEGER:
        case UACPI_OBJECT_STRING:
        case UACPI_OBJECT_BUFFER:
        case UACPI_OBJECT_PACKAGE: {
            if (in_processor && name_in(name.text, left_out_processor)) {
                return;
            }
            emit_name(a, node, name.text);
            return;
        }
        default: {
            // Regions, fields, mutexes, and events stay the firmware's.
            return;
        }
    }
}

static void emit_children(
    struct aml *a, uacpi_namespace_node *parent, bool in_processor
) {
    uacpi_namespace_node *iter = NULL;
    while (uacpi_namespace_node_next(parent, &iter) == UACPI_STATUS_OK) {
        emit_object(a, iter, in_processor);
    }
}

static void emit_scopes(struct aml *a, uacpi_namespace_node *parent) {
    uacpi_namespace_node *iter = NULL;
    uacpi_object_type_bits scopes = UACPI_OBJECT_DEVICE_BIT
        | UACPI_OBJECT_PROCESSOR_BIT | UACPI_OBJECT_POWER_RESOURCE_BIT
        | UACPI_OBJECT_THERMAL_ZONE_BIT;
    while (uacpi_namespace_node_next_typed(parent, &iter, scopes)
        == UACPI_STATUS_OK) {
        emit_object(a, iter, false);
    }
}

// The root's children: the predefined scopes as scopes, the rest as objects.
static void emit_root(struct aml *a) {
    uacpi_namespace_node *root = uacpi_namespace_root();
    uacpi_namespace_node *iter = NULL;
    while (uacpi_namespace_node_next(root, &iter) == UACPI_STATUS_OK) {
        uacpi_object_name name = uacpi_namespace_node_name(iter);
        if (memcmp(name.text, "_GPE", 4) == 0
         || memcmp(name.text, "_GL_", 4) == 0
         || memcmp(name.text, "_OSI", 4) == 0
         || memcmp(name.text, "_OS_", 4) == 0
         || memcmp(name.text, "_REV", 4) == 0) {
            continue;
        }
        if (memcmp(name.text, "_SB_", 4) == 0
         || memcmp(name.text, "_TZ_", 4) == 0
         || memcmp(name.text, "_PR_", 4) == 0
         || memcmp(name.text, "_SI_", 4) == 0) {
            if (memcmp(name.text, "_SB_", 4) == 0
             || memcmp(name.text, "_TZ_", 4) == 0) {
                target_add(iter);
            }
            aml_begin(a, AML_SCOPE);
            aml_byte(a, AML_ROOT_CHAR);
            emit_seg(a, name.text);
            emit_children(a, iter, false);
            aml_end(a);
            continue;
        }
        emit_object(a, iter, false);
    }
}

// Store (Field, Local) and friends, by name.
static void store_names(struct aml *a, const char *src, unsigned int local) {
    aml_op(a, AML_STORE);
    aml_name(a, src);
    aml_local(a, local);
}

static void store_int_name(struct aml *a, uint32_t value, const char *dst) {
    aml_op(a, AML_STORE);
    aml_int(a, value);
    aml_name(a, dst);
}

// If (LEqual (Local, value)) {, closed by aml_end().
static void if_local_equals(
    struct aml *a, unsigned int local, uint32_t value
) {
    aml_begin(a, AML_IF);
    aml_op(a, AML_LEQUAL);
    aml_local(a, local);
    aml_int(a, value);
}

// Index (Local, Add (Local, offset)): a byte of a buffer, as a target.
static void byte_at(
    struct aml *a, unsigned int buffer, unsigned int base, size_t offset
) {
    aml_op(a, AML_INDEX);
    aml_local(a, buffer);
    aml_op(a, AML_ADD);
    aml_local(a, base);
    aml_int(a, offset);
    aml_byte(a, AML_ZERO);
    aml_byte(a, AML_ZERO);
}

// Package arguments, of integers and buffers, as their stubs pass them on: for
// each element, its type, a byte, its length, a word, then its bytes.
static void emit_serp(struct aml *a) {
    aml_method(a, "SERP", 1, 0);
    aml_op(a, AML_STORE);
    aml_begin(a, AML_BUFFER);
    aml_int(a, MBX_SLOT_DATA_SIZE);
    aml_end(a);
    aml_local(a, 0);
    aml_op(a, AML_STORE);
    aml_op(a, AML_SIZE_OF);
    aml_arg(a, 0);
    aml_local(a, 1);
    aml_op(a, AML_STORE);
    aml_int(a, 0);
    aml_local(a, 2);
    aml_op(a, AML_STORE);
    aml_int(a, 0);
    aml_local(a, 3);
    aml_begin(a, AML_WHILE);
    aml_op(a, AML_LLESS);
    aml_local(a, 2);
    aml_local(a, 1);
    {
        aml_op(a, AML_STORE);
        aml_op(a, AML_DEREF_OF);
        aml_op(a, AML_INDEX);
        aml_arg(a, 0);
        aml_local(a, 2);
        aml_byte(a, AML_ZERO);
        aml_local(a, 4);
        aml_op(a, AML_STORE);
        aml_op(a, AML_OBJECT_TYPE);
        aml_local(a, 4);
        aml_local(a, 5);
        aml_op(a, AML_STORE);
        aml_local(a, 5);
        byte_at(a, 0, 3, 0);
        if_local_equals(a, 5, TYPE_INTEGER);
        {
            aml_op(a, AML_STORE);
            aml_int(a, 4);
            byte_at(a, 0, 3, 2);
            for (size_t i = 0; i < 4; i++) {
                aml_op(a, AML_STORE);
                aml_op(a, AML_AND);
                aml_op(a, AML_SHIFT_RIGHT);
                aml_local(a, 4);
                aml_int(a, 8 * i);
                aml_byte(a, AML_ZERO);
                aml_int(a, 0xff);
                aml_byte(a, AML_ZERO);
                byte_at(a, 0, 3, 4 + i);
            }
            aml_op(a, AML_ADD);
            aml_local(a, 3);
            aml_int(a, 8);
            aml_local(a, 3);
        }
        aml_end(a);
        aml_begin(a, AML_ELSE);
        {
            if_local_equals(a, 5, TYPE_BUFFER);
            {
                aml_op(a, AML_STORE);
                aml_op(a, AML_SIZE_OF);
                aml_local(a, 4);
                aml_local(a, 6);
                aml_op(a, AML_STORE);
                aml_op(a, AML_AND);
                aml_local(a, 6);
                aml_int(a, 0xff);
                aml_byte(a, AML_ZERO);
                byte_at(a, 0, 3, 2);
                aml_op(a, AML_STORE);
                aml_op(a, AML_SHIFT_RIGHT);
                aml_local(a, 6);
                aml_int(a, 8);
                aml_byte(a, AML_ZERO);
                byte_at(a, 0, 3, 3);
                aml_op(a, AML_STORE);
                aml_int(a, 0);
                aml_local(a, 7);
                aml_begin(a, AML_WHILE);
                aml_op(a, AML_LLESS);
                aml_local(a, 7);
                aml_local(a, 6);
                aml_op(a, AML_STORE);
                aml_op(a, AML_DEREF_OF);
                aml_op(a, AML_INDEX);
                aml_local(a, 4);
                aml_local(a, 7);
                aml_byte(a, AML_ZERO);
                aml_op(a, AML_INDEX);
                aml_local(a, 0);
                aml_op(a, AML_ADD);
                aml_op(a, AML_ADD);
                aml_local(a, 3);
                aml_int(a, 4);
                aml_byte(a, AML_ZERO);
                aml_local(a, 7);
                aml_byte(a, AML_ZERO);
                aml_byte(a, AML_ZERO);
                aml_op(a, AML_INCREMENT);
                aml_local(a, 7);
                aml_end(a);
                aml_op(a, AML_ADD);
                aml_local(a, 3);
                aml_op(a, AML_ADD);
                aml_local(a, 6);
                aml_int(a, 4);
                aml_byte(a, AML_ZERO);
                aml_local(a, 3);
            }
            aml_end(a);
            aml_begin(a, AML_ELSE);
            aml_op(a, AML_ADD);
            aml_local(a, 3);
            aml_int(a, 4);
            aml_local(a, 3);
            aml_end(a);
        }
        aml_end(a);
        aml_op(a, AML_INCREMENT);
        aml_local(a, 2);
    }
    aml_end(a);
    aml_op(a, AML_RETURN);
    aml_local(a, 0);
    aml_end(a);
}

static void slot_name(char out[5], size_t slot, char kind) {
    out[0] = 'S';
    out[1] = (char)('0' + slot);
    out[2] = kind;
    out[3] = kind == 'D' ? 'T' : kind == 'L' ? 'N' : kind == 'I' ? 'N' : 'Y';
    out[4] = '\0';
}

// PUTA (slot, object): an argument into its slot.
static void emit_puta(struct aml *a) {
    aml_method(a, "PUTA", 2, 0);
    aml_op(a, AML_STORE);
    aml_op(a, AML_OBJECT_TYPE);
    aml_arg(a, 1);
    aml_local(a, 0);
    for (size_t slot = 0; slot < MBX_SLOT_COUNT; slot++) {
        char type[5];
        char length[5];
        char integer[5];
        char data[5];
        slot_name(type, slot, 'T');
        slot_name(length, slot, 'L');
        slot_name(integer, slot, 'I');
        slot_name(data, slot, 'D');
        aml_begin(a, AML_IF);
        aml_op(a, AML_LEQUAL);
        aml_arg(a, 0);
        aml_int(a, slot);
        aml_op(a, AML_STORE);
        aml_local(a, 0);
        aml_name(a, type);
        if_local_equals(a, 0, TYPE_INTEGER);
        aml_op(a, AML_STORE);
        aml_arg(a, 1);
        aml_name(a, integer);
        aml_end(a);
        aml_begin(a, AML_IF);
        aml_op(a, AML_LOR);
        aml_op(a, AML_LEQUAL);
        aml_local(a, 0);
        aml_int(a, TYPE_STRING);
        aml_op(a, AML_LEQUAL);
        aml_local(a, 0);
        aml_int(a, TYPE_BUFFER);
        aml_op(a, AML_STORE);
        aml_op(a, AML_SIZE_OF);
        aml_arg(a, 1);
        aml_name(a, length);
        aml_op(a, AML_STORE);
        aml_arg(a, 1);
        aml_name(a, data);
        aml_end(a);
        if_local_equals(a, 0, TYPE_PACKAGE);
        aml_op(a, AML_STORE);
        aml_op(a, AML_SIZE_OF);
        aml_arg(a, 1);
        aml_name(a, length);
        aml_op(a, AML_STORE);
        aml_name(a, "SERP");
        aml_arg(a, 1);
        aml_name(a, data);
        aml_end(a);
        aml_end(a);
    }
    aml_end(a);
}

// RRES (): the result the firmware's method gave, which the hypervisor put in
// the mailbox: an integer, a string, made a character at a time as ACPI 1.0
// has no conversion to one, a buffer, or nothing.
static void emit_rres(struct aml *a) {
    aml_method(a, "RRES", 0, 0);
    store_names(a, "MRTY", 0);
    if_local_equals(a, 0, TYPE_INTEGER);
    aml_op(a, AML_RETURN);
    aml_name(a, "MRIN");
    aml_end(a);

    store_names(a, "MRLN", 1);
    store_names(a, "MRDT", 3);
    aml_op(a, AML_STORE);
    aml_int(a, 0);
    aml_local(a, 4);

    if_local_equals(a, 0, TYPE_BUFFER);
    aml_op(a, AML_STORE);
    aml_begin(a, AML_BUFFER);
    aml_local(a, 1);
    aml_end(a);
    aml_local(a, 2);
    aml_begin(a, AML_WHILE);
    aml_op(a, AML_LLESS);
    aml_local(a, 4);
    aml_local(a, 1);
    aml_op(a, AML_STORE);
    aml_op(a, AML_DEREF_OF);
    aml_op(a, AML_INDEX);
    aml_local(a, 3);
    aml_local(a, 4);
    aml_byte(a, AML_ZERO);
    aml_op(a, AML_INDEX);
    aml_local(a, 2);
    aml_local(a, 4);
    aml_byte(a, AML_ZERO);
    aml_op(a, AML_INCREMENT);
    aml_local(a, 4);
    aml_end(a);
    aml_op(a, AML_RETURN);
    aml_local(a, 2);
    aml_end(a);

    if_local_equals(a, 0, TYPE_STRING);
    aml_op(a, AML_STORE);
    aml_string(a, "", 0);
    aml_local(a, 2);
    aml_begin(a, AML_WHILE);
    aml_op(a, AML_LLESS);
    aml_local(a, 4);
    aml_local(a, 1);
    aml_op(a, AML_STORE);
    aml_op(a, AML_DEREF_OF);
    aml_op(a, AML_INDEX);
    aml_local(a, 3);
    aml_local(a, 4);
    aml_byte(a, AML_ZERO);
    aml_local(a, 5);
    // Printable ASCII only, the rest becoming spaces.
    aml_begin(a, AML_IF);
    aml_op(a, AML_LOR);
    aml_op(a, AML_LLESS);
    aml_local(a, 5);
    aml_int(a, 0x20);
    aml_op(a, AML_LGREATER);
    aml_local(a, 5);
    aml_int(a, 0x7e);
    aml_op(a, AML_STORE);
    aml_int(a, 0x20);
    aml_local(a, 5);
    aml_end(a);
    aml_op(a, AML_CONCAT);
    aml_local(a, 2);
    aml_op(a, AML_DEREF_OF);
    aml_op(a, AML_INDEX);
    aml_name(a, "CHRS");
    aml_op(a, AML_SUBTRACT);
    aml_local(a, 5);
    aml_int(a, 0x20);
    aml_byte(a, AML_ZERO);
    aml_byte(a, AML_ZERO);
    aml_local(a, 2);
    aml_op(a, AML_INCREMENT);
    aml_local(a, 4);
    aml_end(a);
    aml_op(a, AML_RETURN);
    aml_local(a, 2);
    aml_end(a);

    if_local_equals(a, 0, TYPE_PACKAGE);
    aml_op(a, AML_RETURN);
    aml_begin(a, AML_PACKAGE);
    aml_byte(a, 0);
    aml_end(a);
    aml_end(a);

    aml_return_int(a, 0);
    aml_end(a);
}

static void emit_rcal(struct aml *a) {
    aml_method(a, "RCAL", 2, 0);
    aml_op(a, AML_STORE);
    aml_arg(a, 0);
    aml_name(a, "MIDX");
    aml_op(a, AML_STORE);
    aml_arg(a, 1);
    aml_name(a, "MARC");
    store_int_name(a, FUNCTION_CALL, "MFUN");
    store_int_name(a, 1, "DOOR");
    aml_op(a, AML_RETURN);
    aml_name(a, "RRES");
    aml_end(a);

    aml_method(a, "GELT", 2, 0);
    aml_op(a, AML_STORE);
    aml_op(a, AML_OR);
    aml_arg(a, 0);
    aml_op(a, AML_SHIFT_LEFT);
    aml_arg(a, 1);
    aml_int(a, 16);
    aml_byte(a, AML_ZERO);
    aml_byte(a, AML_ZERO);
    aml_name(a, "MELT");
    store_int_name(a, FUNCTION_ELEMENT, "MFUN");
    store_int_name(a, 1, "DOOR");
    aml_op(a, AML_RETURN);
    aml_name(a, "RRES");
    aml_end(a);

    aml_method(a, "CALL", 2 + CALL_ARGS, 0);
    aml_op(a, AML_ACQUIRE);
    aml_name(a, "MBXM");
    aml_word(a, 0xffff);
    for (size_t k = 0; k < CALL_ARGS; k++) {
        aml_begin(a, AML_IF);
        aml_op(a, AML_LGREATER);
        aml_arg(a, 1);
        aml_int(a, k);
        aml_name(a, "PUTA");
        aml_int(a, k);
        aml_arg(a, 2 + k);
        aml_end(a);
    }
    aml_op(a, AML_STORE);
    aml_name(a, "RCAL");
    aml_arg(a, 0);
    aml_arg(a, 1);
    aml_local(a, 0);
    aml_op(a, AML_RELEASE);
    aml_name(a, "MBXM");
    aml_op(a, AML_RETURN);
    aml_local(a, 0);
    aml_end(a);
}

// What Linux 7.3's i8042 driver takes for the 8042's devices.
static const char *const keyboard_ids[] = {
    "PNP0300", "PNP0301", "PNP0302", "PNP0303", "PNP0304", "PNP0305",
    "PNP0306", "PNP0309", "PNP030A", "PNP030B", "PNP0320", "PNP0343",
    "PNP0344", "PNP0345", "CPQA0D7", NULL
};
static const char *const mouse_ids[] = {
    "AUI0200", "FJC6000", "FJC6001", "PNP0F03", "PNP0F0B", "PNP0F0E",
    "PNP0F12", "PNP0F13", "PNP0F19", "PNP0F1C", "SYN0801", NULL
};

static uacpi_iteration_decision device_shown(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth
) {
    (void)node;
    (void)depth;
    *(bool *)user = true;
    return UACPI_ITERATION_DECISION_BREAK;
}

static bool firmware_shows(const char *const *ids) {
    bool shown = false;
    uacpi_find_devices_at(uacpi_namespace_root(), ids, device_shown, &shown);
    return shown;
}

static void emit_ps2_device(
    struct aml *a, const char *name, const char *id, const uint8_t *crs,
    size_t crs_size
) {
    aml_begin(a, AML_DEVICE);
    aml_name(a, name);
    aml_name_int(a, "_HID", eisa_id(id));
    aml_op(a, AML_NAME);
    aml_name(a, "_CRS");
    aml_buffer(a, crs, crs_size);
    aml_end(a);
}

static uacpi_iteration_decision window_has_ps2_ports(
    void *user, uacpi_resource *r
) {
    uint64_t minimum = UINT64_MAX;
    uint64_t maximum = 0;
    if (r->type == UACPI_RESOURCE_TYPE_ADDRESS16
     && r->address16.common.type == UACPI_RANGE_IO) {
        minimum = r->address16.minimum;
        maximum = r->address16.maximum;
    } else if (r->type == UACPI_RESOURCE_TYPE_ADDRESS32
            && r->address32.common.type == UACPI_RANGE_IO) {
        minimum = r->address32.minimum;
        maximum = r->address32.maximum;
    } else if (r->type == UACPI_RESOURCE_TYPE_ADDRESS64
            && r->address64.common.type == UACPI_RANGE_IO) {
        minimum = r->address64.minimum;
        maximum = r->address64.maximum;
    }
    if (minimum > PS2_PORT_FIRST || maximum < PS2_PORT_LAST) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    *(bool *)user = true;
    return UACPI_ITERATION_DECISION_BREAK;
}

static uacpi_namespace_node *isa_bridge_below(
    const struct pci_host_bridge *bridge
) {
    uacpi_namespace_node *iter = NULL;
    uacpi_object_type_bits devices = UACPI_OBJECT_DEVICE_BIT;
    while (uacpi_namespace_node_next_typed(bridge->node, &iter, devices)
        == UACPI_STATUS_OK) {
        uint64_t adr;
        if (uacpi_eval_simple_integer(iter, "_ADR", &adr) != UACPI_STATUS_OK) {
            continue;
        }
        struct pci_addr addr = {
            bridge->segment, bridge->bus, (uint8_t)(adr >> 16), (uint8_t)adr
        };
        if (pci_cfg_read(addr, PCI_CLASS, 2) == PCI_CLASS_ISA_BRIDGE) {
            return iter;
        }
    }
    return NULL;
}

// Where firmware has them: a host bridge's window gives its ports to what is
// below it alone, so Windows 7 starts no device with those ports beside it.
static bool ps2_parent(struct path *out) {
    const struct pci_host *host = pci_host_scan();
    for (size_t i = 0; i < host->bridge_count; i++) {
        const struct pci_host_bridge *bridge = &host->bridges[i];
        bool routes = false;
        uacpi_for_each_device_resource(
            bridge->node, "_CRS", window_has_ps2_ports, &routes
        );
        if (routes) {
            uacpi_namespace_node *isa = isa_bridge_below(bridge);
            return guest_path(isa != NULL ? isa : bridge->node, out);
        }
    }
    return false;
}

// An OS looks in the namespace for the 8042's devices, which the guest has
// whatever the firmware says of the machine's.
static void emit_ps2_devices(struct aml *a) {
    // IO (Decode16, 0x60, 0x60, 1, 1), IO (Decode16, 0x64, 0x64, 1, 1),
    // IRQNoFlags () { 1 }
    static const uint8_t keyboard_crs[] = {
        0x47, 0x01, 0x60, 0x00, 0x60, 0x00, 0x01, 0x01,
        0x47, 0x01, 0x64, 0x00, 0x64, 0x00, 0x01, 0x01,
        0x22, 0x02, 0x00, RES_END_TAG, 0x00
    };
    // IRQNoFlags () { 12 }
    static const uint8_t mouse_crs[] = {
        0x22, 0x00, 0x10, RES_END_TAG, 0x00
    };
    struct path parent;
    aml_begin(a, AML_SCOPE);
    if (ps2_parent(&parent)) {
        emit_path(a, &parent);
    } else {
        aml_name(a, "\\_SB");
    }
    if (!firmware_shows(keyboard_ids)) {
        emit_ps2_device(
            a, "RBKB", "PNP0303", keyboard_crs, sizeof(keyboard_crs)
        );
    }
    if (!firmware_shows(mouse_ids)) {
        emit_ps2_device(a, "RBMS", "PNP0F13", mouse_crs, sizeof(mouse_crs));
    }
    aml_end(a);
}

static void emit_mailbox_device(struct aml *a) {
    aml_begin(a, AML_SCOPE);
    aml_name(a, "\\_SB");
    aml_begin(a, AML_DEVICE);
    aml_name(a, "RBMB");
    aml_name_int(a, "_HID", eisa_id("PNP0C02"));
    aml_name_int(a, "_UID", 0x5242);
    // The mailbox, and the guest's configuration space window, which an OS
    // uses only once a motherboard resource claims it.
    uint8_t crs[] = {
        RES_FIXED_MEMORY32, 0x09, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0,
        RES_FIXED_MEMORY32, 0x09, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0,
        RES_END_TAG, 0x00,
    };
    put_le(crs + 4, vacpi.mailbox, 4);
    put_le(crs + 8, MBX_SIZE + DOORBELL_SIZE, 4);
    size_t crs_size = 12;
    const struct pci_plan *plan = pci_plan();
    if (plan != NULL && plan->ecam != 0) {
        put_le(crs + 16, plan->ecam, 4);
        put_le(
            crs + 20, ((uint64_t)plan->ecam_end - plan->ecam_start + 1) << 20,
            4
        );
        crs_size += 12;
    }
    crs[crs_size] = RES_END_TAG;
    crs[crs_size + 1] = 0x00;
    aml_op(a, AML_NAME);
    aml_name(a, "_CRS");
    aml_buffer(a, crs, crs_size + 2);

    aml_op(a, AML_OPERATION_REGION);
    aml_name(a, "MBXR");
    aml_byte(a, AML_REGION_SYSTEM_MEMORY);
    aml_int(a, vacpi.mailbox);
    aml_int(a, MBX_SIZE);
    aml_begin(a, AML_FIELD);
    aml_name(a, "MBXR");
    aml_byte(a, AML_FIELD_DWORD_ACC);
    static const char *const header[] = {
        "MFUN", "MIDX", "MARC", "MSTA", "MRTY", "MRLN", "MRIN", "MELT"
    };
    for (size_t i = 0; i < 8; i++) {
        aml_field_unit(a, header[i], 32);
    }
    aml_field_unit(a, NULL, (MBX_SLOTS - 8 * 4) * 8);
    for (size_t slot = 0; slot < MBX_SLOT_COUNT; slot++) {
        char name[5];
        slot_name(name, slot, 'T');
        aml_field_unit(a, name, 32);
        slot_name(name, slot, 'L');
        aml_field_unit(a, name, 32);
        slot_name(name, slot, 'I');
        aml_field_unit(a, name, 32);
        aml_field_unit(a, NULL, 32);
        slot_name(name, slot, 'D');
        aml_field_unit(a, name, MBX_SLOT_DATA_SIZE * 8);
    }
    aml_field_unit(
        a, NULL, (MBX_RDATA - MBX_SLOTS - MBX_SLOT_COUNT * MBX_SLOT_SIZE) * 8
    );
    aml_field_unit(a, "MRDT", MBX_RDATA_SIZE * 8);
    aml_end(a);

    aml_op(a, AML_OPERATION_REGION);
    aml_name(a, "DBLR");
    aml_byte(a, AML_REGION_SYSTEM_MEMORY);
    aml_int(a, vacpi.doorbell);
    aml_int(a, 4);
    aml_begin(a, AML_FIELD);
    aml_name(a, "DBLR");
    aml_byte(a, AML_FIELD_DWORD_ACC);
    aml_field_unit(a, "DOOR", 32);
    aml_end(a);

    aml_op(a, AML_MUTEX);
    aml_name(a, "MBXM");
    aml_byte(a, 0);
    aml_name_int(a, "PICM", 0);

    aml_op(a, AML_NAME);
    aml_name(a, "CHRS");
    aml_begin(a, AML_PACKAGE);
    aml_byte(a, 0x7f - 0x20);
    for (size_t i = 0x20; i < 0x7f; i++) {
        char c = (char)i;
        aml_string(a, &c, 1);
    }
    aml_end(a);

    emit_serp(a);
    emit_puta(a);
    emit_rres(a);
    emit_rcal(a);
    aml_end(a);
    aml_end(a);
}

static void emit_links(struct aml *a) {
    if (vacpi.links == 0) {
        return;
    }
    aml_begin(a, AML_SCOPE);
    aml_name(a, "\\_SB");
    for (size_t irq = 0; irq < 16; irq++) {
        if ((vacpi.links & ((uint32_t)1 << irq)) == 0) {
            continue;
        }
        static const char hex[] = "0123456789ABCDEF";
        char name[] = "LI00";
        name[2] = hex[irq >> 4];
        name[3] = hex[irq & 0xf];
        aml_begin(a, AML_DEVICE);
        aml_name(a, name);
        aml_name_int(a, "_HID", eisa_id("PNP0C0F"));
        aml_name_int(a, "_UID", irq);
        aml_method(a, "_STA", 0, 0);
        aml_return_int(a, 0x0b);
        aml_end(a);
        // IRQ (Level, ActiveLow, Shared) {irq}: the ELCR has PCI's level.
        uint8_t crs[] = {
            0x23, (uint8_t)((uint32_t)1 << irq),
            (uint8_t)(((uint32_t)1 << irq) >> 8), 0x18, 0x79, 0x00
        };
        aml_op(a, AML_NAME);
        aml_name(a, "_PRS");
        aml_buffer(a, crs, sizeof(crs));
        aml_op(a, AML_NAME);
        aml_name(a, "_CRS");
        aml_buffer(a, crs, sizeof(crs));
        aml_method(a, "_SRS", 1, 0);
        aml_end(a);
        aml_method(a, "_DIS", 0, 0);
        aml_end(a);
        aml_end(a);
    }
    aml_end(a);
}

static void emit_sleep_and_events(struct aml *a) {
    static const struct {
        const char *name;
        uint8_t slp_typ;
    } states[] = { { "\\_S0", 0 }, { "\\_S5", SLP_TYP_S5 } };
    for (size_t i = 0; i < 2; i++) {
        aml_op(a, AML_NAME);
        aml_name(a, states[i].name);
        aml_begin(a, AML_PACKAGE);
        aml_byte(a, 4);
        aml_int(a, states[i].slp_typ);
        aml_int(a, states[i].slp_typ);
        aml_int(a, 0);
        aml_int(a, 0);
        aml_end(a);
    }

    aml_method(a, "\\_PIC", 1, 0);
    aml_op(a, AML_STORE);
    aml_arg(a, 0);
    helper(a, "PICM");
    aml_end(a);

    // NTFY (target, value): a notification from the firmware's AML, to the
    // object the guest has where the firmware has its target.
    aml_begin(a, AML_SCOPE);
    aml_name(a, "\\_SB.RBMB");
    aml_method(a, "NTFY", 2, 0);
    for (size_t i = 0; i < vacpi.target_count; i++) {
        struct path p;
        if (!guest_path(vacpi.targets[i], &p)) {
            continue;
        }
        aml_begin(a, AML_IF);
        aml_op(a, AML_LEQUAL);
        aml_arg(a, 0);
        aml_int(a, i);
        aml_op(a, AML_NOTIFY);
        emit_path(a, &p);
        aml_arg(a, 1);
        aml_end(a);
    }
    aml_end(a);
    aml_end(a);

    // GPE 0 stays asserted while notifications wait, and _L00 takes them all.
    aml_begin(a, AML_SCOPE);
    aml_name(a, "\\_GPE");
    aml_method(a, "_L00", 0, 0);
    aml_op(a, AML_STORE);
    aml_int(a, 1);
    aml_local(a, 2);
    aml_begin(a, AML_WHILE);
    aml_local(a, 2);
    acquire(a);
    aml_op(a, AML_STORE);
    aml_int(a, FUNCTION_NOTIFICATION);
    helper(a, "MFUN");
    aml_op(a, AML_STORE);
    aml_int(a, 1);
    helper(a, "DOOR");
    aml_op(a, AML_STORE);
    helper(a, "MRTY");
    aml_local(a, 2);
    aml_op(a, AML_STORE);
    helper(a, "MIDX");
    aml_local(a, 0);
    aml_op(a, AML_STORE);
    helper(a, "MRIN");
    aml_local(a, 1);
    release(a);
    aml_begin(a, AML_IF);
    aml_local(a, 2);
    helper(a, "NTFY");
    aml_local(a, 0);
    aml_local(a, 1);
    aml_end(a);
    aml_end(a);
    aml_end(a);
    aml_end(a);
}

static void sdt_ids(
    const char signature[4], char oem_id[6], char oem_table_id[8],
    uint32_t *oem_revision
) {
    memcpy(oem_id, "REBIOS", 6);
    memcpy(oem_table_id, "REBIOS  ", 8);
    *oem_revision = 1;
    uacpi_table table;
    if (uacpi_table_find_by_signature(signature, &table) == UACPI_STATUS_OK) {
        memcpy(oem_id, table.hdr->oemid, 6);
        memcpy(oem_table_id, table.hdr->oem_table_id, 8);
        *oem_revision = table.hdr->oem_revision;
        uacpi_table_unref(&table);
    }
}

static void mailbox_place(void);

uint8_t *vacpi_dsdt(size_t *length) {
    if (!vacpi.active) {
        return NULL;
    }
    mailbox_place();
    struct aml a;
    aml_init(&a);
    for (size_t i = 0; i < AML_SDT_HEADER_SIZE; i++) {
        aml_byte(&a, 0);
    }
    acpi_lock();
    uacpi_namespace_for_each_child(
        uacpi_namespace_root(), hide_found, NULL, UACPI_OBJECT_DEVICE_BIT,
        UACPI_MAX_DEPTH_ANY, NULL
    );
    emit_mailbox_device(&a);
    emit_root(&a);
    emit_ps2_devices(&a);
    emit_links(&a);
    emit_sleep_and_events(&a);
    acpi_unlock();

    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    sdt_ids(ACPI_DSDT_SIGNATURE, oem_id, oem_table_id, &oem_revision);
    // Revision 1 keeps the guest's integers to 32 bits, as ACPI 1.0 has them.
    aml_table_header(
        a.data, a.size, "DSDT", 1, oem_id, oem_table_id, oem_revision
    );
    printf(
        "Guest DSDT: %zu bytes, %zu methods, %zu notification targets\n",
        a.size, vacpi.method_count, vacpi.target_count
    );
    *length = a.size;
    return a.data;
}

static void put16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, 2);
}

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

// The guest has no sleep state to resume from, and no firmware to share the
// global lock with.
void vacpi_facs(uint8_t facs[VACPI_FACS_SIZE]) {
    memset(facs, 0, VACPI_FACS_SIZE);
    memcpy(facs, "FACS", 4);
    put32(facs + 4, VACPI_FACS_SIZE);
}

void vacpi_fadt(uint8_t fadt[VACPI_FADT_SIZE], uint32_t facs, uint32_t dsdt) {
    memset(fadt, 0, VACPI_FADT_SIZE);
    put32(fadt + FADT_FIRMWARE_CTRL, facs);
    put32(fadt + FADT_DSDT, dsdt);
    // Dual 8259s or I/O APICs, the MADT telling which the guest has.
    fadt[FADT_INT_MODEL] = 1;
    uint32_t flags = FADT_WBINVD | FADT_PROC_C1 | FADT_SLP_BUTTON
        | FADT_FIX_RTC;
    struct acpi_fadt *host;
    if (uacpi_table_fadt(&host) == UACPI_STATUS_OK) {
        put16(fadt + FADT_SCI_INT, host->sci_int);
        fadt[FADT_DAY_ALRM] = host->day_alrm;
        fadt[FADT_MON_ALRM] = host->mon_alrm;
        fadt[FADT_CENTURY] = host->century;
    }
    if (vacpi.pm1) {
        put32(fadt + FADT_PM1A_EVT_BLK, vacpi.pm1_evt);
        put32(fadt + FADT_PM1A_CNT_BLK, vacpi.pm1_cnt);
        fadt[FADT_PM1_EVT_LEN] = 4;
        fadt[FADT_PM1_CNT_LEN] = 2;
    }
    if (vacpi.pm_tmr != 0) {
        put32(fadt + FADT_PM_TMR_BLK, vacpi.pm_tmr);
        fadt[FADT_PM_TM_LEN] = 4;
        if (vacpi.pm_tmr_32) {
            flags |= FADT_TMR_VAL_EXT;
        }
    }
    if (vacpi.gpe) {
        put32(fadt + FADT_GPE0_BLK, vacpi.gpe0);
        fadt[FADT_GPE0_BLK_LEN] = 2;
    }
    if (!vacpi.power_button) {
        flags |= FADT_PWR_BUTTON;
    }
    // Neither C2 nor C3.
    put16(fadt + FADT_P_LVL2_LAT, 101);
    put16(fadt + FADT_P_LVL3_LAT, 1001);
    put32(fadt + FADT_FLAGS, flags);

    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    sdt_ids(ACPI_FADT_SIGNATURE, oem_id, oem_table_id, &oem_revision);
    aml_table_header(
        fadt, VACPI_FADT_SIZE, "FACP", 1, oem_id, oem_table_id, oem_revision
    );
}

static void sci_update(void) {
    if (!vacpi.sci) {
        return;
    }
    spinlock_acquire(&vacpi.lock);
    bool level = (vacpi.pm1_sts & vacpi.pm1_en & PM1_PWRBTN) != 0
        || (vacpi.queue_count != 0 && (vacpi.gpe0_en & 1) != 0);
    spinlock_release(&vacpi.lock);
    irq_virtual_line(vacpi.sci_gsi, level);
}

static void write_result(uacpi_object *obj, uint32_t status) {
    uint32_t header[4] = { status, TYPE_NONE, 0, 0 };
    uacpi_data_view view = { 0 };
    uint64_t value;
    uacpi_object_array package;
    if (obj != NULL) {
        switch (uacpi_object_get_type(obj)) {
            case UACPI_OBJECT_INTEGER: {
                uacpi_object_get_integer(obj, &value);
                header[1] = TYPE_INTEGER;
                header[3] = (uint32_t)value;
                break;
            }
            case UACPI_OBJECT_STRING:
            case UACPI_OBJECT_BUFFER: {
                uacpi_object_get_string_or_buffer(obj, &view);
                header[1] = uacpi_object_get_type(obj) == UACPI_OBJECT_STRING
                    ? TYPE_STRING : TYPE_BUFFER;
                if (view.length > MBX_RDATA_SIZE) {
                    view.length = MBX_RDATA_SIZE;
                }
                header[2] = (uint32_t)view.length;
                break;
            }
            case UACPI_OBJECT_PACKAGE: {
                uacpi_object_get_package(obj, &package);
                header[1] = TYPE_PACKAGE;
                header[2] = (uint32_t)package.count;
                break;
            }
            default: {
                break;
            }
        }
    }
    guest_phys_write(vacpi.mailbox + MBX_STATUS, header, sizeof(header));
    if (view.length != 0) {
        guest_phys_write(
            vacpi.mailbox + MBX_RDATA, view.const_bytes, view.length
        );
    }
}

// An argument the guest's AML put in a slot, as a uACPI object.
static uacpi_object *slot_object(size_t slot) {
    uintptr_t base = vacpi.mailbox + MBX_SLOTS + slot * MBX_SLOT_SIZE;
    uint32_t head[3];
    guest_phys_read(base, head, sizeof(head));
    uint32_t type = head[0];
    size_t length = head[1];
    uint8_t data[MBX_SLOT_DATA_SIZE];
    if (length > MBX_SLOT_DATA_SIZE) {
        length = MBX_SLOT_DATA_SIZE;
    }
    switch (type) {
        case TYPE_INTEGER: {
            return uacpi_object_create_integer(head[2]);
        }
        case TYPE_STRING:
        case TYPE_BUFFER: {
            guest_phys_read(base + MBX_SLOT_DATA, data, length);
            uacpi_data_view view = { .const_bytes = data, .length = length };
            return type == TYPE_STRING ? uacpi_object_create_string(view)
                : uacpi_object_create_buffer(view);
        }
        case TYPE_PACKAGE: {
            guest_phys_read(base + MBX_SLOT_DATA, data, MBX_SLOT_DATA_SIZE);
            uacpi_object *elements[MAX_PACKAGE];
            size_t count = 0;
            size_t off = 0;
            for (size_t i = 0;
                 i < length && count < MAX_PACKAGE && off + 4 <= sizeof(data);
                 i++) {
                uint8_t etype = data[off];
                uint32_t elength = (uint32_t)data[off + 2]
                    | (uint32_t)data[off + 3] << 8;
                if (off + 4 + elength > sizeof(data)) {
                    break;
                }
                if (etype == TYPE_INTEGER && elength == 4) {
                    uint32_t v;
                    memcpy(&v, data + off + 4, 4);
                    elements[count++] = uacpi_object_create_integer(v);
                } else if (etype == TYPE_BUFFER) {
                    uacpi_data_view view = {
                        .const_bytes = data + off + 4, .length = elength
                    };
                    elements[count++] = uacpi_object_create_buffer(view);
                } else {
                    elements[count++] = uacpi_object_create_integer(0);
                }
                off += 4 + elength;
            }
            uacpi_object_array array = { .objects = elements, .count = count };
            uacpi_object *pkg = uacpi_object_create_package(array);
            for (size_t i = 0; i < count; i++) {
                uacpi_object_unref(elements[i]);
            }
            return pkg;
        }
        default: {
            return uacpi_object_create_integer(0);
        }
    }
}

static void mailbox_call(size_t index, uint32_t argc) {
    if (index >= vacpi.method_count || argc > MBX_SLOT_COUNT) {
        write_result(NULL, 1);
        return;
    }
    uacpi_object *args[MBX_SLOT_COUNT];
    for (size_t i = 0; i < argc; i++) {
        args[i] = slot_object(i);
    }
    uacpi_object_array array = { .objects = args, .count = argc };
    uacpi_object *ret = NULL;
    uacpi_status status = uacpi_eval(
        vacpi.methods[index].node, NULL, argc != 0 ? &array : NULL, &ret
    );
    for (size_t i = 0; i < argc; i++) {
        uacpi_object_unref(args[i]);
    }
    if (status != UACPI_STATUS_OK) {
        const char *path = uacpi_namespace_node_generate_absolute_path(
            vacpi.methods[index].node
        );
        printf("Guest call of %s: %s\n", path, uacpi_status_to_string(status));
        uacpi_free_absolute_path(path);
    }
    if (vacpi.last != NULL) {
        uacpi_object_unref(vacpi.last);
    }
    vacpi.last = ret;
    write_result(ret, status != UACPI_STATUS_OK);
}

static void call_run(uacpi_handle ctx) {
    (void)ctx;
    mailbox_call(vacpi.call_index, vacpi.call_argc);
    __atomic_store_n(&vacpi.call, CALL_DONE, __ATOMIC_RELEASE);
}

static void mailbox_element(uint32_t element) {
    size_t i = element & 0xffff;
    size_t j = element >> 16;
    uacpi_object_array package;
    uacpi_object *obj = NULL;
    if (vacpi.last != NULL
     && uacpi_object_get_package(vacpi.last, &package) == UACPI_STATUS_OK
     && i < package.count) {
        obj = package.objects[i];
        if (j != ELEMENT_NONE) {
            uacpi_object_array inner;
            obj = uacpi_object_get_package(obj, &inner) == UACPI_STATUS_OK
                && j < inner.count
                ? inner.objects[j] : NULL;
        }
    }
    write_result(obj, obj == NULL);
}

static void mailbox_notification(void) {
    uint32_t header[4] = { 0, TYPE_NONE, 0, 0 };
    uint32_t target = 0;
    spinlock_acquire(&vacpi.lock);
    if (vacpi.queue_count != 0) {
        struct notification *n = &vacpi.queue[vacpi.queue_head];
        target = n->target;
        header[1] = TYPE_INTEGER;
        header[3] = n->value;
        vacpi.queue_head = (vacpi.queue_head + 1) % QUEUE_SIZE;
        vacpi.queue_count--;
    }
    spinlock_release(&vacpi.lock);
    guest_phys_write(vacpi.mailbox + MBX_INDEX, &target, sizeof(uint32_t));
    guest_phys_write(vacpi.mailbox + MBX_STATUS, header, sizeof(header));
    sci_update();
}

static uint64_t doorbell_read(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size
) {
    (void)v;
    (void)ctx;
    (void)offset;
    (void)size;
    return 0;
}

static void doorbell_write(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value
) {
    (void)ctx;
    (void)offset;
    (void)size;
    (void)value;
    uint32_t header[8];
    if (!guest_phys_read(vacpi.mailbox, header, sizeof(header))) {
        return;
    }
    switch (header[MBX_FUNCTION / 4]) {
        case FUNCTION_CALL: {
            // The AML task runs the call on any CPU, as the guest's own OSPM
            // would, a slice at each write of the guest's, whose MBXM keeps to
            // one call at a time: a write that finds the call not done does
            // not complete, so that the guest takes its interrupts and writes
            // again.
            uint32_t state = CALL_NONE;
            bool started = __atomic_compare_exchange_n(
                &vacpi.call, &state, CALL_RUNNING, false, __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED
            );
            if (started) {
                vacpi.call_index = header[MBX_INDEX / 4];
                vacpi.call_argc = header[MBX_ARGC / 4];
                uacpi_kernel_schedule_work(
                    UACPI_WORK_NOTIFICATION, vacpi.call_work, call_run, NULL
                );
            }
            acpi_slice();
            state = CALL_DONE;
            bool finished = __atomic_compare_exchange_n(
                &vacpi.call, &state, CALL_NONE, false, __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED
            );
            if (!finished) {
                v->mmio_retry = true;
            }
            break;
        }
        case FUNCTION_ELEMENT: {
            mailbox_element(header[MBX_ELEMENT / 4]);
            break;
        }
        case FUNCTION_NOTIFICATION: {
            mailbox_notification();
            break;
        }
        default: {
            write_result(NULL, 1);
            break;
        }
    }
}

static const struct mmio_ops doorbell_ops = {
    .read = doorbell_read,
    .write = doorbell_write,
};

static void mailbox_place(void) {
    if (vacpi.mailbox != 0) {
        return;
    }
    vacpi.mailbox = layout_take(
        MBX_SIZE + DOORBELL_SIZE, PAGE_SIZE, E820_RESERVED
    );
    if (vacpi.mailbox == 0) {
        panic("No room in the guest's memory for its ACPI mailbox\n");
    }
    vacpi.doorbell = vacpi.mailbox + MBX_SIZE;
    gpt_map(vacpi.doorbell, 0, DOORBELL_SIZE, 0, 0);
    gpt_flush();
    vacpi.call_work = uacpi_kernel_create_work_item();
    mmio_register(vacpi.doorbell, DOORBELL_SIZE, &doorbell_ops, NULL);
}

static uacpi_status notify(
    uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value
) {
    (void)ctx;
    uint32_t target = UINT32_MAX;
    for (size_t i = 0; i < vacpi.target_count; i++) {
        if (vacpi.targets[i] == node) {
            target = i;
            break;
        }
    }
    if (target == UINT32_MAX) {
        const char *path = uacpi_namespace_node_generate_absolute_path(node);
        printf(
            "Notify(%s, %#llx) has no target in the guest\n", path,
            (unsigned long long)value
        );
        uacpi_free_absolute_path(path);
        return UACPI_STATUS_OK;
    }
    spinlock_acquire(&vacpi.lock);
    // An OS without ACPI never takes them.
    if (vacpi.queue_count == QUEUE_SIZE) {
        bool first = !vacpi.dropping;
        vacpi.dropping = true;
        spinlock_release(&vacpi.lock);
        if (first) {
            printf("The guest takes no notifications, dropping them\n");
        }
        return UACPI_STATUS_OK;
    }
    vacpi.dropping = false;
    struct notification *n = &vacpi.queue[(vacpi.queue_head
        + vacpi.queue_count) % QUEUE_SIZE];
    n->target = target;
    n->value = (uint32_t)value;
    vacpi.queue_count++;
    spinlock_release(&vacpi.lock);
    sci_update();
    return UACPI_STATUS_OK;
}

// Until the guest takes the power button over, it powers the machine off, as
// the firmware's does outside ACPI mode.
static uacpi_interrupt_ret power_button(uacpi_handle ctx) {
    (void)ctx;
    spinlock_acquire(&vacpi.lock);
    bool guest = (vacpi.pm1_en & PM1_PWRBTN) != 0;
    vacpi.pm1_sts |= PM1_PWRBTN;
    spinlock_release(&vacpi.lock);
    if (!guest) {
        printf("Power button pressed, with no OS to take it\n");
        power_off();
    }
    sci_update();
    return UACPI_INTERRUPT_HANDLED;
}

static uint32_t bytes_read(uint32_t reg, uint16_t offset, size_t size) {
    uint32_t mask = size == 4 ? (uint32_t)0xffffffff
        : ((uint32_t)1 << (8 * size)) - 1;
    return (uint32_t)((uint64_t)reg >> (8 * offset)) & mask;
}

static void pm1_evt_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in,
    uint32_t *value
) {
    (void)v;
    (void)ctx;
    spinlock_acquire(&vacpi.lock);
    uint32_t reg = vacpi.pm1_sts | (uint32_t)vacpi.pm1_en << 16;
    if (in) {
        *value = bytes_read(reg, offset, size);
    } else {
        for (size_t i = 0; i < size; i++) {
            unsigned int at = offset + i;
            uint8_t b = (uint8_t)(*value >> (8 * i));
            if (at < 2) {
                vacpi.pm1_sts &= (uint16_t)~((uint16_t)b << (8 * at));
            } else if (at < 4) {
                vacpi.pm1_en = (uint16_t)((vacpi.pm1_en
                    & ~(0xff << (8 * (at - 2))))
                    | (uint16_t)b << (8 * (at - 2)));
            }
        }
    }
    spinlock_release(&vacpi.lock);
    if (!in) {
        sci_update();
    }
}

static void pm1_cnt_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in,
    uint32_t *value
) {
    (void)v;
    (void)ctx;
    uint32_t reg = PM1_CNT_SCI_EN
        | (uint32_t)vacpi.slp_typ << PM1_CNT_SLP_TYP_SHIFT;
    if (in) {
        *value = bytes_read(reg, offset, size);
        return;
    }
    uint32_t mask
        = (size == 4 ? (uint32_t)0xffffffff : ((uint32_t)1 << (8 * size)) - 1)
        << (8 * offset);
    uint32_t written = (reg & ~mask) | ((*value << (8 * offset)) & mask);
    vacpi.slp_typ = (uint16_t)((written >> PM1_CNT_SLP_TYP_SHIFT) & 7);
    if ((written & PM1_CNT_SLP_EN) == 0) {
        return;
    }
    if (vacpi.slp_typ == SLP_TYP_S5) {
        printf("The guest powers off\n");
        power_off();
    }
    printf(
        "The guest asked for sleep type %u, which it has no _Sx for\n",
        vacpi.slp_typ
    );
}

static void gpe0_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in,
    uint32_t *value
) {
    (void)v;
    (void)ctx;
    spinlock_acquire(&vacpi.lock);
    // GPE 0 is level-triggered: its status shows notifications waiting.
    uint32_t reg = (vacpi.queue_count != 0 ? (uint32_t)1 : (uint32_t)0)
        | (uint32_t)vacpi.gpe0_en << 8;
    if (in) {
        *value = bytes_read(reg, offset, size);
    } else {
        for (size_t i = 0; i < size; i++) {
            if (offset + i == 1) {
                vacpi.gpe0_en = (uint8_t)(*value >> (8 * i));
            }
        }
    }
    spinlock_release(&vacpi.lock);
    if (!in) {
        sci_update();
    }
}

static bool io_block(const struct acpi_gas *gas, uint16_t *port) {
    if (gas->address == 0
     || gas->address_space_id != UACPI_ADDRESS_SPACE_SYSTEM_IO
     || gas->address > 0xfffc) {
        return false;
    }
    *port = (uint16_t)gas->address;
    return true;
}

void vacpi_init(void) {
    if (!acpi_init()) {
        return;
    }
    acpi_namespace_start();
    vacpi.active = true;

    // The guest's PM1 and GPE registers are where the host's are, trapped.
    struct acpi_fadt *fadt;
    if (uacpi_table_fadt(&fadt) == UACPI_STATUS_OK) {
        vacpi.pm1 = io_block(&fadt->x_pm1a_evt_blk, &vacpi.pm1_evt)
            && io_block(&fadt->x_pm1a_cnt_blk, &vacpi.pm1_cnt);
        vacpi.gpe = io_block(&fadt->x_gpe0_blk, &vacpi.gpe0);
        if (io_block(&fadt->x_pm_tmr_blk, &vacpi.pm_tmr)) {
            vacpi.pm_tmr_32 = (fadt->flags & ACPI_TMR_VAL_EXT) != 0;
        }
        vacpi.power_button = vacpi.pm1 && (fadt->flags & ACPI_PWR_BUTTON) == 0;
    }
    if (vacpi.pm1) {
        struct io_handler evt = {
            .base = vacpi.pm1_evt, .count = 4, .access = pm1_evt_access
        };
        struct io_handler cnt = {
            .base = vacpi.pm1_cnt, .count = 2, .access = pm1_cnt_access
        };
        io_register(&evt);
        io_register(&cnt);
    }
    if (vacpi.gpe) {
        struct io_handler gpe = {
            .base = vacpi.gpe0, .count = 2, .access = gpe0_access
        };
        io_register(&gpe);
    }

    uint32_t irq;
    if (acpi_sci_irq(&irq)) {
        uint32_t gsi = irq < 16 ? irq_isa_gsi(irq) : irq;
        if (gsi != UINT32_MAX) {
            vacpi.sci = true;
            vacpi.sci_gsi = gsi;
            irq_take_host_line(gsi, acpi_sci_interrupt, NULL);
        }
    }
    printf(
        "Guest ACPI: SCI on GSI %d, PM1 at %#x/%#x, GPE0 at %#x, PM timer at "
        "%#x%s\n",
        vacpi.sci ? (int)vacpi.sci_gsi : -1, vacpi.pm1_evt, vacpi.pm1_cnt,
        vacpi.gpe0, vacpi.pm_tmr, vacpi.power_button ? ", power button" : ""
    );

    acpi_lock();
    uacpi_install_notify_handler(uacpi_namespace_root(), notify, NULL);
    if (vacpi.power_button) {
        uacpi_install_fixed_event_handler(
            UACPI_FIXED_EVENT_POWER_BUTTON, power_button, NULL
        );
    }
    acpi_unlock();
}
