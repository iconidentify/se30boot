# se30boot

A boot menu for the Macintosh SE/30 that doesn't need Mac OS, in the spirit of GRUB. It starts from the Mac volume's boot blocks, before Mac OS loads, and offers a menu: Linux, [se30os](https://github.com/iconidentify/se30os), or carry on into Mac OS.

- **What it uses from Mac OS:** only the ROM and the boot disk's own driver.
- **Kernels:** it loads them exactly as Penguin does, so Linux and se30os run unchanged.
- **Tested:** booted se30os on a real SE/30 (68 MB, 32-bit-clean ROM, SCSI2SD). It boots Linux, se30os and Mac OS in MAME.

Companion projects:
- [se30os](https://github.com/iconidentify/se30os), an operating system written from scratch for the SE/30.
- [se30-linux](https://github.com/iconidentify/se30-linux), Debian m68k with Linux 7.2.8 tuned for the SE/30.

## Using it

When the SE/30 starts up, the menu appears after the happy Mac.
- Up/Down or 1–9 choose an entry, and Return boots it.
- E edits the entry's command line for this boot only.
- After the timeout, the default entry boots.

The configuration is `:Linux:se30boot.cfg` on the Mac volume. It's a text file, so you can edit it in SimpleText. Lines starting with `#` are comments.

```
timeout 5                  # seconds before the default boots; -1 waits
default Mac OS 7.1         # an entry's number or title
title Linux
kernel Linux:vmlinux       # a path on the boot volume, gzipped or not
append console=ttyS0,9600 console=tty0
title se30os
kernel Linux:se30os.gz
title Mac OS 7.1
macos                      # carry on starting Mac OS
```

- `memory ADDRESS SIZE` lines (for example `memory 0 64M`) tell kernels about RAM that Mac OS can't show, as in 24-bit mode.
- `debug` shows where the kernel's pieces are and waits for a key before starting it.

## Requirements

- **The machine:** a Macintosh SE/30 starting up from a System 7.1 volume. The installer checks for System 7.1's boot blocks.
- **32-bit addressing:** turn it on in the Memory control panel. In 24-bit mode Mac OS maps only 8 MB, which is too little for Linux.
- **Kernels:** in a `Linux` folder on that volume, the same place Penguin keeps them.

## Building and installing

1. **The build containers.** The build runs in a Debian container with an m68k cross compiler. Build it once:

   ```
   sudo docker build -t se30-build -f docker/Dockerfile.se30-build docker
   sudo docker build -t se30-xbuild -f docker/Dockerfile.se30-xbuild docker
   ```

2. **Build.** `./make.sh` makes `build/stage1.bin` (624 bytes) and `build/se30boot` (25 KB).
3. **Install.** `tools/install-se30boot.sh /dev/sdX` installs on a card or disk. You can give it a disk image instead. It:
   - copies stage 2 to `:Linux:se30boot` and locks it;
   - writes `se30boot.cfg` if the volume has none;
   - unpacks `:Linux:vmlinux.gz` to `:Linux:vmlinux`;
   - points the boot blocks at stage 1.

   Running it again updates everything except an existing configuration. `SE30BOOT_CFG=file` installs a different configuration, replacing the one on the volume.
4. **Remove.** `tools/se30boot.py remove /dev/sdX` restores System 7.1's boot blocks exactly. `tools/se30boot.py info /dev/sdX` shows what is installed.

Back up the card first.

## How it works

- **Stage 1** (`stage1.S`, 214 bytes).
  - The ROM reads the boot volume's two boot blocks and calls them. System 7.1's boot code is only 262 bytes, at 0x8a–0x18f, and the rest of the 1 KB is filler. Stage 1 goes in the filler at 0x190, and the boot blocks' entry branch points at it.
  - It reads stage 2 to 0x700000 with the boot drive's driver (`_Read`), from the volume offset the installer recorded, and jumps to it.
  - If anything is wrong (too little memory, a read error, stage 2 missing), it puts back the registers the ROM called it with and runs System 7.1's own code. The Mac starts as if se30boot weren't there.
- **Stage 2** (`stage2/`, C and 68k assembly).
  - **Screen.** It draws straight into the screen with the 6x13 font.
  - **Files.** It reads HFS through the ROM driver. The catalog and extents B-trees are read a node at a time, and lookups walk the leaf chains.
  - **Keys.** Mac OS's event queue doesn't exist yet when the boot blocks run, so keys come from the ROM keyboard driver's `KeyMap`.
- **Loading a kernel.**
  - Kernels are ELF files, gzipped or not. Each segment goes straight to its place, and a gzipped kernel is unpacked as it is read, through a 32 KB window.
  - Unpacking runs at about 110 KB/s on a 16 MHz 68030, so a 7 MB Linux kernel takes about a minute gzipped and seconds uncompressed. That is why the installer unpacks `vmlinux.gz`.
- **Physical memory.** Mac OS runs with the MMU on, and on an SE/30 with different-sized banks the RAM is not where it appears. With 4 MB in bank A and 64 MB in bank B, logical 4 MB is physical 0x04000000.
  - Like Penguin, se30boot walks Mac OS's translation tables to find the physical RAM and where its own buffers really are.
  - It reads the tables through transparent translation register 0, from a routine in low memory, where logical and physical addresses are the same.
  - Nothing here runs until a kernel is chosen, so the menu and Mac OS depend only on the disk and the screen.
- **Bootinfo, as Penguin 19 builds it.**
  - Memory banks, largest first, with the kernel 4 KB into the first.
  - The command line, model, video, SCC and ROM addresses, boot time, and time zone (from the Map control panel's PRAM).
  - The FPU type, probed with `fsave`: Linux checks the FPU's state frames against it.
- **Starting the kernel.** A mover copied to low memory turns the MMU off, copies the kernel into place (in pieces if memory was fragmented) and jumps to it. The kernel gets `Pen2` in d0, with interrupts and the MMU off.
- **Mac OS.** The Mac OS entry paints the screen gray again, restores the ROM's registers and stack, and runs System 7.1's boot code.

## Testing

se30os's MAME harness boots through se30boot: `SE30BOOT=1 ./test/boot.sh` in [se30os](https://github.com/iconidentify/se30os), with this repo next to it.

- **Choosing a test.** `SE30BOOT_CFG` picks the configuration, and the `kmap CODE` command presses keys by setting `KeyMap` bits.
- **What passes in MAME.**
  - se30os boots to its desktop.
  - Linux boots to its login prompt, from a gzipped or an uncompressed kernel.
  - Mac OS starts normally.
  - The menu and command-line editing work.
  - The kernel image built in memory matched the ELF file byte for byte.
- **Limits of MAME.**
  - MAME's stock SE/30 ROM starts in 24-bit mode, where kernels see 8 MB unless the configuration has a `memory` line.
  - MAME's ADB doesn't update `KeyMap` before Mac OS loads.
  - MAME's RAM is one bank.

The real SE/30 covered all three.

## Licence

MIT, see `LICENSE`. `stage2/font6x13.c` is generated from the public-domain 6x13 font.
