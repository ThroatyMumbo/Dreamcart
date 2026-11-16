# The Dreamcart!

<img src="images/thing.png" alt="Dreamcart 64" width="320">

Not to be confused with the piece of furniture of the same name...

This is a set of cartridge readers for Dreamcast that supports N64, SNES, and Genesis (or Mega Drive if that's your fancy).
It can be used to play your physical game copies (complete with save data!) via an emulator or through a dynamic recompiler (for N64 only).

Video demo: https://youtu.be/mrIwJNA4yfQ

## Disclaimer

Since these readers use actual cartridges, exercise caution and **use at your own risk**.
After building the reader (or using an equivalent bench setup), always test it on a throwaway cartridge first and backup your save files if possible.
Throughout all my testing I personally haven't lost any save data *yet*, but the risk is still there.

Some existing cartridge readers and backup tools:
- [Open Source Cartridge Reader (OSCR)](https://github.com/sanni/cartreader)
- [Retrode 2](https://www.retrode.com/)
- [Epilogue SN Operator](https://epilogue.co/product/sn-operator)

## Requirements

### Hardware
- A Dreamcast (duh)
- Any of the cart readers (PCBs under [hardware/][hardware/]) + cartridges
   - For N64: This can really be any RP2350B-based MCU hooked up directly to a cartridge slot with the proper wiring. Not ideal but it does work.
- [dcload-ip](https://github.com/KallistiOS/dcload-ip) burned to a disc
- A CMSIS-DAP debug probe (e.g. Raspberry Pi Debug Probe) for loading the firmware

### Software
- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) for building the firmware
- [OpenOCD](https://github.com/raspberrypi/openocd) with RP2350 support for loading it
- [KallistiOS](https://github.com/KallistiOS/KallistiOS) and the built toolchain
- dc-tool-ip for loading software onto the Dreamcast (I typically use Ethernet via the BBA)
  - You could *probably* burn the software to a disc and run it directly, but I haven't tested this.

## Usage

### Firmware

Each cart reader has its own firmware under `firmware/`, sharing code from `firmware/common/`:

| Reader  | Directory           | Output        |
|---------|---------------------|---------------|
| N64     | `firmware/n64`      | `n64link.elf` |
| SNES    | `firmware/snes`     | `sneslink.elf`|
| Genesis | `firmware/genesis`  | `mdlink.elf`  |

#### Building

Needs the Pico SDK (2.x), CMake, Ninja and `arm-none-eabi-gcc`. The board type defaults to `pimoroni_pga2350`.

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S firmware/n64 -B firmware/n64/build -G Ninja
cmake --build firmware/n64/build
```

Swap `n64` for `snes` or `genesis` to build the other readers.

#### Loading

The firmware is loaded over SWD with a CMSIS-DAP probe (e.g. a Raspberry Pi Debug Probe) wired to the reader's `SWD+UART` header, using an OpenOCD build with RP2350 support:

```sh
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000" \
  -c "program firmware/n64/build/n64link.elf verify reset exit"
```

Power the reader before connecting the probe. The firmware's console is on UART1 (GP40 TX / GP41 RX, 115200 baud), on the same header.

Flashing over USB via bootselect can be done as well, but SWD is typically easier and allows for repeated flashing without the bootselect song and dance (or USB becoming unresponsive, which it loves to do).

### Dreamcart 64

This assumes you're using dcload-ip for running custom Dreamcast software.

1) Run dcload-ip on your Dreamcast
2) Setup environment vars:
```sh
source /opt/toolchains/dc/kos/environ.sh # KallistiOS env
export DC_IP=192.168.*.* # Use the IP address shown on DC screen
```
3) Launch it
```sh
recomp/run.sh
```

You should see "Dreamcart 64" which auto-detects the cart reader and should display the title of the cartridge.
- Titles might be slightly off since it reads it from the cartridge header and does some formatting.
- If the title shows up as a jumbled mess or "No cartridge" then you'll have to "Swap cartridge" and reseat (the joys of cartridges!)

## FAQ

### Which games does this support?
Of the N64 games I own and have tested:

| Game | Supported? |
|------|------------|
| Super Mario 64 | Yes |
| Mario Kart 64 | Yes |
| Kirby 64 | Yes |
| Rayman 2 | Yes |
| Cruis'n USA | Yes |
| Cruis'n World | Yes |
| Elmo's Number Journey | Yes |
| Pokémon Stadium 2 | No |
| Donkey Kong 64 | No |

For SNES and Genesis it's up to what the emulators support. I've tested Donkey Kong Country, Super Mario All Stars, and Mortal Kombat II (on Genesis). The only issues have been sound-related.

### Why not just use ROMs?
Besides the legally dubious nature of ROMs themselves, I like the idea of being able to access an exact copy of a game from different places,
more specifically the save data. No juggling save files around or dealing with the logistics of how to transfer a save or ROM from one place to
another. Also, using actual cartridges is cooler.

### What about games that require a save pak?
The Dreamcast VMU emulates the save pak using 66 blocks (32 KB). If you don't have room for that you'll get a warning before starting any
game (haven't figured out a clean way to check if the game actually requires one or not).

As for transferring data to/from an actual save pak, this is technically possible but not supported in this project.
- FWIW, one way to do this is to hook up an N64 controller to an MCU (potentially even the cartridge reader itself, but I haven't tested this)
  and use the Joybus PIO to read the save pak data off the controller while the save pak is inserted.
  Then you can use a custom DC program to copy that to the VMU via dcload-ip's PC link. So yeah, technically possible but very clunky.

### How do I get SNES and Genesis games working?
These require a modified version of [Super Famicast](https://github.com/sega-dreamcast/super-famicast) and/or [Genesis Plus DC](https://github.com/nextgeniuspro/genesisplus-dc).
Still working on adding a clean set of patches to this repo to support the cartridge reader stuff.

I don't plan on adding a native recompiler for these platforms. N64 is the main focus.

### What's the SD card for?
This is an optional large size cache for recompiled games from the Dreamcart 64 to reduce boot times. The PGA2350's 16 MB cache could be used as well, but obviously that fills up fast.
It's currently not used for anything on the SNES and Genesis readers.

## Credits and References

### Firmware

- **[Open Source Cartridge Reader (OSCR)](https://github.com/sanni/cartreader)**: The main reference for the core cartridge reader firmware.
- **[MaplePad](https://github.com/mackieks/MaplePad)**: Core Dreamcast controller protocol (Maple Bus) code that I've used successfully in several projects.
- **[no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)**: Used for the optional SD card recompiled game cache

### N64 Recompiler

- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)**: The main reference for the recompiler. Would not be feasible otherwise.
- **[rabbitizer](https://github.com/Decompollaborate/rabbitizer)** (MIT): MIPS decoder, vendored in `recomp/rabbitizer`
- **[mupen64plus-rsp-hle](https://github.com/mupen64plus/mupen64plus-rsp-hle)** (GPLv2): audio microcode primitives, vendored in `recomp/mupen64plus-rsp-hle`
- **[DaedalusX64](https://github.com/DaedalusX64/daedalus)**: libultra function signatures (`hooks/patch_symbols.inl`)
- **[ultralib](https://github.com/decompals/ultralib)**: libgultra signatures (`hooks/libgultra.inl`)
- **[ares](https://github.com/ares-emulator/ares)**: reference for PIF and Controller Pak behavior
- **[f3dex2](https://github.com/Mr-Wiseguy/f3dex2)**: F3DEX2 microcode reference
- **[LZ4](https://github.com/lz4/lz4)**: block format used for the translation cache
- **[dcload-ip](https://github.com/KallistiOS/dcload-ip)**: loading and console I/O over the BBA
