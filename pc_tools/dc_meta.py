#!/usr/bin/env python3
"""
dc_meta.py -- MIPS-free, version-agnostic metadata generator for the Dreamcast
OoT asset build.

Replaces the N64-build-derived inputs that pc_tools/dc_assets.py currently reads
from build/<version>/{oot-*.ld,oot-*.map,oot-*.elf,spec,oot-*.z64}. Everything
here is produced entirely on the host (no MIPS toolchain), from:

  * the in-repo `spec/` (preprocessed with `gcc -E`),
  * the host tools mkldscript / mkdmadata (portable C, clang-built on demand),
  * tools/aica/audio_tables.json (soundfont + samplebank offsets),
  * the sequence table read directly out of the decompressed baserom,
  * dmadata (baserom's own file table) for the raw audio blob locations.

Layout policy: OPTION A. The Dreamcast ROM (VROM) layout is recomputed
cumulatively from the *DC-compiled* segment sizes (compute_vrom_layout), NOT
inherited from the N64 build. Because gDmaDataTable (via dmadata_table_spec.h,
which uses DEFINE_DMA_ENTRY symbol references) and gVromTable both derive from
this single computed layout, the game's DMA VROM requests and reimpl.c's
find_segment() stay self-consistent by construction.

dc_assets.py is expected to import this module, take the static metadata up
front, and call compute_vrom_layout() once it knows the real DC segment sizes.

Standalone use:
  dc_meta.py --version gc-eu-mq-dbg            # print metadata summary
  dc_meta.py --version gc-eu-mq-dbg --verify   # diff every field against the
                                               # carried-over MIPS build outputs
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Host (Dreamcast-agnostic) toolchain used only to build the layout tools and
# preprocess the spec. `gcc` is clang on macOS; both are fine.
HOST_CC = os.environ.get("HOST_CC", "cc")
CPP = os.environ.get("CPP", "gcc -E")


# ---------------------------------------------------------------------------
# Version configuration -- parsed from the decomp Makefile so any supported
# ROM works without a hand-maintained table here.
# ---------------------------------------------------------------------------
def load_version_config(version):
    """Mirror the Makefile's per-version + per-platform settings for `version`."""
    mk = open(os.path.join(REPO, "Makefile")).read()

    # The version block: `else ifeq ($(VERSION),<v>)` up to the next `else`/`endif`.
    m = re.search(
        r'ifeq \(\$\(VERSION\),%s\)(.*?)(?:else ifeq \(\$\(VERSION\)|endif)'
        % re.escape(version),
        mk, re.S)
    if not m:
        raise SystemExit(f"dc_meta: version '{version}' not found in Makefile")
    block = m.group(1)

    def field(name, default=None):
        mm = re.search(r'%s\s*[:?]?=\s*(\S+)' % name, block)
        return mm.group(1) if mm else default

    region = field("REGION")
    platform = field("PLATFORM")
    revision = field("REVISION", "0")
    debug = field("DEBUG_FEATURES", "1" if version.endswith("dbg") else "0")

    # LIBULTRA_VERSION / LIBULTRA_PATCH live in the per-PLATFORM ifeq blocks.
    pm = re.search(
        r'ifeq \(\$\(PLATFORM\),%s\)(.*?)(?:else ifeq \(\$\(PLATFORM\)|endif)'
        % re.escape(platform), mk, re.S)
    pblock = pm.group(1) if pm else ""
    lv = re.search(r'LIBULTRA_VERSION\s*[:?]?=\s*(\S+)', pblock)
    lp = re.search(r'LIBULTRA_PATCH\s*[:?]?=\s*(\S+)', pblock)

    version_macro = version.upper().replace("-", "_").replace(".", "_")
    return {
        "version": version,
        "version_macro": version_macro,
        "region": region,
        "platform": platform,
        "revision": revision,
        "debug_features": debug,
        "libultra_version": lv.group(1) if lv else "L",
        "libultra_patch": lp.group(1) if lp else "0",
    }


