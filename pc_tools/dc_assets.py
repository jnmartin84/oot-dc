#!/usr/bin/env python3
import os
import subprocess
import re
import shutil

# Dreamcast toolchain
CC = "sh-elf-gcc"
AS = "sh-elf-as"
LD = "sh-elf-ld"
OBJCOPY = "sh-elf-objcopy"
NM = "sh-elf-nm"
ENDIAN = "-EL"

# Check toolchain exists
if not shutil.which(CC):
    print(f"ERROR: {CC} not found in PATH!")
    print("Make sure you've sourced the KOS environment:")
    print("  source /opt/toolchains/dc/kos/environ.sh")
    exit(1)

import sys

# Every layout input (segment bases, VROM layout, audio symbols, spec) is generated
# on the host by pc_tools/dc_meta.py from the ROM + spec/spec -- no MIPS toolchain,
# any supported ROM. The VROM layout is recomputed from DC segment sizes (option A).
# The extracted/<v> + build/<v> inputs come from pc_tools/dc_extract.py.
# (The former mode that read build/<v>/{.ld,.map,.elf,.z64} from a MIPS build was
# REMOVED 2026-09-03 and must not come back.)
VERSION = os.environ.get("DC_VERSION", "gc-eu-mq-dbg")
if "--version" in sys.argv:
    VERSION = sys.argv[sys.argv.index("--version") + 1]

EXTRACTED = f"extracted/{VERSION}"
BUILD_DIR = "build_dc/assets"
DATA_DIR = "assets_dc"

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dc_meta
print(f"generating layout metadata for {VERSION} from the ROM (dc_meta)")
_meta = dc_meta.gather(VERSION)
SPEC_PATH = _meta["spec_path"]
_asset_defs = " ".join(dc_meta.cpp_defines(_meta["cfg"]))
CFLAGS = ("-c -g -O0 -fno-common -fno-toplevel-reorder -fno-zero-initialized-in-bss "
          "-D_LANGUAGE_C -DLINUX -D__DREAMCAST__ -DASSET_BUILD "
          "-DF3DEX_GBI_2 -DF3DEX_GBI_PL -DGBI_DOWHILE -DGBI_DEBUG "
          f"{_asset_defs} "
          f"-Iinclude -Iextracted/{VERSION} -Ibuild_dc/meta -Ibuild/{VERSION} -I.")

os.makedirs(BUILD_DIR, exist_ok=True)
os.makedirs(DATA_DIR, exist_ok=True)
os.makedirs("src/dreamcast", exist_ok=True)

print(f"=== Building Dreamcast assets ===\n")

# =============================================================================
# Segment bases (dc_meta: cpp spec | mkldscript)
# =============================================================================
segment_bases = {}
# dc_meta returns ints; downstream linker scripts want hex strings.
segment_bases = {k: hex(v) for k, v in _meta["segment_bases"].items()}

print(f"Found {len(segment_bases)} segment bases")

# =============================================================================
# VROM layout (option A, dc_meta) + D_ linker constants
# =============================================================================
rom_segments = {}
all_segments_map = {}
linker_symbols = {}
linker_symbols = dict(_meta["linker_symbols"])

# Option A: recompute the DC VROM layout cumulatively from DC segment sizes.
# gDmaDataTable (via _NAMESegmentRomStart/End) and gVromTable both come from
# this single layout, so DMA lookups stay consistent by construction. VRAM
# (non-rom) boundaries come from the fixed segment_bases (segment slots).
def optionA_maps(sizes):
    # Segments dc_assets does not compile (makerom/boot/code/...) fall back to
    # their baserom sizes so their VROM slots stay realistic; compiled asset
    # segments (and audio) override with real DC sizes.
    merged = dict(_meta["baserom_sizes"])
    merged.update(sizes)
    layout = dc_meta.compute_vrom_layout(
        _meta["dma_order"], merged, _meta["romaligns"])
    asm, rs = {}, {}
    for name, (s, e) in layout.items():
        asm[(name, True)] = {'Start': s, 'End': e}
        rs[name] = {'Start': s, 'End': e}
    for name, base in _meta["segment_bases"].items():
        asm[(name, False)] = {'Start': base, 'End': base + sizes.get(name, 0)}
    return asm, rs

