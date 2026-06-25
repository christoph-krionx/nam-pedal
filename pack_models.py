#!/usr/bin/env python3
"""
pack_models.py — pack .namb model files (+ optional cabinet IRs) into a
NAMPedal model bank.

The bank is written to a fixed QSPI flash region (default 0x90800000),
completely independent of the firmware binary.  Models can be swapped by
reflashing only the bank; the firmware image is never touched.

Usage:
  pack_models.py [options] NAME:MODEL[:IR] [NAME:MODEL[:IR] ...]

  NAME   display name stored in the bank (max 31 chars)
  MODEL  path to a .namb model file, or a .nam file (plain WaveNet or
         SlimmableContainer — converted automatically via nam2namb)
  IR     optional path to a cabinet IR WAV file
         (PCM 16/24/32-bit or IEEE float 32-bit, mono or multi-channel)
         Only channel 0 is used.  IR is truncated to MAX_IR_TAPS samples
         (256 taps ≈ 5.3 ms @ 48 kHz).

Options:
  -o FILE       Output file (default: model_bank.bin)
  --slim FLOAT  Slim factor for SlimmableContainer .nam files (default: 0.0).
                Selects the first submodel whose max_value > FLOAT, giving
                the smallest model for values near 0.
  --flash       Flash the bank to a connected Daisy (requires: pip install pyusb)
  --port PORT   Serial port of the running pedal (e.g. /dev/tty.usbmodemXXXX).
                When given, sends a DFU trigger byte over USB CDC so the pedal
                resets into DFU mode automatically before flashing.
                Without --port the Daisy must already be in DFU mode.
  --addr ADDR   Target flash address in hex (default: 0x90600000)
  --list FILE   List models stored in an existing bank file and exit

Requires the Daisy bootloader (BOOT_QSPI firmware target).
Flashing uses pydfu.py (MIT) vendored alongside this script.
--port requires pyserial (pip install pyserial).

Examples:
  pack_models.py --flash --port /dev/tty.usbmodem0001 "TonePlier:tone_plier.nam:mesa_cab.wav"
  pack_models.py --slim 0.0 "Amp1:model1.nam" "Amp2:model2.nam:cab.wav"
  pack_models.py -o bank.bin Fender:clean.namb:twin_reverb.wav
  pack_models.py --list model_bank.bin
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import time

BANK_MAGIC   = 0x504D414E  # "NAMP"
BANK_VERSION = 1
HEADER_SIZE  = 64          # bytes
ENTRY_SIZE   = 48          # name(32) + namb_offset(4) + namb_size(4) + ir_offset(4) + ir_num_samples(4)
NAMB_MAGIC   = 0x4E414D42  # "NAMB"
MAX_IR_TAPS  = 256         # must match MAX_IR_TAPS in NAMPedal.cpp


# ── WAV reader ────────────────────────────────────────────────────────────

def _s24_le(data, offset):
    v = data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16)
    return v - 0x1000000 if v >= 0x800000 else v


def read_wav_mono_f32(path):
    """Return (samples: list[float], sample_rate: int) from a WAV file.

    Supports PCM 16/24/32-bit and IEEE float 32-bit.  Multi-channel files are
    reduced to mono by taking channel 0 only.  Samples are normalised to [-1, 1].
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

    # WAVE_FORMAT_EXTENSIBLE — read sub-format GUID's first word.
    if audio_fmt == 0xFFFE and len(fmt_chunk) >= 26:
        audio_fmt = struct.unpack_from('<H', fmt_chunk, 24)[0]

    frame_bytes = (bps // 8) * num_ch

    if audio_fmt == 3 and bps == 32:
        n = len(data_chunk) // frame_bytes
        interleaved = list(struct.unpack_from(f'<{n * num_ch}f', data_chunk))
    elif audio_fmt == 1 and bps == 16:
        n = len(data_chunk) // frame_bytes
        interleaved = [s / 32768.0
                       for s in struct.unpack_from(f'<{n * num_ch}h', data_chunk)]
    elif audio_fmt == 1 and bps == 24:
        n = len(data_chunk) // frame_bytes
        interleaved = [_s24_le(data_chunk, i * 3) / 8388608.0
                       for i in range(n * num_ch)]
    elif audio_fmt == 1 and bps == 32:
        n = len(data_chunk) // frame_bytes
        interleaved = [s / 2147483648.0
                       for s in struct.unpack_from(f'<{n * num_ch}i', data_chunk)]
    else:
        sys.exit(f'error: {path}: unsupported format code {audio_fmt} / {bps}-bit')

    mono = interleaved[::num_ch]  # channel 0 only
    return mono, sample_rate


# ── IR processing pipeline ────────────────────────────────────────────────

def _ir_resample(samples, from_rate, to_rate):
    if from_rate == to_rate:
        return samples
    try:
        from scipy.signal import resample_poly
        from math import gcd
        import numpy as np
        g = gcd(to_rate, from_rate)
        return list(resample_poly(np.array(samples, dtype='float64'),
                                  to_rate // g, from_rate // g))
    except ImportError:
        pass
    try:
        import numpy as np
        n_out = round(len(samples) * to_rate / from_rate)
        return list(np.interp(np.linspace(0, 1, n_out),
                               np.linspace(0, 1, len(samples)),
                               samples))
    except ImportError:
        pass
    n_in = len(samples)
    n_out = round(n_in * to_rate / from_rate)
    result = []
    for i in range(n_out):
        t = i * (n_in - 1) / max(n_out - 1, 1)
        lo = int(t); hi = min(lo + 1, n_in - 1)
        result.append(samples[lo] + (t - lo) * (samples[hi] - samples[lo]))
    return result


def _ir_minimum_phase(samples, eps=1e-8):
    """Minimum-phase conversion via real cepstrum. Requires numpy."""
    import numpy as np
    n = len(samples)
    if n < 4:
        return samples
    fft_size = 1
    while fft_size < n:
        fft_size <<= 1
    fft_size <<= 2  # 4× to prevent cepstral aliasing

    h    = np.array(samples, dtype=np.float64)
    H    = np.fft.rfft(h, n=fft_size)
    mag  = np.abs(H)
    peak = float(np.max(mag))
    if peak < 1e-30:
        return samples

    log_mag  = np.log(np.maximum(mag, eps * peak))
    cepstrum = np.fft.irfft(log_mag, n=fft_size)

    fold = np.zeros(fft_size)
    fold[0]                   = cepstrum[0]
    fold[1:fft_size // 2]     = 2.0 * cepstrum[1:fft_size // 2]
    fold[fft_size // 2]       = cepstrum[fft_size // 2]

    h_mp = np.fft.irfft(np.exp(np.fft.rfft(fold, n=fft_size)), n=fft_size)
    return list(h_mp[:n])


def _ir_trim_taper(samples, n_taps, fade_len=64):
    """Truncate to n_taps with a Hann half-window over the last fade_len samples."""
    import math as _math
    out = list(samples[:n_taps]) if len(samples) >= n_taps \
          else list(samples) + [0.0] * (n_taps - len(samples))
    fade_len = min(fade_len, n_taps)
    start = n_taps - fade_len
    for i in range(fade_len):
        t = i / max(fade_len - 1, 1)
        out[start + i] *= 0.5 * (1.0 + _math.cos(_math.pi * t))
    return out


def _ir_normalize(samples):
    peak = max(abs(s) for s in samples)
    if peak < 1e-30:
        return list(samples)
    return [s / peak for s in samples]


def process_ir(samples, sample_rate, target_rate=48000, max_taps=MAX_IR_TAPS):
    """Full IR pipeline: resample → min-phase → truncate+taper → normalize."""
    # Resample
    if sample_rate != target_rate:
        print(f'       resampling {sample_rate} Hz → {target_rate} Hz...')
        samples = _ir_resample(samples, sample_rate, target_rate)

    # Minimum-phase conversion
    try:
        samples = _ir_minimum_phase(samples)
        print(f'       min-phase: applied')
    except ImportError:
        print(f'       min-phase: skipped (pip install numpy for better truncation)')

    # Truncate + Hann tail taper
    n_in = len(samples)
    fade = 64  # 25% of 256; chosen to smooth the truncation without sacrificing body
    samples = _ir_trim_taper(samples, max_taps, fade_len=fade)
    if n_in > max_taps:
        print(f'       truncated {n_in} → {max_taps} taps '
              f'({n_in / target_rate * 1000:.1f} ms → {max_taps / target_rate * 1000:.1f} ms), '
              f'Hann tail {fade} samples')

    # Normalize
    peak = max(abs(s) for s in samples)
    if peak > 1e-30:
        samples = _ir_normalize(samples)
        print(f'       normalized: peak was {20 * math.log10(peak):.1f} dBFS → 0.0 dBFS')

    return samples


# ── .nam → .namb conversion via nam2namb ─────────────────────────────────

def find_nam2namb():
    """Locate the nam2namb binary, checking known relative paths then PATH."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(script_dir, 'nam2namb'),
        os.path.join(script_dir, '..', 'nam-binary-loader', 'build', 'nam2namb'),
        os.path.join(script_dir, '..', 'nam-binary-loader', 'build_tools', 'nam2namb'),
    ]
    for path in candidates:
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return os.path.normpath(path)
    # Fall back to PATH
    import shutil
    found = shutil.which('nam2namb')
    if found:
        return found
    sys.exit(
        'error: nam2namb binary not found.\n'
        'Build it with: cd nam-binary-loader && cmake -B build_tools -DBUILD_TOOLS=ON && cmake --build build_tools\n'
        'Or place the binary next to pack_models.py.'
    )


def nam_to_namb(path, slim_factor=0.0):
    """Convert a .nam file to .namb bytes using the nam2namb binary."""
    binary = find_nam2namb()
    with tempfile.NamedTemporaryFile(suffix='.namb', delete=False) as tmp:
        tmp_path = tmp.name
    try:
        cmd = [binary, '--slim', str(slim_factor), path, tmp_path]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            sys.exit(f'error: nam2namb failed:\n{result.stderr.strip()}')
        if result.stderr:
            # nam2namb prints selection info to stderr — relay it
            for line in result.stderr.strip().splitlines():
                print(f'  {line}')
        return open(tmp_path, 'rb').read()
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)


# ── model reader (dispatches on file extension) ───────────────────────────

def read_model(path, slim_factor=0.0):
    """Return .namb bytes from a .namb file, or convert a .nam file first."""
    if path.endswith('.nam'):
        return nam_to_namb(path, slim_factor)
    # .namb — read and validate magic
    try:
        data = open(path, 'rb').read()
    except OSError as e:
        sys.exit(f'error: {e}')
    if len(data) < 4:
        sys.exit(f'error: {path}: file too small')
    magic, = struct.unpack_from('<I', data, 0)
    if magic != NAMB_MAGIC:
        sys.exit(f'error: {path}: not a .namb file (magic=0x{magic:08X})')
    return data


# ── Bundle builder ────────────────────────────────────────────────────────

def build_bank(models):
    """Pack models into a binary bank.

    models: list of (name: str, namb: bytes, ir: bytes | None)
    Returns: bytes
    """
    n = len(models)
    data_base = HEADER_SIZE + n * ENTRY_SIZE

    # Compute blob offsets (namb then optional IR per model, 4-byte aligned).
    namb_offsets = []
    ir_offsets   = []
    cur = data_base
    for _name, namb, ir in models:
        namb_offsets.append(cur)
        cur += len(namb)
        cur = (cur + 3) & ~3
        if ir is not None:
            ir_offsets.append(cur)
            cur += len(ir)
            cur = (cur + 3) & ~3
        else:
            ir_offsets.append(0)

    # Header: magic(4) version(4) num_models(4) + padding to 64 bytes.
    hdr = struct.pack('<III', BANK_MAGIC, BANK_VERSION, n).ljust(HEADER_SIZE, b'\x00')

    # Entries: name(32) namb_offset(4) namb_size(4) ir_offset(4) ir_num_samples(4).
    entries = b''
    for i, (name, namb, ir) in enumerate(models):
        name_b = name.encode('ascii', errors='replace')[:31].ljust(32, b'\x00')
        ir_num = len(ir) // 4 if ir is not None else 0
        entries += name_b + struct.pack('<IIII',
                                       namb_offsets[i], len(namb),
                                       ir_offsets[i],   ir_num)
    assert len(entries) == n * ENTRY_SIZE

    # Data blobs (4-byte aligned padding between them).
    blobs = b''
    for _name, namb, ir in models:
        blobs += namb + b'\x00' * ((4 - len(namb) % 4) % 4)
        if ir is not None:
            blobs += ir + b'\x00' * ((4 - len(ir) % 4) % 4)

    return hdr + entries + blobs


# ── Bank inspector ────────────────────────────────────────────────────────

def list_bank(path):
    try:
        raw = open(path, 'rb').read()
    except OSError as e:
        sys.exit(f'error: {e}')
    if len(raw) < HEADER_SIZE:
        sys.exit('error: file too small to be a model bank')
    magic, version, n = struct.unpack_from('<III', raw, 0)
    if magic != BANK_MAGIC:
        sys.exit(f'error: not a model bank (magic=0x{magic:08X})')
    print(f'Model bank v{version}: {n} model(s), {len(raw)} bytes total')
    for i in range(n):
        base = HEADER_SIZE + i * ENTRY_SIZE
        name = raw[base:base + 32].split(b'\x00')[0].decode('ascii', errors='replace')
        namb_off, namb_sz, ir_off, ir_num = struct.unpack_from('<IIII', raw, base + 32)
        ir_str = (f'IR {ir_num} taps ({ir_num / 48.0:.1f} ms)' if ir_num
                  else 'no IR')
        print(f'  [{i + 1}] "{name}"  .namb {namb_sz} B  {ir_str}')


# ── Flasher ───────────────────────────────────────────────────────────────

def trigger_dfu(port):
    """Send the DFU trigger byte ('D') to the pedal over USB CDC."""
    try:
        import serial
    except ImportError:
        sys.exit('error: pyserial is required for --port  (pip install pyserial)')

    print(f'sending DFU trigger to {port}...')
    try:
        s = serial.Serial(port, baudrate=115200, timeout=1)
        s.dtr = True      # macOS USB CDC requires DTR asserted to forward data
        time.sleep(0.2)   # let the CDC connection settle before writing
        s.write(b'D')
        s.flush()         # ensure the USB packet is transmitted before close
        time.sleep(0.5)   # keep the port open until the packet is on the wire
        s.close()
    except Exception as e:
        sys.exit(f'error: could not open {port}: {e}')


def flash(path, addr, dfu_timeout=15.0):
    # pydfu.py (MIT) is vendored alongside this script; requires pyusb (BSD).
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import pydfu

    data = open(path, 'rb').read()
    print('waiting for Daisy DFU device (0483:df11)...', end='', flush=True)
    deadline = time.monotonic() + dfu_timeout
    while True:
        try:
            pydfu.init(idVendor=0x0483, idProduct=0xdf11)
            print(' connected')
            break
        except ValueError:
            if time.monotonic() >= deadline:
                print()
                sys.exit(
                    f'error: DFU device not found after {dfu_timeout:.0f} s\n'
                    f'(use --port /dev/tty.usbmodemXXXX to trigger DFU automatically,\n'
                    f' or put the Daisy into DFU mode manually before flashing)')
            print('.', end='', flush=True)
            time.sleep(0.5)

    elements = [{'addr': addr, 'size': len(data), 'data': bytearray(data)}]
    pydfu.write_elements(elements, mass_erase_used=False, progress=pydfu.cli_progress)
    pydfu.exit_dfu()
    print('flash complete')


# ── CLI ───────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('models', nargs='*', metavar='NAME:MODEL[:IR]')
    p.add_argument('-o', '--output', default='model_bank.bin', metavar='FILE')
    p.add_argument('--slim', type=float, default=0.0, metavar='FLOAT',
                   help='slim factor for SlimmableContainer .nam files (default: 0.0)')
    p.add_argument('--flash', action='store_true',
                   help='flash to Daisy via pydfu after building')
    p.add_argument('--port', metavar='PORT',
                   help='serial port of the running pedal; triggers DFU automatically')
    p.add_argument('--addr', default=0x90600000,
                   type=lambda x: int(x, 16), metavar='ADDR',
                   help='target flash address in hex (default: 0x90600000)')
    p.add_argument('--list', metavar='FILE',
                   help='list models in an existing bank and exit')
    args = p.parse_args()

    if args.list:
        list_bank(os.path.expanduser(args.list))
        return

    if not args.models:
        p.print_help()
        sys.exit(1)

    models = []
    for spec in args.models:
        parts = spec.split(':')
        if len(parts) < 2 or len(parts) > 3:
            sys.exit(f"error: expected NAME:MODEL[:IR], got '{spec}'")
        name = parts[0].strip()
        if not name:
            sys.exit(f"error: empty name in '{spec}'")

        model_path = os.path.expanduser(parts[1])
        namb = read_model(model_path, slim_factor=args.slim)
        print(f'  [{len(models) + 1}] {name}: {model_path} ({len(namb)} bytes)')

        ir_bytes = None
        if len(parts) == 3:
            ir_path = os.path.expanduser(parts[2])
            ir_samples, sr = read_wav_mono_f32(ir_path)
            print(f'       IR: {ir_path} ({len(ir_samples)} taps @ {sr} Hz)')
            ir_samples = process_ir(ir_samples, sr)
            ir_bytes = struct.pack(f'<{len(ir_samples)}f', *ir_samples)

        models.append((name, namb, ir_bytes))

    bank = build_bank(models)
    output_path = os.path.expanduser(args.output)
    with open(output_path, 'wb') as f:
        f.write(bank)
    print(f'wrote {len(bank)} bytes → {output_path}  '
          f'({len(models)} model(s) at 0x{args.addr:08X})')

    if args.flash:
        if args.port:
            trigger_dfu(args.port)
        flash(output_path, args.addr)


if __name__ == '__main__':
    main()
