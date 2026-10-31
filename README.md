<h1 align="center">ReBIOS</h1>

<p align="center">
    <a href="https://github.com/ReBIOS-Project/ReBIOS/actions/workflows/check.yml"><img src="https://github.com/ReBIOS-Project/ReBIOS/actions/workflows/check.yml/badge.svg" alt="Build Status"></a>
    <a href="https://discord.gg/t5nn4GKTcx"><img src="https://img.shields.io/discord/1390940493873025074?color=5865F2&label=Discord&logo=discord&logoColor=white" alt="Discord"></a>
    <a href="https://matrix.to/#/#ReBIOS:matrix.org"><img src="https://img.shields.io/matrix/ReBIOS:matrix.org?label=Matrix&logo=matrix" alt="Matrix"></a>
</p>

<p align="center">
    <img src="logo.svg?raw=true" alt="ReBIOS logo" title="ReBIOS logo" width="400">
</p>

ReBIOS is an EFI application designed to be a drop-in solution to enable legacy
BIOS booting on modern UEFI-only (class 3) systems, and a drop-in replacement
for [CSMWrap](https://github.com/CSMWrap/CSMWrap). It runs a Compatibility
Support Module (CSM) build of the [SeaBIOS project](https://www.seabios.org/),
and the legacy OS SeaBIOS boots, under a minimal hypervisor using Intel VT-x or
AMD-V. The hypervisor passes the real hardware through to them, and only
emulates what modern PCs no longer have, effectively recreating a traditional
PC BIOS environment.

Logo art by [conkkerxd](https://github.com/conkkerxd).

## Executive Summary

The idea is to drop ReBIOS into a `/EFI/BOOT/` directory on a FAT (12, 16,
or 32) partition on the medium containing the legacy BIOS OS. UEFI firmware
will pick this up and show the medium as a bootable device. Ideally, that's all
that would be needed.

1. **Download:** Get the latest `rebios.efi` from the
   [Releases page](https://github.com/ReBIOS-Project/ReBIOS/releases), or build
   it as described in [Building](#building).
2. **Deploy:** Copy `rebios.efi` to a FAT-formatted partition as
   `/EFI/BOOT/BOOTX64.EFI` (the hardcoded path is needed so that the firmware
   picks it up automatically).
3. **Boot:** Select the UEFI boot entry for the drive onto which ReBIOS was
   deployed.

SeaBIOS then boots the legacy OS from that same drive first. It is highly
recommended that the partition table used is MBR (MS-DOS partition table), as
UEFI firmwares are perfectly capable of booting off of this format, and because
it is the most compatible with most legacy OSes one may want to boot.

ReBIOS only exists for 64-bit (x86-64) UEFI firmware: there is no
`BOOTIA32.EFI`.

### The ReBIOS USB stick

ReBIOS can also run from a USB stick of its own, which leaves the legacy OS's
drives as they are: they need no FAT partition for ReBIOS, which matters for
OSes installed on other file systems, such as NTFS. The stick takes up a USB
port while the computer runs.

1. **Download:** Get the latest `rebios-usb.img` from the
   [Releases page](https://github.com/ReBIOS-Project/ReBIOS/releases), or build
   it with `make usb` (see [Building](#building)).
2. **Write:** Write the image to a USB stick, raw, with e.g. `dd`
   (`dd if=rebios-usb.img of=/dev/sdX bs=4M`), Rufus, or balenaEtcher. Whatever
   the stick held is lost.
3. **Boot:** Select the UEFI boot entry for the stick.

ReBIOS then boots the computer's other drives, those with something to boot
first: internal drives, then CDs and DVDs, then other USB drives. The
`boot_drive` setting in the stick's `/EFI/BOOT/rebios.ini` names a drive
instead (see [Options](#options)), and SeaBIOS's boot menu, on Esc, picks any
drive for one boot.

The stick's partition is an EFI system partition, which legacy OSes give no
drive letter, so that it moves none of theirs. Linux mounts it as it does any
FAT partition, for editing the configuration; Windows gives it a drive letter
only when asked to, e.g. with `diskpart`'s `assign` command.

## Additional Prerequisites

### Hardware

ReBIOS requires:

1. **Hardware virtualisation:** Intel VT-x with EPT and unrestricted guest
   support, or AMD-V with nested paging.
2. **An IOMMU:** Intel VT-d or AMD-Vi (AMD IOMMU). ReBIOS uses it to map device
   DMA the way the legacy OS sees memory. Without one, ReBIOS says so and
   returns to the firmware. It does the same where the machine's integrated
   graphics malfunctions under VT-d: Intel's Broadwell, and Ironlake in
   Arrandale processors, or in Clarkdale ones whose firmware sets up no shadow
   GTT for VT-d.

### Secure Boot

Secure boot should be disabled unless one wants to manually sign the ReBIOS EFI
application, which is possible, but beyond the scope of this README.

### Firmware Settings

ReBIOS is designed to be as drop-in as possible, but these settings, which most
modern firmware that ReBIOS targets exposes, and often enables by default, must
be enabled:

1. **Virtualisation Technology** (Intel VT-x/VMX, AMD-V/SVM)
2. **IOMMU** (Intel VT-d, AMD-Vi)

Unlike with CSMWrap, there is no need to disable **X2APIC**: the legacy OS gets
an xAPIC of its own, whatever mode the hardware's is in. There is also no need
to disable **Above 4G Decoding** and there *should* be no need to disable
**Resizable BAR** (please report an issue if your configuration only works with
**Resizable BAR** disabled!).

### Video Card Considerations

If the video card has a legacy Video BIOS, ReBIOS has SeaBIOS run it, providing
a pretty much native-like experience.

Otherwise, as on many recent systems, and on cards whose Video BIOS is known
not to work in this setting (AMD's RDNA cards, and its integrated graphics but
for the Vega ones), ReBIOS emulates a complete legacy VGA with its own Video
BIOS, drawn on the UEFI framebuffer and scaled to the screen. It is not
SeaVGABIOS: text modes (40 and 80 columns, 200, 350, and 400 lines, blinking,
underline, the cursor, loadable fonts), CGA, EGA, and VGA graphics modes, and
programs that program the VGA registers and video memory directly (e.g. pretty
much all MS-DOS games, MS-DOS Editor, etc.) work as on a real VGA. On the
tablets and handhelds that Linux knows to have their screen mounted sideways,
the picture is turned upright, as are ReBIOS's own messages.

Its Video BIOS also offers the VESA BIOS Extensions (VBE 3.0): modes in 8, 15,
16, 24, and 32 bits per pixel up to the screen's resolution, through a banked
window or a linear framebuffer, and the display's EDID where the firmware has
it. Operating systems still see the real video card as the only display
adapter, which its own drivers can drive, while the linear framebuffer is in
PCI memory that no device uses.

## Configuration

ReBIOS supports an optional INI-style configuration file. Place a file named
`rebios.ini` in the same directory as the ReBIOS EFI executable (e.g.
`/EFI/BOOT/rebios.ini`). Being a CSMWrap replacement, ReBIOS takes CSMWrap's
configuration as well: it reads, and uses the first it finds of:

1. `rebios.ini` next to the executable,
2. `csmwrap.ini` next to the executable,
3. the `ReBIOSConfig` NVRAM variable,
4. the `CSMWrapConfig` NVRAM variable.

If none is present, sensible defaults are used.

### Setting up NVRAM based config

The NVRAM variable holds the same content as `rebios.ini`, parsed identically:
newline-separated, one `key=value` per line.

Authoring via the UEFI Shell `setvar` command is **not supported**: its
quoted-string argument cannot carry a literal newline. Write the variable from
Linux via efivarfs instead, reusing the exact `rebios.ini` content. It must be
a single `printf` (efivarfs performs a complete variable set on every
`write()`): the `\x07\x00\x00\x00` prefix is the `NV|BS|RT` attribute mask
efivarfs expects, and the double-quoted `%s` argument is emitted verbatim, so
the file's newline separators and backslash paths survive unmangled (the double
quotes are required; without them the shell would word-split the config):

```sh
printf '\x07\x00\x00\x00%s' "$(cat rebios.ini)" > /sys/firmware/efi/efivars/ReBIOSConfig-d559b78d-5494-4c9e-b7f7-e492b0946448
```

### Options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `serial` | bool | `false` | Enable serial debug output |
| `serial_port` | hex/int | `0x3f8` | Serial I/O port address (COM1=`0x3f8`, COM2=`0x2f8`, COM3=`0x3e8`, COM4=`0x2e8`) |
| `serial_baud` | int | `115200` | Serial baud rate |
| `verbose` | bool | `false` | Show debug output on screen while ReBIOS starts |
| `boot_drive` | `self`/`auto`/drive | `self` | The drive SeaBIOS boots first, the others following in the `auto` order. `self`: the drive ReBIOS was started from, if it has a legacy boot sector. `auto`: drives with something to boot (a legacy boot sector, or a bootable CD or DVD) before those without, internal drives before CDs and DVDs before external (USB) drives, and the drive ReBIOS was started from last; the [ReBIOS USB stick](#the-rebios-usb-stick)'s setting. Anything else names a drive by its UEFI device path (e.g. `PciRoot(0x0)/Pci(0x1F,0x2)/Sata(0x1,0xFFFF,0x0)`), or else by part of its model name (e.g. `Samsung SSD 860`), case aside, as ReBIOS lists them when `verbose` is on |
| `vgabios` | string | *(empty)* | Path to a custom VBIOS file on the partition ReBIOS was started from (e.g. `\EFI\ReBIOS\vgabios.bin`). When empty, the card's built-in OpROM is used, and, failing that, the emulated VGA. If vgabios is set to `cbfs`, vgabios.bin is loaded from coreboot's CBFS. If you set it to `cbfs:filename` you can use a custom CBFS filename |
| `vga` | PCI address | *(empty)* | PCI address of the VGA card to use (e.g. `00:02.0`). Format: `BB:DD.F` (hex). When empty, the card the firmware displays on is used, or, if it displays on none, the first card with a VBIOS |
| `vga_emulation` | `auto`/`always`/`never` | `auto` | When to emulate a VGA: when the card has no usable Video BIOS (or, without `vgabios`, when the firmware is Apple's or the screen is mounted sideways), always (even if it has one), or never (a card without a usable Video BIOS then gets none) |
| `vga_scaling` | `stretch`/`aspect`/`integer`/`center`/`manual` | `stretch` | How the emulated VGA fills the screen: all of it; as much of it as 4:3 allows, as a VGA monitor shows every mode; the largest whole number of screen pixels per VGA pixel that fits; one screen pixel per VGA pixel; or `vga_scale` screen pixels per VGA pixel. All but `stretch` center the picture |
| `vga_scale` | int | `1` | The scale factor for `vga_scaling = manual`, from 1 to 16 |
| `profile` | bool | `false` | Log, every 10 seconds, how often the OS made ReBIOS step in and why: VM exits per second and the time taken by reason, and the emulated I/O ports and memory pages behind them. A debugging aid |
| `nested` | bool | `false` | Let the OS use the processor's virtualisation itself, to run virtual machines of its own: Intel's VMX for now, AMD's SVM still to come, hence off by default |
| `avic` | `auto`/`always`/`never` | `auto` | When to have AMD's AVIC run the OS's local APICs, which saves exits to ReBIOS: on Zen 4 and later processors with x2AVIC, as Linux's KVM does; on every processor with AVIC but those of family 17h, where AMD advises against it; or never |
| `cpuid_limit` | bool | `false` | Start the OS with CPUID reporting no basic leaf past 2, as firmware setups' "Limit CPUID Maxval" option does, for OSes that fail on processors with more, such as Windows NT 4.0. On Intel processors that still have that option, the OS sees it set in IA32_MISC_ENABLE and may turn it off |
| `igpu_stolen_remap` | bool | `true` | Give an Intel integrated GPU's stolen memory (the graphics memory the firmware sets aside) an address in the OS's own memory map, and move the firmware's graphics page table entries there, as the IOMMU translates what the GPU accesses. If the screen goes black on an Intel integrated GPU once the OS starts, try `false`: the stolen memory then stays at the firmware's address, as with ReBIOS 0.3.0 |
| `legacy_usb` | bool | `true` | Drive the USB controllers until the OS takes them over, as a BIOS's legacy USB support does: USB keyboards and mice work as PS/2 ones, and the BIOS boots from USB disks. With `false` they are the OS's from the start, and the BIOS has no USB |
| `cpu_allowlist` | int/hex list | *(unset)* | Comma-separated list of APIC IDs of logical CPUs that should be given to the OS. Each entry is either a single ID or an inclusive range `N-M` (e.g. `0,2-4,7`). An empty value (`cpu_allowlist =`) is itself a setting and means "hide every AP" (only the BSP stays visible). The BSP is always given to the OS regardless. Mutually exclusive with `cpu_blocklist` |
| `cpu_blocklist` | int/hex list | *(unset)* | Comma-separated list of APIC IDs of logical CPUs that should be hidden from the OS. Each entry is either a single ID or an inclusive range `N-M` (e.g. `5-7`). An empty value (`cpu_blocklist =`) is a no-op for visibility but still claims the slot, so `cpu_allowlist` cannot also be set. The BSP is always given to the OS regardless. Mutually exclusive with `cpu_allowlist` |
| `iommu_disable` | | | Obsolete CSMWrap setting, ignored with a warning: ReBIOS requires the IOMMU |
| `system_thread` | | | Obsolete CSMWrap setting, ignored with a warning: ReBIOS keeps no CPU for itself |

Hidden CPUs are left out of the MADT and the MP table, and cannot be started by
the OS.

Boolean values accept `true`/`yes`/`1` and `false`/`no`/`0` (case-insensitive).
Comments start with `;` or `#`.

### Example

```ini
; ReBIOS configuration
serial = true
serial_port = 0x3f8
serial_baud = 115200
vgabios = \EFI\ReBIOS\vgabios.bin

; Show the emulated VGA as a 4:3 monitor would, and hide APIC IDs 4 through 6 from the OS.
vga_scaling = aspect
cpu_blocklist = 4-6

; Boot the drive whose model name has "WDC WD5000" in it first.
boot_drive = WDC WD5000
```

## Frequently Asked Questions

### Is this an emulator?

No! At least not in the sense of it being a full-screened emulator window.
ReBIOS uses the CPU's hardware virtualisation to run the legacy OS *natively*
on the system: its code runs on the real CPU, and it drives the real disks, USB
controllers, network cards, and so on. ReBIOS only steps in for what modern
UEFI class 3 systems no longer have: the legacy interrupt controllers, a VGA
when the video card has no legacy Video BIOS, and the like.

### I booted a multi-core capable OS with CSMWrap and was missing a logical processor (thread), what about ReBIOS?

ReBIOS does not keep a logical processor for itself as CSMWrap does: every one
of them is given to the OS (unless `cpu_allowlist` or `cpu_blocklist` say
otherwise), and systems with only one logical processor work too.

### Does ReBIOS have any advantages over native CSM?

Yes! Native CSM firmware is often riddled with issues and hardly tested against
legacy OSes anymore. ReBIOS ships a reliable, free, and open-source legacy BIOS
implementation - SeaBIOS - and it is tested against legacy OSes. Issues
affecting modern, commonly shipped CSM implementations do not affect ReBIOS,
like for example:

- ACPI tables legacy OSes cannot use. Native CSM hands the OS the firmware's
  own tables, written for modern Windows: Windows 2000 and XP stop on their AML
  (STOP 0xA5), and Windows 98 and ME end up installed without ACPI. ReBIOS
  gives the OS ACPI tables of its own, in AML that even Windows 98 understands,
  whose methods call into ReBIOS, which runs the firmware's AML itself.
- Dirty control register values at handoff. (This is something that
  [cregfix](https://github.com/mintsuki/cregfix) was created to work around).
  ReBIOS starts the legacy BIOS with the control registers in their reset
  state.
- Legacy BIOS routines failing to reliably run when called from Virtual 8086
  Mode. EMM386, Windows 3.x under 386 enhanced mode, and more, are affected by
  this issue and it results in crashes. ReBIOS is not affected.

And when it comes to improvements that are not necessarily bugs in CSM
implementations, ReBIOS, amongst other things:

- Generates MP tables for legacy OSes that support the legacy Intel
  MultiProcessor Specification standard but not ACPI, and a PCI IRQ routing
  table ($PIR).
- Allows one to select a non-primary video card for VGA output which native CSM
  implementations do not allow. This is useful for multi-booting modern and
  legacy OSes.

### Does ReBIOS have any advantages over CSMWrap?

Yes! CSMWrap starts SeaBIOS on the bare hardware as the UEFI firmware leaves
it. ReBIOS runs it in a virtual machine that is the real machine, but for what
it adds back of the legacy PC:

- **ACPI for legacy OSes.** CSMWrap passes the firmware's ACPI tables on as
  they are, and with them AML written for modern Windows, on which Windows 2000
  and XP stop (STOP 0xA5) and without which Windows 98 and ME must be
  installed. ReBIOS runs the firmware's ACPI itself and gives the OS ACPI
  tables of its own, in AML that ACPI 1.0 interpreters such as Windows 98's
  understand, whose methods call into ReBIOS to run the firmware's: the OS
  still sees the machine's devices and gets their events, and the memory the
  firmware keeps for its AML no longer breaks up the OS's.
- **A real VGA without a legacy Video BIOS.** Where CSMWrap has to fall back to
  SeaVGABIOS, with which only text written through the BIOS shows, ReBIOS
  emulates a complete VGA, with VBE modes up to the screen's resolution (see
  [Video Card Considerations](#video-card-considerations)).
- **Legacy interrupt controllers.** The 8259 PICs, the I/O APICs, and the local
  APICs the OS sees are virtual, so DOS-era software works on machines whose
  chipset lacks the 8259s or locks x2APIC mode on, and ReBIOS itself routes PCI
  interrupts to free ISA IRQs for PIC mode, whatever the firmware's PIC mode
  wiring.
- **A PC's memory map.** The OS gets memory from 1 MiB up to 4 GiB as
  contiguous as the firmware's reserved ranges allow, wherever the machine's
  memory actually is; its own copy of the SMBIOS tables; the option ROMs and
  the BIOS write-protected, as a BIOS would leave them; and the upper memory
  that no ROM uses free for memory managers to find (e.g. for JemmEx's UMBs and
  EMS page frame).
- **DMA from anywhere.** The IOMMU maps device DMA exactly as the OS sees
  memory, so the BIOS's buffers in upper memory, and the buffers of legacy
  drivers wherever they are, work.
- **Virtual 8086 mode without a system thread.** SeaBIOS reaches its 32-bit
  code from V86 mode through the hypervisor, and disk transfers into EMS pages
  work.
- **Every processor.** No logical processor is kept for ReBIOS itself.
- **A boot drive of choice.** SeaBIOS boots first whichever drive the
  configuration names, by model or device path, or picks one by itself, so
  ReBIOS can also run from a USB stick of its own, sparing legacy OS
  installations a FAT partition for it (see
  [The ReBIOS USB stick](#the-rebios-usb-stick)).

## Building

Building requires GCC, GNU binutils, `python3`, and `git`, used to fetch the
dependencies (see `get-deps`). `curl` is used to download the EDK2 OVMF
firmware images for `make run`.

```sh
./get-deps
make
```

`make usb` also builds `bin/rebios-usb.img`, the image for
[the ReBIOS USB stick](#the-rebios-usb-stick), which needs mtools.

Release tarballs (`rebios-<version>.tar.gz` and `.tar.xz`) come with the
dependencies, so they build without `./get-deps`. `make dist` creates them from
the current commit. The source archives GitHub generates for each release lack
the dependencies, and do not build.

Assembly files with the `*.S` extension are built using the same toolchain as
the C sources. Only `*.asm` files, of which ReBIOS ships none, require `nasm`.

The `TOOLCHAIN_PREFIX` make variable can be used to set the toolchain. For
example:

```sh
make TOOLCHAIN_PREFIX=x86_64-elf-
```

The program is linked through the compiler driver, using its default linker. A
different linker can be picked by adding a `-fuse-ld=` option to `LDFLAGS`.

Link-time optimisation can be enabled by adding `-flto` to `CFLAGS`:

```sh
make CFLAGS='-g -O2 -pipe -flto'
```

SeaBIOS is built with the GNU tools named by `SEABIOS_TOOLCHAIN_PREFIX`, which
follows `TOOLCHAIN_PREFIX`, or by `SEABIOS_CC`, `SEABIOS_LD`,
`SEABIOS_OBJCOPY`, `SEABIOS_OBJDUMP`, and `SEABIOS_STRIP` individually.
`HOST_CC` builds its configuration tools.

The Video BIOS is built likewise, with the GNU tools named by
`VGABIOS_TOOLCHAIN_PREFIX`, or by `VGABIOS_CC` and `VGABIOS_OBJCOPY`, and with
the flags `VGABIOS_CFLAGS` (by default `-g -O2 -pipe`), `VGABIOS_CPPFLAGS`, and
`VGABIOS_LDFLAGS` rather than `CFLAGS`, `CPPFLAGS`, and `LDFLAGS`. It is always
built with `-Os`, as it has to fit in 32 KiB.

## Contributing

Contributions are welcome! Whether it's reporting bugs, suggesting features,
improving documentation, or submitting code changes, your help is appreciated.

Additionally, one can join our [Discord server](https://discord.gg/t5nn4GKTcx)
or our [Matrix room](https://matrix.to/#/#ReBIOS:matrix.org) for any
project-related discussion, or to otherwise chat with likeminded people.

## Credits & Acknowledgements

*   The **[SeaBIOS project](https://www.seabios.org/)** for their CSM code.
*   **[CSMWrap](https://github.com/CSMWrap/CSMWrap)**, which ReBIOS replaces,
    for its SeaBIOS changes and its knowledge of firmware and hardware quirks.
*   **[PicoEFI](https://github.com/PicoEFI/PicoEFI)** for the EFI C runtime,
    build system, and headers.
*   **[uACPI](https://github.com/uACPI/uACPI)** for ACPI table handling and AML
    evaluation.
*   **[nanoprintf](https://github.com/charlesnicholson/nanoprintf)** for
    formatted output.
*   **[Flanterm](https://github.com/Mintsuki/Flanterm)** for the console ReBIOS
    shows its output on.
*   **[Tendril](https://github.com/Mintsuki/Tendril)** for driving USB.
*   **[QEMU](https://www.qemu.org/)**, whose VGA BIOS the emulated VGA's fonts
    (public domain fonts from Joseph Gil's fntcol16 collection) and mode
    register values were taken from.
*   **[EDK2 (TianoCore)](https://github.com/tianocore/edk2)** for the OVMF
    firmware ReBIOS is tested on.
*   All contributors and testers from the community!

## License

ReBIOS is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License version 2 as published by the Free
Software Foundation. See [LICENSE](LICENSE).

ReBIOS builds in software of others, under their own licenses:

- [SeaBIOS](https://www.seabios.org/), under the GNU LGPL, version 3: a
  separate program, which ReBIOS carries and runs as the legacy BIOS of the
  machine it virtualises.
- [uACPI](https://github.com/uACPI/uACPI), by Daniil Tatianin, under the MIT
  license.
- [PicoEFI](https://github.com/PicoEFI/PicoEFI),
  [freestanding-c-hdrs](https://github.com/osdev0/freestanding-c-hdrs), and
  [freestanding-lgrt](https://github.com/osdev0/freestanding-lgrt), by Mintsuki
  and contributors, under the 0BSD license.
- [nanoprintf](https://github.com/charlesnicholson/nanoprintf), by Charles
  Nicholson, under the Unlicense or the 0BSD license.
- [Flanterm](https://github.com/Mintsuki/Flanterm), by Mintsuki and
  contributors, under the BSD 2-Clause license.
- [Tendril](https://github.com/Mintsuki/Tendril), by Mintsuki and contributors,
  under the BSD 2-Clause license, with the notices of what it draws on in its
  `3RDPARTY.md`.