# Seed with zero sizes for the PHASE-3 pass (VROM values there are unused);
# refreshed with real DC sizes just before PHASE 6.
all_segments_map, rom_segments = optionA_maps({})

print(f"Found {len(rom_segments)} ROM segments")

# =============================================================================
# Audio sub-segment symbols (dc_meta: audio_tables.json + ROM sequence table)
# =============================================================================
audio_symbols = {}
audio_symbols = dict(_meta["audio_symbols"])

print(f"Found {len(audio_symbols)} audio sub-segment symbols")

# =============================================================================
# Parse spec for segment -> source mapping
# =============================================================================
segments = {}
seg_name = None
with open(SPEC_PATH, 'r') as f:
    for line in f:
        line = line.strip()
        if 'beginseg' in line:
            seg_name = None
        elif line.startswith('name'):
            seg_name = line.split('"')[1]
            segments[seg_name] = []
        elif line.startswith('include') and 'assets/' in line:
            build_path = line.split('"')[1]
            possible_paths = [
                build_path.replace('build/', 'extracted/').replace('.o', '.c'),
                build_path.replace(f'build/{VERSION}/', '').replace('.o', '.c'),
                build_path.replace(f'build/{VERSION}/', f'extracted/{VERSION}/').replace('.o', '.c'),
            ]
            c_path = None
            for p in possible_paths:
                if os.path.exists(p):
                    c_path = p
                    break
            if seg_name and c_path:
                segments[seg_name].append(c_path)
        elif 'endseg' in line:
            seg_name = None

segments = {k: v for k, v in segments.items() if v}
print(f"Found {len(segments)} asset segments")

# =============================================================================
# PHASE 1: Compile all segments
# =============================================================================
print("\n=== Compiling segments ===")
compiled = {}
compile_failed = []
first_error_shown = False

total = len(segments)
for i, (seg_name, c_paths) in enumerate(segments.items(), 1):
    print(f"\r[{i}/{total}] {seg_name} ({len(c_paths)} files)...                    ", end="", flush=True)
    o_paths = []
    failed = False
    
    for j, c_path in enumerate(c_paths):
        o_path = f"{BUILD_DIR}/{seg_name}_{j}.o"
        
        cmd = f"{CC} {CFLAGS} {c_path} -o {o_path}"
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        if r.returncode == 0:
            o_paths.append(o_path)
        else:
            if not first_error_shown:
                print(f"\n\n=== FIRST COMPILE ERROR ===")
                print(f"Command: {cmd}")
                print(f"Exit code: {r.returncode}")
                print(f"STDERR:\n{r.stderr}")
                print(f"STDOUT:\n{r.stdout}")
                print("=" * 40 + "\n")
                first_error_shown = True
            compile_failed.append((seg_name, c_path, r.stderr[:500]))
            failed = True
            break
    if not failed and o_paths:
        compiled[seg_name] = o_paths

print(f"\nCompiled: {len(compiled)}, Failed: {len(compile_failed)}")

# =============================================================================
# PHASE 2: Build symbol dependency map
# =============================================================================
print("\n=== Building symbol dependency map ===")
segment_defines = {}
segment_needs = {}
symbol_to_segment = {}