def cpp_defines(cfg):
    """The -D flags used to preprocess the spec. Returned as a LIST (never a
    string -- this shell is zsh, which does not word-split unquoted vars)."""
    plat = {"N64": (1, 0, 0), "GC": (0, 1, 0), "IQUE": (0, 0, 1)}[cfg["platform"]]
    d = [
        "-DCOMPILER_GCC", "-DNON_MATCHING", "-DAVOID_UB",
        f"-DPLATFORM_N64={plat[0]}", f"-DPLATFORM_GC={plat[1]}",
        f"-DPLATFORM_IQUE={plat[2]}",
        f"-DOOT_VERSION={cfg['version_macro']}",
        f"-DOOT_REVISION={cfg['revision']}",
        f"-DOOT_REGION=REGION_{cfg['region']}",
        f"-DLIBULTRA_VERSION=LIBULTRA_VERSION_{cfg['libultra_version']}",
        f"-DLIBULTRA_PATCH={cfg['libultra_patch']}",
    ]
    if cfg["platform"] == "IQUE":
        d.append("-DBBPLAYER")
    if cfg["debug_features"] == "1":
        d.append("-DDEBUG_FEATURES=1")
    else:
        d += ["-DDEBUG_FEATURES=0", "-DNDEBUG"]
    return d


# ---------------------------------------------------------------------------
# Host tools (clang-built, cached under build_dc/tools).
# ---------------------------------------------------------------------------
def ensure_host_tools():
    """Build native mkldscript/mkdmadata if the in-tree binaries aren't runnable
    on this host (the committed ones are Linux ELF). Returns (mkldscript, mkdmadata)."""
    outdir = os.path.join(REPO, "build_dc", "tools")
    os.makedirs(outdir, exist_ok=True)
    tools = {}
    srcs = {
        "mkldscript": ["mkldscript.c", "spec.c", "util.c"],
        "mkdmadata": ["mkdmadata.c", "spec.c", "util.c"],
    }
    for name, files in srcs.items():
        out = os.path.join(outdir, name)
        need = not os.path.exists(out)
        if not need:
            # is it runnable here? (committed binary may be foreign-arch)
            r = subprocess.run([out], capture_output=True)
            need = (r.returncode not in (0, 1))  # exec-format-error => 126/2/...
        if need:
            cmd = [HOST_CC.split()[0], "-Wall", "-std=gnu99", "-O2",
                   "-I" + os.path.join(REPO, "tools")]
            cmd += [os.path.join(REPO, "tools", f) for f in files]
            cmd += ["-o", out]
            subprocess.run(cmd, check=True)
        tools[name] = out
    return tools["mkldscript"], tools["mkdmadata"]


