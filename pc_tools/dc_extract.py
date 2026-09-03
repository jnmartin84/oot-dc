#!/usr/bin/env python3
"""
dc_extract.py -- Host-only (MIPS-free) extraction driver for the Dreamcast build.

Produces, from just `baseroms/<version>/baserom.z64`, everything the DC build reads
that the N64 decomp normally produces via `make setup` + the MIPS `make`:

  extracted/<version>/            (identical to `make setup`; the main Makefile's
                                   mips-linux-gnu-ld guard blocks running it here)
  build/<version>/
    dmadata_table_spec.h          mkdmadata on the cpp'd spec (dc_meta)
    compress_ranges.txt, spec
    assets/**/*.inc.c             build_from_png / bin2c / n64texconv JFIF
    assets/text/*.enc.{nes,jpn}.h cpp | msgenc
    assets/audio/samples/**.aifc  sampleconv (needed by sfc)
    assets/audio/{samplebanks,soundfonts}/*.xml   $(BUILD_DIR)-substituted copies
    assets/audio/soundfonts/*.{c,h,name}          sfc  (DC needs the .h)
    assets/audio/{samplebank,soundfont}_table.h   atblgen
    assets/audio/{soundfont,sequence}_sizes.h     from the ROM's audio tables
                                   (the N64 build sizes these from MIPS .o files;
                                   proven identical for gc-eu-mq-dbg)

NOT produced (MIPS-only, and the DC build does not read them): *.o, .map/.elf/.z64,
samplebank .s, sequence .s/.o, sequence_font_table (hardcoded in src/linux/reimpl.c
for gc-eu-mq-dbg -- derive from the .seq `#include "Soundfont_N.h"` lines when
multi-version lands).

Run with the project venv interpreter (needs crunch64/ipl3checksum/pyyaml/pygfxd):
    .venv/bin/python3 pc_tools/dc_extract.py --version gc-eu-mq-dbg
Host tools are (re)built with clang when the in-tree binaries can't run here
(the committed ones are Linux x86-64). Needs GNU make >= 3.82 (`brew install make`
-> gmake) because the tool Makefiles use `define NAME =`.
"""
import argparse
import ctypes
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dc_meta  # noqa: E402

REPO = dc_meta.REPO
HOST_CC = dc_meta.HOST_CC


def log(msg):
    print(f"[dc_extract] {msg}", flush=True)


def run(cmd, **kw):
    """Run a command (list) from the repo root; raise on failure."""
    kw.setdefault("cwd", REPO)
    kw.setdefault("check", True)
    return subprocess.run(cmd, **kw)


# ---------------------------------------------------------------------------
# Environment checks
# ---------------------------------------------------------------------------
def check_python():
    missing = []
    for mod in ("crunch64", "ipl3checksum", "yaml", "pygfxd"):
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)
    if missing:
        raise SystemExit(
            f"dc_extract: python modules missing: {missing}. Run with the project venv:\n"
            f"  python3 -m venv .venv && .venv/bin/pip install -U pip -r requirements.txt\n"
            f"  .venv/bin/python3 {sys.argv[0]} ...")


def find_make():
    """GNU make >= 3.82 (Apple's /usr/bin/make is 3.81 and mis-parses `define X =`)."""
    for cand in ("gmake", "make"):
        path = shutil.which(cand)
        if not path:
            continue
        out = subprocess.run([path, "--version"], capture_output=True, text=True).stdout
        m = re.search(r"GNU Make (\d+)\.(\d+)", out)
        if m and (int(m.group(1)), int(m.group(2))) >= (3, 82):
            return path
    raise SystemExit("dc_extract: need GNU make >= 3.82 (macOS: `brew install make` -> gmake)")


# ---------------------------------------------------------------------------
# Host tools
# ---------------------------------------------------------------------------
def _runnable(path):
    if not os.path.exists(path):
        return False
    try:
        subprocess.run([path], capture_output=True, timeout=10)
        return True
    except OSError:          # Exec format error (foreign arch)
        return False
    except subprocess.TimeoutExpired:
        return True


def _loadable(so):
    if not os.path.exists(so):
        return False
    try:
        ctypes.CDLL(so)
        return True
    except OSError:
        return False