total = len(compiled)
for i, (seg_name, o_paths) in enumerate(compiled.items(), 1):
    print(f"\r[{i}/{total}] {seg_name}...                    ", end="", flush=True)
    defined = set()
    undefined = set()
    
    for o_path in o_paths:
        r = subprocess.run(f"{NM} {o_path}", shell=True, capture_output=True, text=True)
        if r.returncode == 0:
            for line in r.stdout.strip().split('\n'):
                parts = line.split()
                if len(parts) >= 3 and parts[1] in 'TDRBCdtrb':
                    defined.add(parts[2])
                    symbol_to_segment[parts[2]] = seg_name
        
        r = subprocess.run(f"{NM} -u {o_path}", shell=True, capture_output=True, text=True)
        if r.returncode == 0:
            for line in r.stdout.strip().split('\n'):
                if line.strip():
                    undefined.add(line.split()[-1])
    
    segment_defines[seg_name] = defined
    segment_needs[seg_name] = undefined

all_defined = set(symbol_to_segment.keys())
print(f"\nFound {len(all_defined)} defined symbols across all segments")

def get_dependencies(seg_name):
    needs = segment_needs.get(seg_name, set())
    deps = set()
    for sym in needs:
        if sym in symbol_to_segment:
            dep_seg = symbol_to_segment[sym]
            if dep_seg != seg_name:
                deps.add(dep_seg)
    return deps

# =============================================================================
# PHASE 3: Generate initial segments.elf
# =============================================================================
print("\n=== Generating segments.elf ===")

def generate_segments_elf(built_segments_sizes=None):
    asm_path = f'{BUILD_DIR}/segments.S'
    written_symbols = set()
    
    with open(asm_path, 'w') as f:
        f.write("/* Auto-generated segment boundaries */\n\n")
        for (name, has_rom), data in sorted(all_segments_map.items(), key=lambda x: x[1].get('Start', 0)):
            if 'Start' in data and 'End' in data:
                start = data['Start']
                end = data['End']
                
                if not has_rom and start >= 0x80000000:
                    continue
                
                # USE DC SIZES when available (this is the key fix!)
                if has_rom and built_segments_sizes and name in built_segments_sizes:
                    end = start + built_segments_sizes[name]
                
                prefix = "Rom" if has_rom else ""
                # SH4 uses underscore prefix, so C symbol _foo becomes __foo in asm
                start_sym = f"__{name}Segment{prefix}Start"
                end_sym = f"__{name}Segment{prefix}End"
                
                if start_sym not in written_symbols:
                    f.write(f".global {start_sym}\n.set {start_sym}, 0x{start:08X}\n")
                    written_symbols.add(start_sym)
                if end_sym not in written_symbols:
                    f.write(f".global {end_sym}\n.set {end_sym}, 0x{end:08X}\n")
                    written_symbols.add(end_sym)
        
        for sym, addr in sorted(linker_symbols.items()):
            dsym = f"_{sym}"
            if dsym not in written_symbols:
                f.write(f".global {dsym}\n.set {dsym}, 0x{addr:08X}\n")
                written_symbols.add(dsym)

        # Audio sub-segment symbols (Sequence_N_Start/Size, Soundfont_N_Start/Size, SampleBank_N_Start/Size)
        if audio_symbols:
            f.write("\n/* Audio sub-segment symbols */\n")
            for sym, addr in sorted(audio_symbols.items()):
                asm_sym = f"_{sym}"
                if asm_sym not in written_symbols:
                    f.write(f".global {asm_sym}\n.set {asm_sym}, 0x{addr:08X}\n")
                    written_symbols.add(asm_sym)

    segments_elf = f'{BUILD_DIR}/segments.elf'
    cmd = f"{AS} {ENDIAN} {asm_path} -o {BUILD_DIR}/segments.o"
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"Assembly failed: {r.stderr}")
        return False
    cmd = f"{LD} {ENDIAN} -e 0 {BUILD_DIR}/segments.o -o {segments_elf}"
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"Link failed: {r.stderr}")
    return r.returncode == 0

segments_elf = f'{BUILD_DIR}/segments.elf'

if generate_segments_elf():
    print(f"Generated initial segments.elf with {len(all_segments_map)} segments")
else:
    print(f"Failed to generate segments.elf!")

