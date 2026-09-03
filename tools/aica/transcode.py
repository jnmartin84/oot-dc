#!/usr/bin/env python3
"""
Full per-sample transcode: VADPCM -> PCM16 -> (2x decimate if >64K) -> AICA
Yamaha ADPCM, producing the final ADPCM bytes + descriptor. Shared by the
build-time emitter.

Decimation is scipy-optional: scipy.signal.resample_poly is used when available
(the output that was ear-checked/approved), else a stdlib windowed-sinc FIR so a
clean checkout builds with only stock Python 3.
"""

import array
import hashlib
import math
import os
import struct

try:
    import numpy as np
    from scipy.signal import resample_poly
    _HAVE_SCIPY = True
except Exception:
    _HAVE_SCIPY = False
    # scipy's resample_poly is required for the approved 2x-downsample output. The
    # stdlib decimate fallback produces WRONG ADPCM -> silent/broken sampled audio.
    # Fail loudly by default so this never silently ships (opt back in explicitly).
    if os.environ.get("AICA_ALLOW_STDLIB_DECIMATE") != "1":
        raise ImportError(
            "tools/aica/transcode.py requires scipy (+numpy) for correct sample "
            "downsampling; the stdlib fallback produces broken sampled audio. "
            "Create/populate the build venv and use it:\n"
            "    python3 -m venv .venv && .venv/bin/pip install -U -r requirements.txt\n"
            "    make -f Makefile.dc PYTHON=.venv/bin/python3 ...\n"
            "To deliberately accept the inferior stdlib path, set "
            "AICA_ALLOW_STDLIB_DECIMATE=1."
        )

import vadpcm
import yamaha_adpcm
import yamaha_adpcm_v2 as ya2

AICA_MAX = 65534
BEAM = int(os.environ.get("AICA_BEAM", "32"))

# --- beam-encode result cache: deterministic in (pcm, BEAM, ENC_VERSION), so
# re-runs skip re-encoding unchanged samples. Safe under multiprocessing (atomic
# replace). Bump ENC_VERSION whenever yamaha_adpcm_v2.encode changes. ---
ENC_VERSION = b"ya2-beam-1"
_CACHE_DIR = os.environ.get("AICA_ENCODE_CACHE",
                            os.path.join(os.path.dirname(os.path.abspath(__file__)), ".encode_cache"))


def beam_encode_cached(pcm):
    """ya2.encode(pcm, beam=BEAM) with an on-disk cache. Returns (adpcm_bytes, n)."""
    h = hashlib.sha1(ENC_VERSION)
    h.update(b"|beam=%d|" % BEAM)
    h.update(array.array("i", pcm).tobytes())
    path = os.path.join(_CACHE_DIR, h.hexdigest())
    try:
        with open(path, "rb") as f:
            n = struct.unpack("<I", f.read(4))[0]
            return f.read(), n
    except OSError:
        pass
    adpcm, n = ya2.encode(pcm, beam=BEAM)
    os.makedirs(_CACHE_DIR, exist_ok=True)
    tmp = "%s.tmp%d" % (path, os.getpid())
    with open(tmp, "wb") as f:
        f.write(struct.pack("<I", n))
        f.write(adpcm)
    os.replace(tmp, path)
    return adpcm, n


# AICA_SM_* sample-format codes (dc/sound/aica_comm.h).
FMT_PCM16, FMT_PCM8, FMT_ADPCM = 0, 1, 2
PROMOTE_SNR_DB = float(os.environ.get("AICA_PROMOTE_SNR_DB", "16.0"))   # OoT RAM-tuned (knee); SM64 used 18
PCM16_MAX_SAMPLES = int(os.environ.get("AICA_PCM16_MAX_SAMPLES", "2048"))
# Samples the game pitches above +1 octave (nf>2.0): AICA ADPCM pitch-clamps
# there, so they must be PCM. Filled from the AICA_OCTAVE_LOG sweep; keyed by
# src_offset (the runtime lookup key). Empty until OoT is swept.
FORCE_PCM_KEYS = {int(k, 16) for k in os.environ.get("AICA_FORCE_PCM_KEYS", "").replace(",", " ").split()}

