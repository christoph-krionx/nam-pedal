#!/usr/bin/env python3
"""
convert_ir.py — resample and trim a cabinet IR WAV to exactly N taps at 48 kHz.

Usage:
  python convert_ir.py INPUT.wav OUTPUT.wav [--taps 256] [--rate 48000] [--fade 128]

  --taps N   Target length in samples (default: 256, matches MAX_IR_TAPS in firmware)
  --rate HZ  Target sample rate (default: 48000)
  --fade N   Cosine fade-out applied to the last N samples of the tail
             to prevent a hard-truncation click (default: 128, 0 = off)

Uses scipy.signal.resample_poly when available (highest quality); falls back
to numpy linear interpolation, then to pure-Python linear interpolation.

The output is a mono IEEE-float-32 WAV at the target rate, ready to pass
directly to pack_models.py as the IR argument:

  python pack_models.py --flash "Amp:model.namb:OUTPUT.wav"
"""

import argparse
import math
import os
import struct
import sys


# ── WAV I/O ───────────────────────────────────────────────────────────────

def read_wav_mono_f32(path):
    """Return (samples: list[float], sample_rate: int).

    Supports PCM 16/24/32-bit and IEEE float 32-bit.
    Multi-channel files use channel 0 only.
    Samples are normalised to [-1, 1].
    """
    try:
        raw = open(path, 'rb').read()
    except OSError as e:
        sys.exit(f'error: {e}')

    if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
        sys.exit(f'error: {path}: not a WAV file')

    fmt_chunk = data_chunk = None
    pos = 12
    while pos + 8 <= len(raw):
        tag  = raw[pos:pos + 4]
        clen = struct.unpack_from('<I', raw, pos + 4)[0]
        body = raw[pos + 8: pos + 8 + clen]
        if tag == b'fmt ':
            fmt_chunk = body
        elif tag == b'data':
            data_chunk = body
        pos += 8 + clen + (clen & 1)  # word-align per RIFF spec

    if fmt_chunk is None or data_chunk is None:
        sys.exit(f'error: {path}: missing fmt or data chunk')

    audio_fmt   = struct.unpack_from('<H', fmt_chunk, 0)[0]
    num_ch      = struct.unpack_from('<H', fmt_chunk, 2)[0]
    sample_rate = struct.unpack_from('<I', fmt_chunk, 4)[0]
    bps         = struct.unpack_from('<H', fmt_chunk, 14)[0]

    if audio_fmt == 0xFFFE and len(fmt_chunk) >= 26:   # EXTENSIBLE
        audio_fmt = struct.unpack_from('<H', fmt_chunk, 24)[0]

    frame_bytes = (bps // 8) * num_ch

    def s24(d, off):
        v = d[off] | (d[off+1] << 8) | (d[off+2] << 16)
        return v - 0x1000000 if v >= 0x800000 else v

    n = len(data_chunk) // frame_bytes
    if   audio_fmt == 3 and bps == 32:
        raw_s = list(struct.unpack_from(f'<{n * num_ch}f', data_chunk))
    elif audio_fmt == 1 and bps == 16:
        raw_s = [s / 32768.0 for s in struct.unpack_from(f'<{n * num_ch}h', data_chunk)]
    elif audio_fmt == 1 and bps == 24:
        raw_s = [s24(data_chunk, i * 3) / 8388608.0 for i in range(n * num_ch)]
    elif audio_fmt == 1 and bps == 32:
        raw_s = [s / 2147483648.0 for s in struct.unpack_from(f'<{n * num_ch}i', data_chunk)]
    else:
        sys.exit(f'error: {path}: unsupported format {audio_fmt}/{bps}-bit')

    return raw_s[::num_ch], sample_rate   # channel 0


def write_wav_f32_mono(path, samples, sample_rate):
    """Write a mono IEEE-float-32 WAV."""
    n    = len(samples)
    data = struct.pack(f'<{n}f', *samples)
    fmt  = struct.pack('<HHIIHH',
        3,               # IEEE float
        1,               # mono
        sample_rate,
        sample_rate * 4, # byte rate
        4,               # block align
        32)              # bits per sample

    out  = b'RIFF'
    out += struct.pack('<I', 4 + (8 + len(fmt)) + (8 + len(data)))
    out += b'WAVE'
    out += b'fmt '  + struct.pack('<I', len(fmt))  + fmt
    out += b'data'  + struct.pack('<I', len(data)) + data

    with open(path, 'wb') as f:
        f.write(out)


# ── Resampling ────────────────────────────────────────────────────────────

def resample_to(samples, from_rate, to_rate):
    """Resample, preferring scipy > numpy > pure-Python."""
    if from_rate == to_rate:
        return samples

    # Best quality: scipy polyphase
    try:
        from scipy.signal import resample_poly
        from math import gcd
        import numpy as np
        g     = gcd(to_rate, from_rate)
        up, down = to_rate // g, from_rate // g
        print(f'  scipy.signal.resample_poly  up={up}  down={down}')
        return list(resample_poly(np.array(samples, dtype='float64'), up, down))
    except ImportError:
        pass

    # Good quality: numpy linear interpolation
    try:
        import numpy as np
        n_out  = round(len(samples) * to_rate / from_rate)
        old_t  = np.linspace(0.0, 1.0, len(samples))
        new_t  = np.linspace(0.0, 1.0, n_out)
        print('  numpy linear interpolation (install scipy for higher quality)')
        return list(np.interp(new_t, old_t, samples))
    except ImportError:
        pass

    # Fallback: pure Python linear interpolation
    n_in  = len(samples)
    n_out = round(n_in * to_rate / from_rate)
    print('  pure-Python linear interpolation (install numpy/scipy for higher quality)')
    result = []
    for i in range(n_out):
        t  = i * (n_in - 1) / max(n_out - 1, 1)
        lo = int(t)
        hi = min(lo + 1, n_in - 1)
        result.append(samples[lo] + (t - lo) * (samples[hi] - samples[lo]))
    return result


# ── Trim / pad / fade ─────────────────────────────────────────────────────

def trim_pad_fade(samples, n_taps, fade_len):
    """Trim or zero-pad to n_taps, then apply a cosine fade-out to the tail."""
    if len(samples) >= n_taps:
        samples = list(samples[:n_taps])
    else:
        samples = list(samples) + [0.0] * (n_taps - len(samples))

    if fade_len > 0:
        fade_len = min(fade_len, n_taps)
        start    = n_taps - fade_len
        for i in range(fade_len):
            t           = i / max(fade_len - 1, 1)
            w           = 0.5 * (1.0 + math.cos(math.pi * t))  # 1 → 0
            samples[start + i] *= w

    return samples


# ── Entry point ───────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input',  help='Input IR WAV file')
    ap.add_argument('output', help='Output WAV file (float32 mono)')
    ap.add_argument('--taps', type=int, default=256,
                    help='Target length in taps (default: 256)')
    ap.add_argument('--rate', type=int, default=48000,
                    help='Target sample rate in Hz (default: 48000)')
    ap.add_argument('--fade', type=int, default=128,
                    help='Tail fade-out length in samples (default: 128, 0 = off)')
    args = ap.parse_args()

    samples, sr = read_wav_mono_f32(args.input)
    n_in = len(samples)
    print(f'input:  {os.path.basename(args.input)}'
          f'  {n_in} taps @ {sr} Hz  ({n_in / sr * 1000:.1f} ms)')

    if sr != args.rate:
        print(f'resampling {sr} Hz → {args.rate} Hz ...')
        samples = resample_to(samples, sr, args.rate)
        print(f'  {len(samples)} taps after resample')

    action = 'trimmed' if len(samples) > args.taps else 'zero-padded'
    samples = trim_pad_fade(samples, args.taps, args.fade)
    fade_note = f', cosine fade last {args.fade} samples' if args.fade > 0 else ''
    print(f'{action} → {args.taps} taps{fade_note}')

    peak   = max(abs(s) for s in samples)
    peak_db = 20 * math.log10(peak) if peak > 0 else float('-inf')
    print(f'peak:   {peak:.5f}  ({peak_db:.1f} dBFS)')
    ms = args.taps / args.rate * 1000
    print(f'output: {os.path.basename(args.output)}'
          f'  {args.taps} taps @ {args.rate} Hz  ({ms:.1f} ms)')

    write_wav_f32_mono(args.output, samples, args.rate)


if __name__ == '__main__':
    main()
