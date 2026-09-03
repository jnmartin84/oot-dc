# Ocarina of Time for Dreamcast

## AI Disclosure
The original author of the port, Bruce, used various LLMs to bootstrap this port and produce the PowerVR fast3d interpreter it uses.
I have since used LLMs to rework the build system and update the documentation to accurately reflect those updates.
I am not the original author of this project, I have just offered to clean it up and present it for people to do what they see fit with it.

A native Sega Dreamcast port of *The Legend of Zelda: Ocarina of Time*, built
from the [ZeldaRET decompilation](https://github.com/zeldaret/oot). The game
code is compiled for the SH4 with [KallistiOS](https://github.com/KallistiOS/KallistiOS);
the N64's RSP/RDP graphics are replaced by a display-list interpreter that draws
on the PowerVR, and the game's music and sound effects are played through the
AICA's hardware voices.

The whole build runs with your host and Dreamcast toolchain.
There is no N64 build step and no MIPS toolchain:
the layout metadata the port needs is derived from the ROM.

**No game data is included.** You need your own copy of the game. Only the
GameCube Master Quest debug ROM (`gc-eu-mq-dbg`) is supported at the moment.

## Requirements

* KallistiOS 2.2 or newer, with the `sh4zam` library installed from `kos-ports`
* clang or gcc, libxml2 headers, python3
* GNU make 3.82 or newer (macOS: `brew install make` and use `gmake`)

## Building

```bash
# once: python deps (scipy is required for the audio transcode)
python3 -m venv .venv && .venv/bin/pip install -U pip -r requirements.txt

# your ROM (.z64, .n64 or .v64 all work)
cp /path/to/your/rom.z64 baseroms/gc-eu-mq-dbg/baserom.z64

source /opt/toolchains/dc/kos/environ.sh

make -f Makefile.dc extract    # decompress the ROM, extract assets, generate build inputs (~30s)
make -f Makefile.dc assets     # compile the asset segments for the Dreamcast (a few minutes)
make -j $(nproc) -f Makefile.dc        # build zelda.elf
```

`extract` builds the handful of host tools it needs with your C compiler the
first time it runs. `Makefile.dc` has no dependency tracking, so run
`make -f Makefile.dc clean` after editing source files.

The result is `zelda.elf` plus the runtime asset files in `assets_dc/`. The
game loads those at runtime from `/pc/assets_dc/`, i.e. over dcload's host
filesystem, so run it with dc-tool from the repository root, for example:

```bash
sudo dc-tool-ip -t <dreamcast-ip> -x zelda.elf -c ./
```

## Options

| variable | default | effect |
|---|---|---|
| `DEBUG_FEATURES` | `0` | `1` builds the debug version (map select on boot, debug camera, on-screen debug text) |
| `LTO` | `1` | `0` disables link-time optimisation |
| `DC_VERSION` | `gc-eu-mq-dbg` | ROM version directory under `baseroms/` |

Example: `make -f Makefile.dc DEBUG_FEATURES=1`.

## Layout

* `src/dreamcast/`, `src/linux/`: the Dreamcast and port platform layer (renderer, file loading, audio glue)
* `src/audio/internal/aica_synth.c`, `tools/aica/`: the AICA audio driver and its build-time sample transcoder
* `pc_tools/`: the host-only build pipeline (`dc_extract.py`, `dc_meta.py`, `dc_assets.py`)
* `Makefile.dc`: the Dreamcast build
* everything else (`src/`, `include/`, `assets/`, `spec/`, `tools/`) is the decompilation, trimmed to what this build uses

## Credits

This port would not exist without the work of the ZeldaRET team and
contributors on the [Ocarina of Time decompilation](https://github.com/zeldaret/oot).
See the upstream repository for the decompilation project itself, its
documentation and its contributors.