# --- optional baked-in software reverb (AICA_REVERB=1) -------------------------
# A light Freeverb (Schroeder comb+allpass) convolved into each sample's PCM
# BEFORE ADPCM/PCM encode, so the dry hardware path carries a touch of room.
# Off by default -> byte-identical builds. Ported from sm64-dc; one-shots gain a
# decaying tail (capped to AICA_MAX), looped samples are run to steady state so
# the loop region stays periodic across the seam, with the head crossfaded in.
# NOTE: OoT RAM is tight (~600 KB free after the pool loads), and reverb both adds
# one-shot tails AND lowers ADPCM SNR (more PCM promotions) -> the pool grows.
# Keep an eye on the emitted pool size; trim REVERB_TAIL_CAP if it overflows.
REVERB = os.environ.get("AICA_REVERB", "0") not in ("0", "", "false", "False")
REVERB_RATE = 32000            # nominal playback rate used to scale delay lengths
# OoT-specific: tighter than SM64 to fit ~620 KB free RAM. Tails dominate the
# pool growth, so room/floor/cap are cut hard; wet is also dropped a notch.
REVERB_DRY = 0.86              # dry gain (<1 for summing headroom)
REVERB_WET = 0.20              # wet gain -- lighter than SM64 (0.26) for RAM
REVERB_INPUT_GAIN = 0.015      # Freeverb pre-scale into the comb bank
REVERB_ROOMSIZE = 0.42         # smaller room -> shorter RT -> shorter tails
REVERB_DAMPING = 0.5           # -> comb lowpass damp1 = damping*0.4
REVERB_FEEDBACK = REVERB_ROOMSIZE * 0.28 + 0.7
REVERB_DAMP1 = REVERB_DAMPING * 0.4
REVERB_XFADE = 96              # head->steady-loop crossfade length (samples)
REVERB_PREWARM = 48000         # loop samples run to reach steady state (~>RT60)
REVERB_MAX_PREWARM = 72000     # hard cap on warmup work for tiny loops
REVERB_TAIL_CAP = 11000        # hard cap on one-shot tail appended (samples ~0.34s)
REVERB_TAIL_FLOOR = 0.022      # stop the tail once it decays this far below body peak (-33 dB)
REVERB_TAIL_QUIET = 256        # ...for this many consecutive samples


def _snr_db(ref, test):
    sig = err = 0.0
    for a, b in zip(ref, test):
        sig += float(a) * a
        err += float(a - b) * (a - b)
    if err == 0.0:
        return 99.0
    if sig == 0.0:
        return -99.0
    return 10.0 * math.log10(sig / err)


def _pcm16_bytes(pcm):
    return struct.pack(f"<{len(pcm)}h", *(_clamp16(v) for v in pcm))


def _pcm8_bytes(pcm):
    # AICA 8-bit is signed linear (top 8 bits of the 16-bit sample), rounded.
    out = bytearray(len(pcm))
    for i, v in enumerate(pcm):
        q = (v + 128) >> 8
        q = -128 if q < -128 else 127 if q > 127 else q
        out[i] = q & 0xFF
    return bytes(out)


def _clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def _decimate2(pcm):
    """Anti-aliased 2x downsample. scipy if present (matches approved output);
    else a 23-tap Hamming-windowed-sinc lowpass (fc=0.25) + take every 2nd."""
    if _HAVE_SCIPY:
        dec = resample_poly(np.asarray(pcm, dtype=np.float64), 1, 2)
        return [_clamp16(int(round(v))) for v in dec]
    N, fc = 23, 0.25
    mid = (N - 1) // 2
    h = []
    for i in range(N):
        x = i - (N - 1) / 2.0
        s = 2 * fc if x == 0 else math.sin(2 * math.pi * fc * x) / (math.pi * x)
        w = 0.54 - 0.46 * math.cos(2 * math.pi * i / (N - 1))
        h.append(s * w)
    g = sum(h)
    h = [c / g for c in h]
    L = len(pcm)
    out = []
    for n in range(0, L, 2):
        acc = 0.0
        for k in range(N):
            idx = n + k - mid
            idx = 0 if idx < 0 else (L - 1 if idx >= L else idx)
            acc += pcm[idx] * h[k]
        out.append(_clamp16(int(round(acc))))
    return out


class _Comb:
    """Lowpass-feedback comb filter (one Freeverb voice)."""
    __slots__ = ("buf", "idx", "store", "fb", "d1", "d2")

    def __init__(self, size, fb, d1):
        self.buf = [0.0] * size
        self.idx = 0
        self.store = 0.0
        self.fb = fb
        self.d1 = d1
        self.d2 = 1.0 - d1

    def process(self, x):
        y = self.buf[self.idx]
        self.store = y * self.d2 + self.store * self.d1
        self.buf[self.idx] = x + self.store * self.fb
        self.idx += 1
        if self.idx >= len(self.buf):
            self.idx = 0
        return y


class _Allpass:
    __slots__ = ("buf", "idx", "fb")

    def __init__(self, size, fb):
        self.buf = [0.0] * size
        self.idx = 0
        self.fb = fb

    def process(self, x):
        bufout = self.buf[self.idx]
        y = bufout - x
        self.buf[self.idx] = x + bufout * self.fb
        self.idx += 1
        if self.idx >= len(self.buf):
            self.idx = 0
        return y