# =============================================================================
# PHASE 4: Generate stubs.ld
# =============================================================================
print("\n=== Generating stubs.ld ===")
all_undefined = set()
for needs in segment_needs.values():
    all_undefined.update(needs)

external_symbols = all_undefined - all_defined

segment_sym_count = 0
stub_ld_path = f"{BUILD_DIR}/stubs.ld"
with open(stub_ld_path, 'w') as f:
    f.write("/* Auto-generated stub symbols */\n\n")
    for sym in sorted(external_symbols):
        if sym and re.match(r'__?\w+Segment(Rom)?(Start|End)$', sym):
            segment_sym_count += 1
            continue
        if sym:
            f.write(f"{sym} = 0;\n")

print(f"Generated {stub_ld_path} with {len(external_symbols)} stubs")
print(f"  (filtered out {len(all_undefined) - len(external_symbols)} symbols defined in assets)")

# =============================================================================
# PHASE 5: Link to ELF (pass 1)
# =============================================================================
print("\n=== Linking segments to ELF (pass 1) ===")
success = 0
link_failed = []
built_segments = {}
first_link_error_shown = False
first_objcopy_error_shown = False
first_empty_bin_shown = False

seg_deps = {seg: get_dependencies(seg) for seg in compiled}
sorted_segs = sorted(compiled.keys(), key=lambda s: len(seg_deps[s]))

total = len(sorted_segs)
for i, seg_name in enumerate(sorted_segs, 1):
    o_paths = compiled[seg_name]
    deps = seg_deps[seg_name]
    print(f"\r[{i}/{total}] {seg_name} (deps: {len(deps)})...                    ", end="", flush=True)
    
    base = segment_bases.get(seg_name, "0")
    elf_path = f"{BUILD_DIR}/{seg_name}.elf"
    bin_path = f"{DATA_DIR}/{seg_name}.bin"
    seg_ld_path = f"{BUILD_DIR}/{seg_name}.ld"
    
    o_files = ' '.join(o_paths)
    
    with open(seg_ld_path, 'w') as f:
        f.write(f"""
SECTIONS {{
    . = {base};
    .data : {{ *(.data.rel.local) *(.data.rel) *(.data) *(.data.*) }}
    .rodata : {{ *(.rodata .rodata.*) }}
    .bss : {{ *(.bss .bss.*) }}
    /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame) }}
}}
""")
    
    # Build dependency references
    dep_refs = f"-R {segments_elf}"
    for dep in deps:
        dep_elf = f"{BUILD_DIR}/{dep}.elf"
        if os.path.exists(dep_elf):
            dep_refs += f" -R {dep_elf}"
    
    link_cmd = f"{LD} {ENDIAN} -e 0 -T {seg_ld_path} -T {stub_ld_path} {dep_refs} {o_files} -o {elf_path} --no-check-sections"
    r = subprocess.run(link_cmd, shell=True, capture_output=True, text=True)
    
    if r.returncode != 0:
        if not first_link_error_shown:
            print(f"\n\n=== FIRST LINK ERROR ===")
            print(f"Segment: {seg_name}")
            print(f"Command: {link_cmd}")
            print(f"Exit code: {r.returncode}")
            print(f"STDERR:\n{r.stderr}")
            print(f"STDOUT:\n{r.stdout}")
            print("=" * 60 + "\n")
            first_link_error_shown = True
        link_failed.append((seg_name, "elf link", r.stderr[:500]))
        continue
    
    subprocess.run(
        f"{OBJCOPY} --strip-symbol=gIdentityMtx --strip-symbol=_gIdentityMtx "
        f"--strip-symbol=gIdentityMtxF --strip-symbol=_gIdentityMtxF {elf_path}",
        shell=True, capture_output=True, text=True
    )
    
    r = subprocess.run(f"{NM} {elf_path} | grep -E 'Segment(Rom)?(Start|End)' | awk '{{print $3}}'",
                       shell=True, capture_output=True, text=True)
    if r.stdout.strip():
        syms = [s for s in r.stdout.strip().split('\n') if s]
        if syms:
            strip_file = f"{BUILD_DIR}/{seg_name}_strip.txt"
            with open(strip_file, 'w') as sf:
                sf.write('\n'.join(syms))
            subprocess.run(f"{OBJCOPY} --strip-symbols={strip_file} {elf_path}",
                          shell=True, capture_output=True, text=True)
            os.remove(strip_file)
    
    objcopy_cmd = f"{OBJCOPY} -O binary --only-section=.data --only-section=.rodata --only-section=.bss {elf_path} {bin_path}"
    r = subprocess.run(objcopy_cmd, shell=True, capture_output=True, text=True)
    
    if r.returncode != 0:
        if not first_objcopy_error_shown:
            print(f"\n\n=== FIRST OBJCOPY ERROR ===")
            print(f"Segment: {seg_name}")
            print(f"Command: {objcopy_cmd}")
            print(f"Exit code: {r.returncode}")
            print(f"STDERR:\n{r.stderr}")
            print("=" * 60 + "\n")
            first_objcopy_error_shown = True
        link_failed.append((seg_name, "objcopy", r.stderr[:500]))
        continue
    
    size = os.path.getsize(bin_path) if os.path.exists(bin_path) else 0
    if size > 0:
        success += 1
        built_segments[seg_name] = size
    else:
        if not first_empty_bin_shown:
            print(f"\n\n=== FIRST EMPTY BIN ===")
            print(f"Segment: {seg_name}")
            print(f"ELF exists: {os.path.exists(elf_path)}")
            if os.path.exists(elf_path):
                print(f"ELF size: {os.path.getsize(elf_path)}")
                r2 = subprocess.run(f"sh-elf-readelf -S {elf_path} | head -30", shell=True, capture_output=True, text=True)
                print(f"ELF sections:\n{r2.stdout}")
            print(f"Bin exists: {os.path.exists(bin_path)}")
            print(f"Bin size: {size}")
            print("=" * 60 + "\n")
            first_empty_bin_shown = True