def ensure_tools(make):
    T = os.path.join(REPO, "tools")
    # n64texconv (.so used by the python extractor, app used for JFIF) + build_from_png
    if not (_loadable(f"{T}/assets/n64texconv/libn64texconv.so")
            and _runnable(f"{T}/assets/n64texconv/n64texconv")
            and _runnable(f"{T}/assets/build_from_png/build_from_png")):
        log("building tools/assets (n64texconv, build_from_png) with clang")
        run([make, "-C", f"{T}/assets", "clean"], capture_output=True)
        run([make, "-C", f"{T}/assets"], capture_output=True)
    # audio: sampleconv (extraction), sbc/sfc/atblgen (build inputs)
    if not all(_runnable(f"{T}/audio/{p}") for p in
               ("sampleconv/sampleconv", "sfc", "sbc", "atblgen")):
        log("building tools/audio (sampleconv, sfc, sbc, atblgen) with clang")
        run([make, "-C", f"{T}/audio", "clean"], capture_output=True)
        run([make, "-C", f"{T}/audio"], capture_output=True)
    if not _runnable(f"{T}/bin2c"):
        log("building tools/bin2c with clang")
        run([make, "-C", T, "-B", "bin2c"], capture_output=True)
    mkldscript, mkdmadata = dc_meta.ensure_host_tools()
    tools = {
        "n64texconv": f"{T}/assets/n64texconv/n64texconv",
        "build_from_png": f"{T}/assets/build_from_png/build_from_png",
        "bin2c": f"{T}/bin2c",
        "sampleconv": f"{T}/audio/sampleconv/sampleconv",
        "sfc": f"{T}/audio/sfc",
        "atblgen": f"{T}/audio/atblgen",
        "mkdmadata": mkdmadata,
    }
    for name, path in tools.items():
        if not (_runnable(path)):
            raise SystemExit(f"dc_extract: host tool {name} not runnable at {path}")
    return tools


# ---------------------------------------------------------------------------
# Phase 1: extraction (mirrors the main Makefile's `setup` target)
# ---------------------------------------------------------------------------
def extract(version, ext, jobs):
    py = sys.executable
    rom = os.path.join("baseroms", version, "baserom-decompressed.z64")
    steps = [
        ("decompress_baserom", [py, "tools/decompress_baserom.py", version]),
        ("extract_baserom", [py, "tools/extract_baserom.py", rom, f"{ext}/baserom", "-v", version]),
        ("extract assets", [py, "-m", "tools.assets.extract", f"{ext}/baserom", ext,
                            "-v", version, f"-j{jobs}"]),
        ("extract_incbins", [py, "tools/extract_incbins.py", f"{ext}/baserom",
                             f"{ext}/incbin", "-v", version]),
        ("extract_text", [py, "tools/extract_text.py", f"{ext}/baserom",
                          f"{ext}/text", "-v", version]),
        ("extract_audio", [py, "tools/extract_audio.py", "-b", f"{ext}/baserom",
                           "-o", ext, "-v", version, "--read-xml"]),
    ]
    for name, cmd in steps:
        t = time.time()
        log(f"extract: {name}")
        run(cmd)
        log(f"extract: {name} done ({time.time() - t:.1f}s)")


# ---------------------------------------------------------------------------
# Phase 2: build/<version> inputs
# ---------------------------------------------------------------------------
def _parallel(jobs, tasks, what):
    """tasks: list of (label, argv). Runs them from REPO, fails on first error."""
    t = time.time()
    failed = []

    def one(task):
        label, argv = task
        r = subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
        if r.returncode != 0:
            failed.append((label, r.stdout + r.stderr))

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        list(ex.map(one, tasks))
    if failed:
        for label, out in failed[:5]:
            log(f"FAILED {what}: {label}\n{out}")
        raise SystemExit(f"dc_extract: {len(failed)} {what} jobs failed")
    log(f"{what}: {len(tasks)} jobs ({time.time() - t:.1f}s)")


def _asset_inputs(ext, subdir, exts):
    """Yield (src_path, rel_path_under_assets) for committed assets/ then extracted."""
    for base in (os.path.join(REPO, "assets"), os.path.join(ext, "assets")):
        root = os.path.join(base, subdir)
        if not os.path.isdir(root):
            continue
        for dp, _, fns in os.walk(root):
            for fn in sorted(fns):
                if fn.endswith(exts):
                    p = os.path.join(dp, fn)
                    yield p, os.path.relpath(p, base)  # path under assets/


