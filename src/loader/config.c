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

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>
#include <loader/config.h>
#include <handoff.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/util.h>
#include <loader/efi_glue.h>
#include <loader/files.h>
#include <memory.h>

#define CONFIG_MAX_SIZE 65536

// A line can be as long as its whole source, so quotes of it are cut short.
#define QUOTE_MAX 80
#define QUOTE_FMT "\"%.*s%s\""
#define QUOTE_ARGS(s) \
    (int)((s).len > QUOTE_MAX ? QUOTE_MAX : (s).len), (s).ptr, \
    (s).len > QUOTE_MAX ? "..." : ""

struct config config;

struct span {
    const char *ptr;
    size_t len;
};

struct setting {
    const char *name;
    // Logs what it does. Returns false only if the value is invalid.
    bool (*apply)(const struct setting *setting, struct span value);
    bool *flag;
    uint32_t filter;
};

static unsigned int current_line;

static struct efi_guid file_info_guid = EFI_FILE_INFO_ID;

static const char *const file_names[] = { "rebios.ini", "csmwrap.ini" };

static uint16_t rebios_variable[] = u"ReBIOSConfig";
static struct efi_guid rebios_variable_guid = HANDOFF_VARIABLE_GUID;
static uint16_t csmwrap_variable[] = u"CSMWrapConfig";
static struct efi_guid csmwrap_variable_guid = {
    0x2910aadb, 0xf7ca, 0x42b5, { 0x8c, 0xe2, 0xd6, 0x2d, 0x6a, 0x69, 0xb5, 0xee }
};

static struct span span_before(struct span s, size_t i) {
    return (struct span){ s.ptr, i };
}

static struct span span_after(struct span s, size_t i) {
    return (struct span){ s.ptr + i + 1, s.len - i - 1 };
}

// Returns s.len if c is not found.
static size_t find_char(struct span s, char c) {
    size_t i = 0;
    while (i < s.len && s.ptr[i] != c) {
        i++;
    }
    return i;
}

static bool is_space_or_tab(char c) {
    return c == ' ' || c == '\t';
}

static bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static struct span trim(struct span s, bool (*strip)(char c)) {
    while (s.len > 0 && strip(s.ptr[0])) {
        s.ptr++;
        s.len--;
    }
    while (s.len > 0 && strip(s.ptr[s.len - 1])) {
        s.len--;
    }
    return s;
}

static char to_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool equals_ignoring_case(struct span s, const char *word) {
    for (size_t i = 0; i < s.len; i++) {
        if (word[i] == 0 || to_lower(s.ptr[i]) != to_lower(word[i])) {
            return false;
        }
    }
    return word[s.len] == 0;
}