print(f"\nBuilt {success}/{len(segments)} segments")

# =============================================================================
# PHASE 6: Regenerate segments.elf with DC binary sizes
# =============================================================================
print("\n=== Regenerating segments.elf with DC sizes ===")

# Recompute the canonical option-A VROM layout now that we know DC sizes.
# segments.elf (below) and vrom_table.h (PHASE 8) both read these globals.
all_segments_map, rom_segments = optionA_maps(built_segments)

if generate_segments_elf(built_segments):
    print(f"Regenerated segments.elf with {len(built_segments)} DC-sized segments")
else:
    print(f"Failed to regenerate segments.elf!")

# Re-link ALL assets with DC-sized segments.elf
print("\n=== Re-linking all assets with DC sizes ===")

relinked = 0
total = len(sorted_segs)
for i, seg_name in enumerate(sorted_segs, 1):
    print(f"\r[{i}/{total}] {seg_name}...                    ", end="", flush=True)
    o_paths = compiled[seg_name]
    deps = seg_deps[seg_name]
    base = segment_bases.get(seg_name, "0")
    elf_path = f"{BUILD_DIR}/{seg_name}.elf"
    bin_path = f"{DATA_DIR}/{seg_name}.bin"
    seg_ld_path = f"{BUILD_DIR}/{seg_name}.ld"
    
    o_files = ' '.join(o_paths)
    
    with open(seg_ld_path, 'w') as f:
        f.write(f"""
SECTIONS {{
    . = {base};
    .data : {{ *(.data.rel.local) *(.data.rel) *(.data) *(.data.*) }}
    .rodata : {{ *(.rodata .rodata.*) }}
    .bss : {{ *(.bss .bss.*) }}
    /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame) }}
}}
""")
    
    dep_refs = f"-R {segments_elf}"
    for dep in deps:
        dep_elf = f"{BUILD_DIR}/{dep}.elf"
        if os.path.exists(dep_elf):
            dep_refs += f" -R {dep_elf}"
    
    r = subprocess.run(
        f"{LD} {ENDIAN} -e 0 -T {seg_ld_path} -T {stub_ld_path} {dep_refs} {o_files} -o {elf_path} --no-check-sections",
        shell=True, capture_output=True, text=True
    )
    
    if r.returncode == 0:
        subprocess.run(
            f"{OBJCOPY} --strip-symbol=gIdentityMtx --strip-symbol=_gIdentityMtx "
            f"--strip-symbol=gIdentityMtxF --strip-symbol=_gIdentityMtxF {elf_path}",
            shell=True, capture_output=True, text=True
        )
        
        r = subprocess.run(f"{NM} {elf_path} | grep -E 'Segment(Rom)?(Start|End)' | awk '{{print $3}}'",
                           shell=True, capture_output=True, text=True)
        if r.stdout.strip():
            syms = [s for s in r.stdout.strip().split('\n') if s]
            if syms:
                strip_file = f"{BUILD_DIR}/{seg_name}_strip.txt"
                with open(strip_file, 'w') as sf:
                    sf.write('\n'.join(syms))
                subprocess.run(f"{OBJCOPY} --strip-symbols={strip_file} {elf_path}",
                              shell=True, capture_output=True, text=True)
                os.remove(strip_file)
        
        subprocess.run(f"{OBJCOPY} -O binary --only-section=.data --only-section=.rodata --only-section=.bss {elf_path} {bin_path}",
                      shell=True, capture_output=True)
        
        size = os.path.getsize(bin_path) if os.path.exists(bin_path) else 0
        if size > 0:
            built_segments[seg_name] = size
            relinked += 1