def gen_textures(tools, ext, bld, jobs, force):
    tasks = []
    for src, rel in _asset_inputs(ext, "", (".png", ".bin", ".jpg")):
        reldir = os.path.dirname(rel)
        outdir = os.path.join(bld, "assets", reldir) + "/"
        os.makedirs(outdir, exist_ok=True)
        base = os.path.basename(rel)
        if src.endswith(".png"):
            out = outdir + base[:-4] + ".inc.c"
            # build_from_png <png> <outdir/> [<committed dir/>] <extracted dir/>
            # (same argument order as the Makefile rules for assets/ and extracted/)
            argv = [tools["build_from_png"], src, outdir]
            cdir = os.path.join(REPO, "assets", reldir) + "/"
            edir = os.path.join(ext, "assets", reldir) + "/"
            if os.path.isdir(cdir):
                argv.append(cdir)
            if os.path.isdir(edir):
                argv.append(edir)
        elif src.endswith(".bin"):
            out = outdir + base + ".inc.c"
            argv = [tools["bin2c"], "-t", "1", src, out]
        else:
            out = outdir + base + ".inc.c"
            argv = [tools["n64texconv"], "JFIF", "", src, out]
        if not force and os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src):
            continue
        tasks.append((rel, argv))
    _parallel(jobs, tasks, "textures")


def gen_text(cfg, ext, bld):
    tl = _text_lang(cfg["version"])
    charmap = "assets/text/charmap.chn.txt" if cfg["platform"] == "IQUE" \
        else "assets/text/charmap.nes.txt"
    outdir = os.path.join(bld, "assets", "text")
    os.makedirs(outdir, exist_ok=True)
    jobs = [("message_data", "nes", ["--encoding", "utf-8", "--charmap", charmap]),
            ("message_data_staff", "nes", ["--encoding", "utf-8", "--charmap", charmap])]
    if tl in ("NTSC", "CN"):
        jobs.append(("message_data", "jpn", ["--encoding", "SHIFT-JIS", "--wchar",
                                             "--charmap", "assets/text/charmap.jpn.txt"]))
    for stem, kind, encargs in jobs:
        out = os.path.join(outdir, f"{stem}.enc.{kind}.h")
        cpp = [HOST_CC, "-E", "-P", "-xc", "-fno-dollars-in-identifiers"] + \
            dc_meta.cpp_defines(cfg) + [f"-I{ext}", f"assets/text/{stem}.h"]
        pre = run(cpp, capture_output=True, text=True).stdout
        pre = pre.rstrip("\n") + "\n"   # clang -E -P vs gcc -E -P trailing-newline parity
        run([sys.executable, "tools/msgenc.py"] + encargs + ["-", out], input=pre, text=True)
        log(f"text: {out}")


def _text_lang(version):
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import version_config
    return version_config.load_version_config(version).text_lang