// Returns -1 for anything but a hexadecimal digit.
static int digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_boolean(struct span s, bool *out) {
    if (equals_ignoring_case(s, "true") || equals_ignoring_case(s, "yes")
     || equals_ignoring_case(s, "1")) {
        *out = true;
        return true;
    }
    if (equals_ignoring_case(s, "false") || equals_ignoring_case(s, "no")
     || equals_ignoring_case(s, "0")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_integer(struct span s, uint32_t *out) {
    uint32_t base = 10;
    if (s.len >= 2 && s.ptr[0] == '0' && (s.ptr[1] == 'x' || s.ptr[1] == 'X')) {
        base = 16;
        s = span_after(s, 1);
    }
    if (s.len == 0) {
        return false;
    }

    uint64_t value = 0;
    for (size_t i = 0; i < s.len; i++) {
        int digit = digit_value(s.ptr[i]);
        if (digit < 0 || (uint32_t)digit >= base) {
            return false;
        }
        value = value * base + (uint32_t)digit;
        if (value > UINT32_MAX) {
            return false;
        }
    }
    *out = (uint32_t)value;
    return true;
}

// Parses one or more hexadecimal digits at *pos, making a value of at most
// max.
static bool parse_hex_field(struct span s, size_t *pos, uint32_t max, uint32_t *out) {
    size_t start = *pos;
    uint32_t value = 0;
    while (*pos < s.len && digit_value(s.ptr[*pos]) >= 0) {
        value = value * 16 + (uint32_t)digit_value(s.ptr[*pos]);
        if (value > max) {
            return false;
        }
        (*pos)++;
    }
    *out = value;
    return *pos > start;
}

static bool skip_char(struct span s, size_t *pos, char c) {
    if (*pos == s.len || s.ptr[*pos] != c) {
        return false;
    }
    (*pos)++;
    return true;
}

static bool parse_pci_address(struct span s, struct handoff_pci_address *out) {
    size_t pos = 0;
    uint32_t bus;
    uint32_t device;
    uint32_t function;
    if (!parse_hex_field(s, &pos, 0xff, &bus) || !skip_char(s, &pos, ':')
     || !parse_hex_field(s, &pos, 0x1f, &device) || !skip_char(s, &pos, '.')
     || !parse_hex_field(s, &pos, 7, &function) || pos != s.len) {
        return false;
    }
    out->segment = 0;
    out->bus = (uint8_t)bus;
    out->devfn = (uint8_t)(device << 3 | function);
    return true;
}

static bool parse_cpu_range(struct span entry, uint32_t *first, uint32_t *last) {
    size_t dash = find_char(entry, '-');
    struct span start = trim(span_before(entry, dash), is_space_or_tab);
    if (!parse_integer(start, first)) {
        return false;
    }
    if (dash == entry.len) {
        *last = *first;
        return true;
    }
    if (!parse_integer(trim(span_after(entry, dash), is_space_or_tab), last)) {
        return false;
    }
    return *first <= *last;
}

static bool parse_cpu_list(struct span s, typeof(config.cpu_ranges[0]) *ranges, size_t *count) {
    *count = 0;
    if (s.len == 0) {
        return true;
    }
    for (;;) {
        size_t comma = find_char(s, ',');
        if (*count == HANDOFF_MAX_CPU_RANGES) {
            return false;
        }
        bool parsed = parse_cpu_range(
            span_before(s, comma), &ranges[*count].first, &ranges[*count].last
        );
        if (!parsed) {
            return false;
        }
        (*count)++;
        if (comma == s.len) {
            return true;
        }
        s = span_after(s, comma);
    }
}

// Lines are logged whole, as the firmware may mirror its console on the log's
// serial port, where the two copies of separate calls would interleave.
__attribute__((format(printf, 1, 2))) static void ignore_line(const char *fmt, ...) {
    char reason[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(reason, sizeof(reason), fmt, args);
    va_end(args);
    printf("  Line %u ignored, %s\n", current_line, reason);
}

static bool apply_boolean(const struct setting *setting, struct span value) {
    bool on;
    if (!parse_boolean(value, &on)) {
        return false;
    }
    *setting->flag = on;
    printf("  %s = %s\n", setting->name, on ? "true" : "false");
    return true;
}

// 0 stands for auto: the port the firmware's tables give.
static bool apply_serial_port(const struct setting *setting, struct span value) {
    if (equals_ignoring_case(value, "auto")) {
        config.serial_port = 0;
        printf("  %s = auto\n", setting->name);
        return true;
    }
    uint32_t port;
    if (!parse_integer(value, &port) || port == 0 || port > 0xffff) {
        return false;
    }
    config.serial_port = (uint16_t)port;
    printf("  %s = %#x\n", setting->name, port);
    return true;
}

static bool apply_serial_baud(const struct setting *setting, struct span value) {
    uint32_t baud;
    if (!parse_integer(value, &baud) || baud == 0) {
        return false;
    }
    config.serial_baud = baud;
    printf("  %s = %u\n", setting->name, baud);
    return true;
}

static bool apply_vgabios(const struct setting *setting, struct span value) {
    size_t len = value.len < CONFIG_VGABIOS_MAX ? value.len : CONFIG_VGABIOS_MAX - 1;
    memset(config.vgabios, 0, sizeof(config.vgabios));
    for (size_t i = 0; i < len; i++) {
        config.vgabios[i] = (unsigned char)value.ptr[i];
    }
    printf("  %s = %.*s%s\n", setting->name, (int)len, value.ptr, len == 0 ? "(none)" : "");
    return true;
}

static bool apply_boot_drive(const struct setting *setting, struct span value) {
    size_t len = value.len < CONFIG_BOOT_DRIVE_MAX ? value.len : CONFIG_BOOT_DRIVE_MAX - 1;
    memset(config.boot_drive, 0, sizeof(config.boot_drive));
    memcpy(config.boot_drive, value.ptr, len);
    printf("  %s = %.*s%s\n", setting->name, (int)len, value.ptr, len == 0 ? "(self)" : "");
    return true;
}

static bool apply_floppy(const struct setting *setting, struct span value, char *out) {
    if (value.len >= HANDOFF_FLOPPY_MODEL_MAX) {
        return false;
    }
    memset(out, 0, HANDOFF_FLOPPY_MODEL_MAX);
    memcpy(out, value.ptr, value.len);
    printf("  %s = %.*s\n", setting->name, (int)value.len, value.ptr);
    return true;
}

static bool apply_floppy_a(const struct setting *setting, struct span value) {
    return apply_floppy(setting, value, config.floppy_a);
}

static bool apply_floppy_b(const struct setting *setting, struct span value) {
    return apply_floppy(setting, value, config.floppy_b);
}

static bool apply_vga(const struct setting *setting, struct span value) {
    struct handoff_pci_address address;
    if (!parse_pci_address(value, &address)) {
        return false;
    }
    config.vga_specified = true;
    config.vga_device = address;
    printf(
        "  %s = %02x:%02x.%x\n", setting->name, address.bus, address.devfn >> 3, address.devfn & 7
    );
    return true;
}

static bool apply_choice(struct span value, const char *const *names, size_t count, uint8_t *out) {
    for (size_t i = 0; i < count; i++) {
        if (equals_ignoring_case(value, names[i])) {
            *out = (uint8_t)i;
            return true;
        }
    }
    return false;
}

// In the order of the HANDOFF_VGA_EMULATION_* values.
static const char *const vga_emulation_names[] = { "auto", "always", "never" };

static bool apply_vga_emulation(const struct setting *setting, struct span value) {
    if (!apply_choice(value, vga_emulation_names, 3, &config.vga_emulation)) {
        return false;
    }
    printf("  %s = %s\n", setting->name, vga_emulation_names[config.vga_emulation]);
    return true;
}

// In the order of the HANDOFF_ZIP_MODE_* values.
static const char *const zip_mode_names[] = { "auto", "floppy", "hdd" };

static bool apply_zip_mode(const struct setting *setting, struct span value) {
    if (!apply_choice(value, zip_mode_names, 3, &config.zip_mode)) {
        return false;
    }
    printf("  %s = %s\n", setting->name, zip_mode_names[config.zip_mode]);
    return true;
}

// In the order of the HANDOFF_VGA_SCALING_* values.
static const char *const vga_scaling_names[] = {
    "stretch", "aspect", "center", "manual", "integer"
};

static bool apply_vga_scaling(const struct setting *setting, struct span value) {
    if (!apply_choice(value, vga_scaling_names, 5, &config.vga_scaling)) {
        return false;
    }
    printf("  %s = %s\n", setting->name, vga_scaling_names[config.vga_scaling]);
    return true;
}

static bool apply_vga_scale(const struct setting *setting, struct span value) {
    uint32_t scale;
    if (!parse_integer(value, &scale) || scale == 0 || scale > 16) {
        return false;
    }
    config.vga_scale = (uint8_t)scale;
    printf("  %s = %u\n", setting->name, scale);
    return true;
}

// In the order of the HANDOFF_VGA_FILTER_* values.
static const char *const vga_filter_names[] = { "nearest", "bilinear" };

static bool apply_vga_filter(const struct setting *setting, struct span value) {
    if (!apply_choice(value, vga_filter_names, 2, &config.vga_filter)) {
        return false;
    }
    printf("  %s = %s\n", setting->name, vga_filter_names[config.vga_filter]);
    return true;
}

// In the order of the HANDOFF_AVIC_* values.
static const char *const avic_names[] = { "auto", "always", "never" };

static bool apply_avic(const struct setting *setting, struct span value) {
    if (!apply_choice(value, avic_names, 3, &config.avic)) {
        return false;
    }
    printf("  %s = %s\n", setting->name, avic_names[config.avic]);
    return true;
}

static bool apply_cpu_list(const struct setting *setting, struct span value) {
    if (config.cpu_filter != HANDOFF_CPU_FILTER_NONE && config.cpu_filter != setting->filter) {
        ignore_line("cpu_allowlist and cpu_blocklist are mutually exclusive");
        return true;
    }

    typeof(config.cpu_ranges) ranges;
    size_t count;
    if (!parse_cpu_list(value, ranges, &count)) {
        return false;
    }
    config.cpu_filter = setting->filter;
    config.cpu_range_count = count;
    memset(config.cpu_ranges, 0, sizeof(config.cpu_ranges));
    memcpy(config.cpu_ranges, ranges, count * sizeof(ranges[0]));

    char list[HANDOFF_MAX_CPU_RANGES * sizeof("4294967295-4294967295, ")];
    size_t len = 0;
    list[0] = 0;
    for (size_t i = 0; i < count; i++) {
        len += (size_t)snprintf(
            list + len, sizeof(list) - len, "%s%u", i == 0 ? "" : ", ", ranges[i].first
        );
        if (ranges[i].last != ranges[i].first) {
            len += (size_t)snprintf(list + len, sizeof(list) - len, "-%u", ranges[i].last);
        }
    }
    printf("  %s = %s\n", setting->name, count == 0 ? "(none)" : list);
    return true;
}

static bool apply_obsolete(const struct setting *setting, struct span value) {
    (void)value;
    ignore_line("%s is obsolete", setting->name);
    return true;
}

static const struct setting settings[] = {
    { .name = "serial", .apply = apply_boolean, .flag = &config.serial },
    { .name = "serial_port", .apply = apply_serial_port },
    { .name = "serial_baud", .apply = apply_serial_baud },
    { .name = "verbose", .apply = apply_boolean, .flag = &config.verbose },
    { .name = "boot_drive", .apply = apply_boot_drive },
    { .name = "floppy_a", .apply = apply_floppy_a },
    { .name = "floppy_b", .apply = apply_floppy_b },
    { .name = "zip_mode", .apply = apply_zip_mode },
    { .name = "vgabios", .apply = apply_vgabios },
    { .name = "vga", .apply = apply_vga },
    { .name = "vga_emulation", .apply = apply_vga_emulation },
    { .name = "vga_scaling", .apply = apply_vga_scaling },
    { .name = "vga_scale", .apply = apply_vga_scale },
    { .name = "vga_filter", .apply = apply_vga_filter },
    { .name = "profile", .apply = apply_boolean, .flag = &config.profile },
    { .name = "avic", .apply = apply_avic },
    { .name = "cpuid_limit", .apply = apply_boolean, .flag = &config.cpuid_limit },
    { .name = "igpu_stolen_remap", .apply = apply_boolean, .flag = &config.igpu_stolen_remap },
    { .name = "legacy_usb", .apply = apply_boolean, .flag = &config.legacy_usb },
    { .name = "save_log", .apply = apply_boolean, .flag = &config.save_log },
    { .name = "pci_high_windows", .apply = apply_boolean, .flag = &config.pci_high_windows },
    { .name = "cpu_allowlist", .apply = apply_cpu_list, .filter = HANDOFF_CPU_FILTER_ALLOW },
    { .name = "cpu_blocklist", .apply = apply_cpu_list, .filter = HANDOFF_CPU_FILTER_BLOCK },
    { .name = "iommu_disable", .apply = apply_obsolete },
    { .name = "nested", .apply = apply_obsolete },
    { .name = "system_thread", .apply = apply_obsolete }
};

static void apply_line(struct span line) {
    line = trim(line, is_blank);
    if (line.len == 0 || line.ptr[0] == ';' || line.ptr[0] == '#') {
        return;
    }

    size_t equals = find_char(line, '=');
    if (equals == line.len) {
        ignore_line("no \"=\" in " QUOTE_FMT, QUOTE_ARGS(line));
        return;
    }
    struct span key = trim(span_before(line, equals), is_blank);
    struct span value = trim(span_after(line, equals), is_blank);
    if (key.len == 0) {
        ignore_line("empty key in " QUOTE_FMT, QUOTE_ARGS(line));
        return;
    }

    for (size_t i = 0; i < ARRAY_COUNT(settings); i++) {
        if (!equals_ignoring_case(key, settings[i].name)) {
            continue;
        }
        if (!settings[i].apply(&settings[i], value)) {
            ignore_line("invalid value " QUOTE_FMT " for %s", QUOTE_ARGS(value), settings[i].name);
        }
        return;
    }
    ignore_line("unknown key " QUOTE_FMT, QUOTE_ARGS(key));
}

static void apply_text(const char *text, size_t size, const char *source) {
    printf("Configuration from %s\n", source);

    size_t len = 0;
    while (len < size && text[len] != 0) {
        len++;
    }

    struct span rest = { text, len };
    current_line = 0;
    while (rest.len > 0) {
        size_t newline = find_char(rest, '\n');
        current_line++;
        apply_line(span_before(rest, newline));
        if (newline == rest.len) {
            break;
        }
        rest = span_after(rest, newline);
    }
}

// For the log, cut short to fit.
static void path_to_ascii(char *out, size_t size, const uint16_t *path) {
    size_t i = 0;
    for (; i + 1 < size && path[i] != 0; i++) {
        out[i] = path[i] >= 0x20 && path[i] < 0x7f ? (char)path[i] : '?';
    }
    out[i] = 0;
}

// Returns the contents of a file fit to apply in a pool allocation, or NULL.
static char *read_file(struct efi_file_protocol *file, const char *source, size_t *size) {
    union {
        struct efi_file_info info;
        uint8_t bytes[EFI_SIZE_OF_EFI_FILE_INFO + 256 * sizeof(uint16_t)];
    } buf;
    size_t buf_size = sizeof(buf);
    efi_status status = file->get_info(file, &file_info_guid, &buf_size, &buf);
    if (status != EFI_SUCCESS) {
        printf("Skipping %s, error %#llx\n", source, (unsigned long long)status);
        return NULL;
    }
    if ((buf.info.attribute & EFI_FILE_DIRECTORY) != 0) {
        printf("Skipping %s, a directory\n", source);
        return NULL;
    }
    if (buf.info.file_size > CONFIG_MAX_SIZE) {
        printf("Skipping %s, larger than 64 KiB\n", source);
        return NULL;
    }
    if (buf.info.file_size == 0) {
        return NULL;
    }

    size_t file_size = (size_t)buf.info.file_size;
    char *text = efi_glue_alloc_pool(file_size);
    if (text == NULL) {
        printf("Skipping %s, out of memory\n", source);
        return NULL;
    }
    size_t done = 0;
    while (done < file_size) {
        size_t chunk = file_size - done;
        status = file->read(file, &chunk, text + done);
        if (status != EFI_SUCCESS) {
            printf("Skipping %s, error %#llx\n", source, (unsigned long long)status);
            efi_bs->free_pool(text);
            return NULL;
        }
        if (chunk == 0) {
            break;
        }
        done += chunk;
    }
    if (done == 0) {
        efi_bs->free_pool(text);
        return NULL;
    }
    *size = done;
    return text;
}

static bool apply_file(struct efi_file_protocol *root, uint16_t *path) {
    char source[256];
    path_to_ascii(source, sizeof(source), path);

    struct efi_file_protocol *file;
    efi_status status = root->open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (status == EFI_NOT_FOUND) {
        return false;
    }
    if (status != EFI_SUCCESS) {
        printf("Skipping %s, error %#llx\n", source, (unsigned long long)status);
        return false;
    }
    size_t size;
    char *text = read_file(file, source, &size);
    file->close(file);
    if (text == NULL) {
        return false;
    }
    apply_text(text, size, source);
    efi_bs->free_pool(text);
    return true;
}

static bool apply_files(void) {
    struct efi_file_protocol *root;
    const struct efi_device_path_protocol *image_path;
    if (!files_open_image_volume(&root, &image_path)) {
        return false;
    }
    bool applied = false;
    for (size_t i = 0; i < ARRAY_COUNT(file_names) && !applied; i++) {
        uint16_t *path = files_sibling_path(image_path, file_names[i]);
        if (path == NULL) {
            break;
        }
        applied = apply_file(root, path);
        efi_bs->free_pool(path);
    }
    root->close(root);
    return applied;
}

// Returns the contents of a variable fit to apply in a pool allocation, or
// NULL.
static char *read_variable(
    uint16_t *name,
    struct efi_guid *guid,
    const char *source,
    size_t *size
) {
    size_t data_size = 0;
    efi_status status = efi_rt->get_variable(name, guid, NULL, &data_size, NULL);
    if (status == EFI_NOT_FOUND) {
        return NULL;
    }
    if (status != EFI_BUFFER_TOO_SMALL && status != EFI_SUCCESS) {
        printf("Skipping %s, error %#llx\n", source, (unsigned long long)status);
        return NULL;
    }
    if (data_size > CONFIG_MAX_SIZE) {
        printf("Skipping %s, larger than 64 KiB\n", source);
        return NULL;
    }
    if (data_size == 0) {
        return NULL;
    }

    char *text = efi_glue_alloc_pool(data_size);
    if (text == NULL) {
        printf("Skipping %s, out of memory\n", source);
        return NULL;
    }
    status = efi_rt->get_variable(name, guid, NULL, &data_size, text);
    if (status != EFI_SUCCESS) {
        printf("Skipping %s, error %#llx\n", source, (unsigned long long)status);
        efi_bs->free_pool(text);
        return NULL;
    }
    if (data_size == 0) {
        efi_bs->free_pool(text);
        return NULL;
    }
    *size = data_size;
    return text;
}

static bool apply_variable(uint16_t *name, struct efi_guid *guid, const char *source) {
    size_t size;
    char *text = read_variable(name, guid, source, &size);
    if (text == NULL) {
        return false;
    }
    apply_text(text, size, source);
    efi_bs->free_pool(text);
    return true;
}

void config_load(void) {
    memset(&config, 0, sizeof(struct config));
    config.serial_port = 0;
    config.serial_baud = 115200;
    config.vga_emulation = HANDOFF_VGA_EMULATION_AUTO;
    config.zip_mode = HANDOFF_ZIP_MODE_AUTO;
    config.vga_scaling = HANDOFF_VGA_SCALING_STRETCH;
    config.vga_scale = 1;
    config.vga_filter = HANDOFF_VGA_FILTER_BILINEAR;
    config.avic = HANDOFF_AVIC_AUTO;
    config.cpu_filter = HANDOFF_CPU_FILTER_NONE;
    config.igpu_stolen_remap = true;
    config.legacy_usb = true;

    bool applied = apply_files()
                || apply_variable(
                       rebios_variable, &rebios_variable_guid, "the ReBIOSConfig variable"
                   )
                || apply_variable(
                       csmwrap_variable, &csmwrap_variable_guid, "the CSMWrapConfig variable"
                   );
    if (applied) {
        return;
    }
    printf("No configuration found, using the defaults\n");
}

void config_to_handoff(struct handoff_config *out) {
    memset(out, 0, sizeof(struct handoff_config));
    out->serial = config.serial;
    out->verbose = config.verbose;
    out->vga_specified = config.vga_specified;
    out->serial_port = config.serial_port;
    out->serial_baud = config.serial_baud;
    out->vga_device = config.vga_device;
    out->vga_emulation = config.vga_emulation;
    out->vga_scaling = config.vga_scaling;
    out->vga_scale = config.vga_scale;
    out->vga_filter = config.vga_filter;
    out->profile = config.profile;
    out->avic = config.avic;
    out->cpuid_limit = config.cpuid_limit;
    out->igpu_stolen_remap = config.igpu_stolen_remap;
    out->legacy_usb = config.legacy_usb;
    memcpy(out->floppy_a, config.floppy_a, sizeof(out->floppy_a));
    memcpy(out->floppy_b, config.floppy_b, sizeof(out->floppy_b));
    out->zip_mode = config.zip_mode;
    out->save_log = config.save_log;
    out->pci_high_windows = config.pci_high_windows;
    out->cpu_filter = config.cpu_filter;
    out->cpu_range_count = config.cpu_range_count;
    for (size_t i = 0; i < config.cpu_range_count; i++) {
        out->cpu_ranges[i].first = config.cpu_ranges[i].first;
        out->cpu_ranges[i].last = config.cpu_ranges[i].last;
    }
}