print(f"\nRe-linked {relinked} segments")

# =============================================================================
# PHASE 7: Generate size comparison map
# =============================================================================
print("\n=== Generating size_map.txt ===")

with open(f'{BUILD_DIR}/size_map.txt', 'w') as f:
    f.write("# N64 vs DC segment sizes\n")
    f.write(f"{'Segment':<40} {'N64':>10} {'DC':>10} {'Diff':>10}\n")
    f.write("-" * 72 + "\n")
    for name, addrs in sorted(rom_segments.items(), key=lambda x: x[1].get('Start', 0)):
        if 'Start' in addrs and 'End' in addrs:
            n64_size = addrs['End'] - addrs['Start']
            dc_size = built_segments.get(name, 0)
            diff = dc_size - n64_size if dc_size else 0
            flag = " ***" if diff != 0 and dc_size else ""
            f.write(f"{name:<40} {n64_size:>10} {dc_size:>10} {diff:>+10}{flag}\n")

print(f"Generated {BUILD_DIR}/size_map.txt")

# =============================================================================
# PHASE 8: Generate vrom_table.h WITH DC SIZES (key fix!)
# =============================================================================
print("\n=== Generating vrom_table.h ===")

# Build entries with DC sizes where available
entries = []
for name, addrs in sorted(rom_segments.items(), key=lambda x: x[1].get('Start', 0)):
    if 'Start' in addrs and 'End' in addrs:
        start = addrs['Start']
        # USE DC SIZE otherwise use N64 size (FIX)
        if name in built_segments:
            end = start + built_segments[name]
        else:
            end = addrs['End']
        entries.append((start, end, name))

# Check for overlaps
overlaps = []
for i in range(len(entries) - 1):
    curr_start, curr_end, curr_name = entries[i]
    next_start, next_end, next_name = entries[i + 1]
    if curr_end > next_start:
        overlap_size = curr_end - next_start
        overlaps.append((curr_name, next_name, overlap_size, curr_end, next_start))

