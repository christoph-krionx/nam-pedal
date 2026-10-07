"""Compare C inference with independent NumPy causal-convolution evaluation."""
import ctypes
import json
import os
import struct
import sys
import zlib
from pathlib import Path
import numpy as np
from convert_a2 import select_model, validate, DILATIONS, KERNELS

root = Path(__file__).resolve().parent
model = select_model(json.loads(Path(sys.argv[1]).read_text(encoding='utf-8-sig')))
validate(model)
weights = np.asarray(model['weights'], dtype=np.float32)
data = (root / 'model.namb').read_bytes()
assert struct.unpack_from('<I', data, 24)[0] == zlib.crc32(data[:24] + data[28:])
assert np.array_equal(np.frombuffer(data, dtype='<f4', offset=32), weights)
dll_directory = os.add_dll_directory('C:/msys64/ucrt64/bin')
engine = ctypes.CDLL(str(root / 'nam_host.dll'))
ptr = ctypes.POINTER(ctypes.c_float)
engine.nam_load_weights.argtypes = [ptr, ctypes.c_int]
engine.host_process.argtypes = [ptr, ptr, ctypes.c_int]
assert engine.nam_load_weights(weights.ctypes.data_as(ptr), len(weights)) == 0
assert engine.nam_load_weights(weights.ctypes.data_as(ptr), len(weights)-1) == -1


def reference(x):
    cursor = 0
    def take(shape):
        nonlocal cursor
        count = int(np.prod(shape))
        result = weights[cursor:cursor+count].reshape(shape).astype(np.float64)
        cursor += count
        return result
    def conv(signal, w, dilation):
        result = np.zeros((w.shape[0], signal.shape[1]))
        for tap in range(w.shape[2]):
            delay = dilation * (w.shape[2]-1-tap)
            if delay == 0:
                result += w[:, :, tap] @ signal
            else:
                result[:, delay:] += w[:, :, tap] @ signal[:, :-delay]
        return result
    signal = take((3, 1)) @ x[None, :]
    head = np.zeros_like(signal)
    for kernel, dilation in zip(KERNELS, DILATIONS):
        value = conv(signal, take((3, 3, kernel)), dilation)
        value += take((3, 1))
        value += take((3, 1)) @ x[None, :]
        value = np.where(value > 0, value, value * 0.01)
        head += value
        signal = signal + take((3, 3)) @ value + take((3, 1))
    output = conv(head, take((1, 3, 16)), 1) + take((1, 1))
    output *= take((1, 1))
    assert cursor == len(weights)
    return output[0]


def run(x, block):
    engine.host_reset()
    y = np.zeros_like(x)
    for start in range(0, len(x), block):
        count = min(block, len(x)-start)
        engine.host_process(x[start:].ctypes.data_as(ptr), y[start:].ctypes.data_as(ptr), count)
    return y

rng = np.random.default_rng(42)
x = np.concatenate([np.zeros(6400), rng.normal(0, 0.05, 14000)]).astype(np.float32)
x[6400] = 0.5
expected = reference(x)
actual = run(x, 48)
assert np.all(np.isfinite(actual))
np.testing.assert_allclose(actual, expected, atol=3e-5, rtol=3e-4)
for block in (1, 17, 47):
    np.testing.assert_allclose(run(x, block), actual, atol=3e-6, rtol=3e-5)
print(f'PASS: CRC, exact weights, count rejection, NumPy reference, block sizes 1/17/47/48. Max error={np.max(np.abs(actual-expected)):.3g}')