def gen_audio(tools, cfg, ext, bld, jobs, force):
    A = os.path.join(bld, "assets", "audio")
    bld_rel = bld   # what $(BUILD_DIR) expands to in the XMLs (repo-relative)
    # 1. samples -> aifc (sfc reads them through the XML sample paths)
    tasks = []
    for src, rel in _asset_inputs(ext, "audio/samples", (".wav",)):
        out = os.path.join(bld, "assets", rel[:-4] + ".aifc")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if not force and os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src):
            continue
        mode = "vadpcm-half" if src.endswith(".half.wav") else "vadpcm"
        tasks.append((rel, [tools["sampleconv"], mode, src, out]))
    _parallel(jobs, tasks, "samples")
    # 2. XML copies with $(BUILD_DIR) substituted (BUILD_DIR_REPLACE in the Makefile)
    xmls = {"samplebanks": [], "soundfonts": []}
    for kind in xmls:
        for src, rel in _asset_inputs(ext, f"audio/{kind}", (".xml",)):
            out = os.path.join(bld, "assets", rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            open(out, "w").write(open(src).read().replace("$(BUILD_DIR)", bld_rel))
            xmls[kind].append(out)
    # 3. soundfonts: sfc -> .c/.h/.name (DC's soundfont_table.h includes the .h)
    tasks = []
    for x in xmls["soundfonts"]:
        stem = x[:-4]
        tasks.append((os.path.basename(stem),
                      [tools["sfc"], "--matching", "--makedepend", stem + ".c.d", x,
                       stem + ".c", stem + ".h", stem + ".name"]))
    _parallel(jobs, tasks, "soundfonts (sfc)")
    # 4. tables
    run([tools["atblgen"], "--banks", f"{A}/samplebank_table.h"] + xmls["samplebanks"])
    run([tools["atblgen"], "--fonts", f"{A}/soundfont_table.h"] + xmls["soundfonts"])
    log("audio: samplebank_table.h soundfont_table.h")
    # 5. size headers from the ROM's own audio tables (N64 build: afile_sizes on .o)
    syms = dc_meta.audio_symbols(cfg)
    for prefix, guard, num in (("Soundfont", "SOUNDFONT_SIZES_H_", "NUM_SOUNDFONTS"),
                               ("Sequence", "SEQUENCE_SIZES_H_", "NUM_SEQUENCES")):
        entries = sorted(
            ((int(k[len(prefix) + 1:-5]), v) for k, v in syms.items()
             if k.startswith(prefix + "_") and k.endswith("_Size") and v > 0))
        # size-0 entries are pointer sequences (no .seq / no .o in the N64 build)
        lines = [f"#ifndef {guard}", f"#define {guard}", ""]
        lines += [f"#define {prefix}_{i}_SIZE 0x{sz:X}" for i, sz in entries]
        lines += ["", f"#define {num} {len(entries)}", "", "#endif", ""]
        out = f"{A}/{prefix.lower()}_sizes.h"
        open(out, "w").write("\n".join(lines))
        log(f"audio: {out} ({len(entries)} entries)")


def gen_meta(tools, cfg, bld):
    # The cpp'd spec itself stays in build_dc/meta (dc_meta); build/<v>/spec is left
    # alone -- when a MIPS build sits there it is dc_meta --verify's ground truth.
    spec = dc_meta.preprocess_spec(cfg)
    os.makedirs(bld, exist_ok=True)
    run([tools["mkdmadata"], spec, os.path.join(bld, "dmadata_table_spec.h"),
         os.path.join(bld, "compress_ranges.txt")])
    log("meta: dmadata_table_spec.h compress_ranges.txt")


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", default=os.environ.get("DC_VERSION", "gc-eu-mq-dbg"))
    ap.add_argument("--extracted-root", default="extracted",
                    help="output root (default extracted/ -> extracted/<version>)")
    ap.add_argument("--build-root", default="build",
                    help="output root for generated build inputs (default build/<version>)")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--force", action="store_true", help="regenerate up-to-date outputs too")
    ap.add_argument("--only", default="tools,extract,meta,textures,text,audio",
                    help="comma list of phases to run")
    args = ap.parse_args()

    check_python()
    phases = set(args.only.split(","))
    version = args.version
    cfg = dc_meta.load_version_config(version)
    os.chdir(REPO)
    # Repo-relative paths everywhere: the extractor and atblgen embed the paths they
    # are given in generated #include lines, and the DC build resolves them via -I.
    ext = os.path.relpath(os.path.abspath(os.path.join(args.extracted_root, version)), REPO)
    bld = os.path.relpath(os.path.abspath(os.path.join(args.build_root, version)), REPO)
    t0 = time.time()
    log(f"version {version} ({cfg['platform']}, {cfg['region']}); extracted={ext} build={bld}")

    tools = ensure_tools(find_make())
    if "extract" in phases:
        extract(version, ext, args.jobs)
    if "meta" in phases:
        gen_meta(tools, cfg, bld)
    if "textures" in phases:
        gen_textures(tools, ext, bld, args.jobs, args.force)
    if "text" in phases:
        gen_text(cfg, ext, bld)
    if "audio" in phases:
        gen_audio(tools, cfg, ext, bld, args.jobs, args.force)
    log(f"done in {time.time() - t0:.1f}s")


if __name__ == "__main__":
    main()