if overlaps:
    print(f"  WARNING: {len(overlaps)} segment overlaps detected:")
    for curr_name, next_name, size, curr_end, next_start in overlaps[:10]:
        print(f"    {curr_name} (end 0x{curr_end:08X}) overlaps {next_name} (start 0x{next_start:08X}) by {size} bytes")
    if len(overlaps) > 10:
        print(f"    ... and {len(overlaps) - 10} more")

with open('src/dreamcast/vrom_table.h', 'w') as f:
    f.write("// Auto-generated VROM table for Dreamcast\n")
    f.write("// Uses DC sizes for segments we built, N64 sizes for others\n")
    f.write("#ifndef VROM_TABLE_H\n#define VROM_TABLE_H\n\n")
    f.write("typedef struct {\n")
    f.write("    unsigned int vrom_start;\n")
    f.write("    unsigned int vrom_end;\n")
    f.write("    const char* name;\n")
    f.write("} VromEntry;\n\n")
    f.write("static VromEntry gVromTable[] = {\n")
    
    for start, end, name in entries:
        f.write(f'    {{ 0x{start:08X}, 0x{end:08X}, "{name}" }},\n')
    
    f.write("    { 0, 0, NULL }\n")
    f.write("};\n\n")
    f.write("#endif\n")

print(f"Generated src/dreamcast/vrom_table.h with {len(entries)} entries")

# =============================================================================
# PHASE 9: Extract audio segments from ROM
# =============================================================================
print("\n=== Extracting audio segments from ROM ===")

# MIPS-free: read the audio blobs straight from the decompressed baserom, at
# the baserom's own dmadata offsets (dc_meta.audio_blobs). Our audio symbols
# (Soundfont_N/Sequence_N/...) are also derived from that same baserom, so the
# .bin data and the sub-segment offsets are self-consistent. (The option-A
# rom_segments VROM values are the DC layout, NOT baserom offsets, so they
# must NOT be used to seek the baserom.)
BASEROM = f"baseroms/{VERSION}/baserom-decompressed.z64"
audio_segments = dict(_meta["audio_blobs"])
if os.path.exists(BASEROM):
    audio_rom = BASEROM
    print(f"  Using baserom: {BASEROM}")
else:
    audio_rom = None
    print(f"  WARNING: {BASEROM} not found, skipping audio extraction")

if audio_rom and not audio_segments:
    print(f"  WARNING: No audio segment ROM addresses found in map file")
elif audio_rom:
    with open(audio_rom, 'rb') as rom:
        for name, (start, end) in sorted(audio_segments.items(), key=lambda x: x[1][0]):
            size = end - start
            bin_path = f"{DATA_DIR}/{name}.bin"
            rom.seek(start)
            data = rom.read(size)
            with open(bin_path, 'wb') as out:
                out.write(data)
            print(f"  {name}: 0x{start:08X}-0x{end:08X} ({size} bytes) -> {bin_path}")
    print(f"Audio segments extracted (big-endian, raw from ROM)")

# =============================================================================
# Summary
# =============================================================================
if compile_failed:
    print(f"\n=== Compile failures ({len(compile_failed)}) ===")
    for name, path, err in compile_failed[:5]:
        print(f"  {name}: {path}")
        if err:
            for line in err.split('\n')[:3]:
                print(f"    {line}")

if link_failed:
    print(f"\n=== Link failures ({len(link_failed)}) ===")
    for name, stage, err in link_failed[:5]:
        print(f"  {name} ({stage})")
        if err:
            for line in err.split('\n')[:3]:
                print(f"    {line}")

print(f"\n=== Output ===")
print(f"  ELFs: {BUILD_DIR}/*.elf  (use with -R for symbol references)")
print(f"  Bins: {DATA_DIR}/*.bin   (load at runtime)")
print(f"  Map:  {BUILD_DIR}/size_map.txt")

print(f"\n=== BUILD COMPLETE ===")
print(f"Now run: make -f Makefile.dc")