class _Freeverb:
    """Mono Freeverb: 8 parallel combs -> 4 series allpasses. Delay tunings are
    the classic 44.1 kHz values scaled to REVERB_RATE."""
    _COMB = (1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617)
    _ALLP = (556, 441, 341, 225)

    def __init__(self, rate):
        sc = rate / 44100.0
        self.combs = [_Comb(max(1, int(round(t * sc))), REVERB_FEEDBACK, REVERB_DAMP1)
                      for t in self._COMB]
        self.allps = [_Allpass(max(1, int(round(t * sc))), 0.5) for t in self._ALLP]

    def process(self, x):
        inp = x * REVERB_INPUT_GAIN
        out = 0.0
        for c in self.combs:
            out += c.process(inp)
        for a in self.allps:
            out = a.process(out)
        return out


def _apply_reverb(pcm, has_loop, loop_start, loop_end):
    """Bake a light reverb into pcm. One-shots gain a decaying tail (capped to
    AICA_MAX); looped samples run the loop region to steady state so it stays
    periodic across the seam, then crossfade the played-once head into it.
    Returns (pcm, loop_start, loop_end)."""
    rv = _Freeverb(REVERB_RATE)

    if not has_loop:
        out = [_clamp16(int(round(x * REVERB_DRY + rv.process(x) * REVERB_WET)))
               for x in pcm]
        floor = max(1.0, max((abs(v) for v in out), default=0) * REVERB_TAIL_FLOOR)
        room = max(0, AICA_MAX - len(out))
        quiet = 0
        for _ in range(min(REVERB_TAIL_CAP, room)):
            w = rv.process(0.0) * REVERB_WET
            out.append(_clamp16(int(round(w))))
            if abs(w) < floor:
                quiet += 1
                if quiet >= REVERB_TAIL_QUIET:
                    break
            else:
                quiet = 0
        return out, loop_start, loop_end

    P = loop_end - loop_start
    if P <= 0:
        out = [_clamp16(int(round(x * REVERB_DRY + rv.process(x) * REVERB_WET)))
               for x in pcm]
        return out, loop_start, loop_end

    head = pcm[:loop_start]
    loop = pcm[loop_start:loop_end]
    head_out = [x * REVERB_DRY + rv.process(x) * REVERB_WET for x in head]

    reps = max(2, REVERB_PREWARM // P + 1)
    reps = min(reps, max(2, REVERB_MAX_PREWARM // P))
    for _ in range(reps - 1):
        for x in loop:
            rv.process(x)
    loop_out = [x * REVERB_DRY + rv.process(x) * REVERB_WET for x in loop]

    # Smooth the one-time head->loop entry: the steady loop is already seamless at
    # its own wrap (loop_end->loop_start) by periodicity, so we only need to morph
    # the tail of the played-once head into the steady loop's natural pre-roll.
    xf = min(REVERB_XFADE, len(head_out), P)
    base = len(head_out) - xf
    for i in range(xf):
        t = (i + 1) / (xf + 1)
        head_out[base + i] = (1.0 - t) * head_out[base + i] + t * loop_out[P - xf + i]

    out = [_clamp16(int(round(v))) for v in head_out]
    out.extend(_clamp16(int(round(v))) for v in loop_out)
    return out, loop_start, loop_start + P


def transcode_sample(s, force=False):
    """s: albank_parse.Sample (parsed with_data=True). Returns descriptor dict.
    Beam-encodes to ADPCM; promotes to PCM if SNR < PROMOTE_SNR_DB or force
    (16-bit when short, else 8-bit). `force` = src_offset in FORCE_PCM_KEYS, decided
    by the caller (transcode_sample lacks the bank base to compute src_offset)."""
    pcm = vadpcm.decode(s.data, s.codec, s.order, s.npredictors, s.book)
    shift = 0
    loop_start, loop_end = s.loop_start, s.loop_end

    if len(pcm) > AICA_MAX:
        # 2x trick: anti-aliased decimate by 2, play back at half freq at runtime.
        pcm = _decimate2(pcm)
        shift = 1
        loop_start //= 2
        loop_end //= 2

    if REVERB:
        pcm, loop_start, loop_end = _apply_reverb(pcm, s.has_loop, loop_start, loop_end)

    adpcm, n = beam_encode_cached(pcm)
    assert n <= AICA_MAX, (s.bank, s.addr, n)
    snr = _snr_db(pcm[:n], ya2.decode(adpcm, n)[:n])

    if force or snr < PROMOTE_SNR_DB:
        if n <= PCM16_MAX_SAMPLES:
            fmt, data = FMT_PCM16, _pcm16_bytes(pcm[:n])
        else:
            fmt, data = FMT_PCM8, _pcm8_bytes(pcm[:n])
    else:
        fmt, data = FMT_ADPCM, adpcm

    if not s.has_loop:
        # one-shot: AICA still needs loopend = sample count as the play length
        loop_start, loop_end = 0, n

    return {
        "bank": s.bank,
        "addr": s.addr,
        "data": data,
        "fmt": fmt,
        "snr": snr,
        "nsamples": n,
        "loop": bool(s.has_loop),
        "loop_start": loop_start,
        "loop_end": min(loop_end, n),
        "downsample_shift": shift,
    }