def preprocess_spec(cfg):
    """gcc -E the master spec exactly like the Makefile's build/<v>/spec rule."""
    outdir = os.path.join(REPO, "build_dc", "meta")
    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, "spec")
    cmd = CPP.split() + ["-P", "-xc", "-fno-dollars-in-identifiers"] + cpp_defines(cfg)
    cmd += ["-I.", os.path.join("spec", "spec")]
    with open(out, "w") as f:
        r = subprocess.run(cmd, cwd=REPO, stdout=f, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        raise SystemExit("dc_meta: spec preprocessing failed:\n" + r.stderr)
    # The Makefile applies BUILD_DIR_REPLACE; mirror it so include paths resolve.
    txt = open(out).read().replace("$(BUILD_DIR)", f"build/{cfg['version']}")
    open(out, "w").write(txt)
    return out


# ---------------------------------------------------------------------------
# Segment metadata from the spec + mkldscript.
# ---------------------------------------------------------------------------
def parse_spec_fields(spec_path):
    """Per-segment fields we need: romalign (for the VROM layout) and a flag for
    whether the segment carries ROM data (has includes)."""
    segs = {}
    name = None
    order = []
    for line in open(spec_path):
        s = line.strip()
        if s.startswith("beginseg"):
            name = None
        elif s.startswith("name "):
            name = s.split('"')[1]
            segs[name] = {"romalign": None, "has_data": False}
            order.append(name)
        elif s.startswith("romalign") and name:
            segs[name]["romalign"] = int(s.split()[1], 0)
        elif s.startswith("include") and name:
            segs[name]["has_data"] = True
        elif s.startswith("endseg"):
            name = None
    return segs, order


def segment_bases(cfg, mkldscript, spec_path):
    """VRAM base per segment, via mkldscript (proven byte-identical to the .ld)."""
    ld = os.path.join(REPO, "build_dc", "meta", "oot.ld")
    subprocess.run([mkldscript, spec_path, ld], check=True)
    bases = {}
    for m in re.finditer(r'\.\.(\w+)\s+(0x[0-9A-Fa-f]+)\s*:', open(ld).read()):
        bases[m.group(1)] = int(m.group(2), 16)
    return bases


def dma_order(cfg, mkdmadata, spec_path):
    """Ordered list of DMA'd segment names (mkdmadata's dmadata_table_spec.h).
    This is the authoritative order the ROM/VROM layout follows."""
    hdr = os.path.join(REPO, "build_dc", "meta", "dmadata_table_spec.h")
    cr = os.path.join(REPO, "build_dc", "meta", "compress_ranges.txt")
    subprocess.run([mkdmadata, spec_path, hdr, cr], check=True)
    names = []
    for m in re.finditer(r'DEFINE_DMA_ENTRY\((\w+),', open(hdr).read()):
        names.append(m.group(1))
    return names


# ---------------------------------------------------------------------------
# Audio symbols (Sequence_N / Soundfont_N / SampleBank_N Start/Size).
# ---------------------------------------------------------------------------
def _read_baserom(cfg):
    p = os.path.join(REPO, "baseroms", cfg["version"], "baserom-decompressed.z64")
    if not os.path.exists(p):
        raise SystemExit(f"dc_meta: {p} not found")
    return open(p, "rb").read()


def _locate_sequence_table(rom, exclude_sizes=frozenset()):
    """Find the sequence AudioTableHeader in the (uncompressed) baserom by its
    shape: 16-byte entries, entry[0].romAddr == 0, a long run of contiguous
    (romAddr[i+1] == romAddr[i] + size[i]) entries, and a preceding u16
    numEntries that matches. The soundfont and samplebank tables have the same
    shape, so `exclude_sizes` (their entry[0].size, known from the JSON) rules
    them out. Version-agnostic; no symbols required."""
    RUN = 8
    for base in range(0, len(rom) - 16 * (RUN + 2), 4):
        ra0, sz0 = struct.unpack_from(">II", rom, base)
        if ra0 != 0 or not (0 < sz0 < 0x400000) or sz0 in exclude_sizes:
            continue
        off = ra0
        ok = True
        for i in range(RUN):
            ra, sz = struct.unpack_from(">II", rom, base + i * 16)
            if ra != off or not (0 < sz < 0x400000):
                ok = False
                break
            off += sz
        if not ok:
            continue
        num = struct.unpack_from(">H", rom, base - 16)[0]
        if 16 <= num <= 4096:
            return base, num
    raise SystemExit("dc_meta: could not locate sequence table in baserom")


def audio_symbols(cfg):
    """Return {sym_name: value} for every Sequence_/Soundfont_/SampleBank_ Start/Size."""
    syms = {}
    tables = json.load(open(os.path.join(REPO, "tools", "aica", "audio_tables.json")))
    for e in tables["soundfonts"]:
        syms[f"Soundfont_{e['index']}_Start"] = e["rom_addr"]
        syms[f"Soundfont_{e['index']}_Size"] = e["size"]
    for e in tables["sample_banks"]:
        if e.get("is_ptr"):
            continue
        syms[f"SampleBank_{e['index']}_Start"] = e["rom_addr"]
        syms[f"SampleBank_{e['index']}_Size"] = e["size"]
    # Sequences: straight out of the baserom's sequence table. Exclude the
    # soundfont/samplebank tables, which share the same header shape.
    exclude = {tables["soundfonts"][0]["size"]}
    exclude |= {b["size"] for b in tables["sample_banks"] if not b.get("is_ptr")}
    rom = _read_baserom(cfg)
    base, num = _locate_sequence_table(rom, exclude)
    for n in range(num):
        ra, sz = struct.unpack_from(">II", rom, base + n * 16)
        # header sentinel / empty tail entries have size 0
        if ra == 0 and sz == 0 and n != 0:
            continue
        syms[f"Sequence_{n}_Start"] = ra
        syms[f"Sequence_{n}_Size"] = sz
    return syms


# ---------------------------------------------------------------------------
# Raw audio blob locations in the baserom (for byte extraction) + D_ constants.
# ---------------------------------------------------------------------------
def _dmadata_entries(cfg):
    dmadata = open(os.path.join(REPO, "extracted", cfg["version"],
                                "baserom", "dmadata"), "rb").read()
    entries = []
    for off in range(0, len(dmadata), 16):
        vs, ve, rs, re_ = struct.unpack_from(">IIII", dmadata, off)
        if (vs, ve, rs, re_) == (0, 0, 0, 0):
            break
        entries.append((vs, ve))
    return entries


def audio_blobs(cfg, dma_names):
    """{Audiobank/Audioseq/Audiotable: (vrom_start, vrom_end)} from dmadata,
    name-anchored via the dma order (1:1 with dmadata entries)."""
    blobs = {}
    for name, (vs, ve) in zip(dma_names, _dmadata_entries(cfg)):
        if name in ("Audiobank", "Audioseq", "Audiotable"):
            blobs[name] = (vs, ve)
    return blobs


def baserom_sizes(cfg, dma_names):
    """{name: uncompressed byte size} for every DMA segment, from the baserom's
    own dmadata. Used to size segments dc_assets.py does not compile (makerom,
    boot, code, ...) so their option-A VROM slots don't collapse."""
    return {name: ve - vs
            for name, (vs, ve) in zip(dma_names, _dmadata_entries(cfg))}


def linker_symbols():
    """The handful of D_XXXXXXXX segment-slot constants the runtime references.
    Each equals the hex embedded in its name (D_06000000 == 0x06000000)."""
    # Only the segment-slot constants (D_NN000000, value == name); other D_
    # symbols the sources reference (e.g. RAM addresses) are real symbols
    # resolved elsewhere, not linker-defined here. Scan the whole src/ tree,
    # since these are referenced from actor overlays too (e.g. ovl_Bg_Mjin).
    r = subprocess.run(
        ["grep", "-rhoE", r"\bD_[0-9A-Fa-f]{2}000000\b", os.path.join(REPO, "src")],
        capture_output=True, text=True)
    syms = {}
    for name in set(r.stdout.split()):
        syms[name] = int(name[2:], 16)
    return syms


# ---------------------------------------------------------------------------
# OPTION A: recompute the DC VROM layout cumulatively from DC segment sizes.
# ---------------------------------------------------------------------------
def compute_vrom_layout(order, sizes, romaligns):
    """Replicates mkldscript's _RomSize accumulation (align start & end by
    `romalign` where present). `sizes` maps segment name -> DC ROM-resident byte
    size. Returns {name: (vrom_start, vrom_end)} in `order`."""
    layout = {}
    romsize = 0
    for name in order:
        ra = romaligns.get(name)
        if ra:
            romsize = (romsize + ra - 1) & ~(ra - 1)
        start = romsize
        romsize += sizes.get(name, 0)
        end = romsize
        if ra:
            romsize = (romsize + ra - 1) & ~(ra - 1)
        layout[name] = (start, end)
    return layout


# ---------------------------------------------------------------------------
# Top-level: gather everything static (no DC sizes needed yet).
# ---------------------------------------------------------------------------
def gather(version):
    cfg = load_version_config(version)
    mkldscript, mkdmadata = ensure_host_tools()
    spec_path = preprocess_spec(cfg)
    spec_fields, spec_order = parse_spec_fields(spec_path)
    bases = segment_bases(cfg, mkldscript, spec_path)
    dnames = dma_order(cfg, mkdmadata, spec_path)
    return {
        "cfg": cfg,
        "spec_path": spec_path,
        "segment_bases": bases,
        "dma_order": dnames,
        "romaligns": {n: spec_fields.get(n, {}).get("romalign") for n in dnames},
        "audio_symbols": audio_symbols(cfg),
        "audio_blobs": audio_blobs(cfg, dnames),
        "baserom_sizes": baserom_sizes(cfg, dnames),
        "linker_symbols": linker_symbols(),
    }


# ---------------------------------------------------------------------------
# Verification against the carried-over MIPS build outputs.
# ---------------------------------------------------------------------------
def _gt_paths(version):
    b = os.path.join(REPO, "build", version, f"oot-{version}")
    return b + ".ld", b + ".map", b + ".elf"


def verify(version):
    meta = gather(version)
    ld, mapf, elf = _gt_paths(version)
    fails = 0

    # 1) segment bases vs .ld
    if os.path.exists(ld):
        gt = {m.group(1): int(m.group(2), 16)
              for m in re.finditer(r'\.\.(\w+)\s+(0x[0-9A-Fa-f]+)\s*:', open(ld).read())}
        bad = {k: (meta["segment_bases"].get(k), v) for k, v in gt.items()
               if meta["segment_bases"].get(k) != v}
        print(f"[bases]  {len(gt)-len(bad)}/{len(gt)} match" + (" OK" if not bad else ""))
        for k, (got, want) in list(bad.items())[:8]:
            print(f"           {k}: got {got} want {want:#x}"); fails += 1
    else:
        print("[bases]  (no ground-truth .ld present, skipped)")

    # 2) audio symbols vs nm(.elf)
    if os.path.exists(elf):
        r = subprocess.run(["nm", elf], capture_output=True, text=True)
        gt = {}
        for line in r.stdout.splitlines():
            p = line.split()
            if len(p) >= 3 and re.match(r'(Sequence|Soundfont|SampleBank)_\d+_(Start|Size)$', p[2]):
                gt[p[2]] = int(p[0], 16)
        bad = {k: (meta["audio_symbols"].get(k), v) for k, v in gt.items()
               if meta["audio_symbols"].get(k) != v}
        print(f"[audio]  {len(gt)-len(bad)}/{len(gt)} match" + (" OK" if not bad else ""))
        for k, (got, want) in list(bad.items())[:8]:
            print(f"           {k}: got {got} want {want:#x}"); fails += 1
    else:
        print("[audio]  (no ground-truth .elf present, skipped)")

    # 3) linker D_ symbols + audio blobs + option-A algorithm vs .map
    if os.path.exists(mapf):
        mp = open(mapf).read()
        # D_ constants
        gtD = {m.group(2): int(m.group(1), 16)
               for m in re.finditer(r'(0x[0-9a-fA-F]+)\s+(D_[0-9A-Fa-f]+)\s*=', mp)}
        badD = {k: v for k, v in meta["linker_symbols"].items() if gtD.get(k) != v}
        print(f"[D_sym]  {len(meta['linker_symbols'])-len(badD)}/{len(meta['linker_symbols'])} match"
              + (" OK" if not badD else ""))
        for k in list(badD)[:8]:
            print(f"           {k}: got {meta['linker_symbols'][k]:#x} want {gtD.get(k)}"); fails += 1

        # rom_segments from .map (Start/End) -> feed N64 sizes into option-A layout
        rs = {}
        for line in mp.splitlines():
            m = re.search(r'(0x[0-9a-fA-F]+)\s+_(\w+)SegmentRom(Start|End)\b', line)
            if m:
                rs.setdefault(m.group(2), {})[m.group(3)] = int(m.group(1), 16)
        n64_sizes = {n: v["End"] - v["Start"] for n, v in rs.items()
                     if "Start" in v and "End" in v}
        layout = compute_vrom_layout(meta["dma_order"], n64_sizes, meta["romaligns"])
        badL = {n: (layout[n][0], rs[n]["Start"]) for n in meta["dma_order"]
                if n in rs and "Start" in rs[n] and layout[n][0] != rs[n]["Start"]}
        tot = sum(1 for n in meta["dma_order"] if n in rs and "Start" in rs[n])
        print(f"[layoutA] {tot-len(badL)}/{tot} VROM starts reproduced from N64 sizes"
              + (" OK" if not badL else ""))
        for n, (got, want) in list(badL.items())[:8]:
            print(f"           {n}: got {got:#x} want {want:#x}"); fails += 1

        # audio blobs come from the baserom's OWN dmadata (authoritative for
        # byte extraction; may differ from the rebuilt .map by original-vs-rebuilt
        # padding). Validate they are the 3 expected segments, sized, and packed
        # contiguously Audiobank -> Audioseq -> Audiotable.
        b = meta["audio_blobs"]
        want3 = {"Audiobank", "Audioseq", "Audiotable"}
        ok = (set(b) == want3
              and all(e > s for s, e in b.values())
              and b["Audiobank"][1] == b["Audioseq"][0]
              and b["Audioseq"][1] == b["Audiotable"][0])
        print(f"[blob]   " + ("Audiobank->Audioseq->Audiotable contiguous & sized OK"
                              if ok else f"BAD: {b}"))
        if not ok:
            fails += 1
    else:
        print("[map]    (no ground-truth .map present, skipped)")

    print("\n" + ("ALL METADATA REPRODUCED (MIPS-free) ✅" if fails == 0
                  else f"{fails} mismatch(es) ✗"))
    return fails


def main():
    ap = argparse.ArgumentParser(description="MIPS-free DC asset metadata generator")
    ap.add_argument("--version", default="gc-eu-mq-dbg")
    ap.add_argument("--verify", action="store_true",
                    help="diff every field against the carried-over MIPS build outputs")
    args = ap.parse_args()
    if args.verify:
        sys.exit(1 if verify(args.version) else 0)
    meta = gather(args.version)
    print(f"version         : {meta['cfg']['version']} ({meta['cfg']['version_macro']})")
    print(f"cpp defines     : {' '.join(cpp_defines(meta['cfg']))}")
    print(f"segment bases   : {len(meta['segment_bases'])}")
    print(f"dma segments    : {len(meta['dma_order'])}")
    print(f"audio symbols   : {len(meta['audio_symbols'])}")
    print(f"audio blobs     : {meta['audio_blobs']}")
    print(f"linker D_ syms  : {meta['linker_symbols']}")


if __name__ == "__main__":
    main()
