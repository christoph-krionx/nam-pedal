"""Pack the exact architecture supported by the pinned TONE3000 C engine.

This is a weights-only NAMB profile, not the general NAM binary loader format.
Flags=0xA203 identifies the fixed 3-channel architecture validated here.
"""
import argparse
import json
import math
import struct
import zlib
from pathlib import Path

DILATIONS = [1, 3, 7, 17, 41, 101, 239] * 2 + [1, 13] + [1, 3, 7, 17, 41, 101, 239]
KERNELS = [6] * 14 + [15, 15] + [6] * 7
PROFILE = 0xA203


def select_model(source):
    if source['architecture'] == 'SlimmableContainer':
        matches = [s['model'] for s in source['config']['submodels']
                   if s['model']['architecture'] == 'WaveNet'
                   and len(s['model']['weights']) == 1871]
        if len(matches) != 1:
            raise ValueError('Expected exactly one compatible 1871-weight submodel')
        return matches[0]
    return source


def validate(model):
    def require(condition, message):
        if not condition:
            raise ValueError(message)
    require(model['architecture'] == 'WaveNet', 'Expected WaveNet')
    require(model['sample_rate'] == 48000, 'Expected 48 kHz')
    config = model['config']
    require(len(config['layers']) == 1, 'Expected one layer array')
    require(config.get('head') is None and 'condition_dsp' not in config, 'Unsupported outer head/condition DSP')
    require(config.get('in_channels', 1) == 1 and config.get('out_channels', 1) == 1, 'Expected mono model')
    layer = config['layers'][0]
    for key, expected in {'input_size': 1, 'condition_size': 1, 'channels': 3,
                          'bottleneck': 3, 'kernel_sizes': KERNELS, 'dilations': DILATIONS,
                          'head': {'out_channels': 1, 'kernel_size': 16, 'bias': True},
                          'groups_input': 1, 'groups_input_mixin': 1}.items():
        require(layer.get(key) == expected, f'Unsupported {key}')
    require(layer.get('slimmable') is None, 'Unsupported dynamic width')
    require(layer['layer1x1']['active'] is True and layer['layer1x1']['groups'] == 1, 'Unsupported layer1x1')
    require(layer['head1x1']['active'] is False, 'Unsupported head1x1')
    require(layer['gating_mode'] == ['none'] * 23, 'Unsupported gating')
    require(layer['secondary_activation'] == [None] * 23, 'Unsupported secondary activation')
    require(layer['activation'] == [{'type': 'LeakyReLU', 'negative_slope': 0.01}] * 23, 'Unsupported activation')
    for key in ('conv_pre_film', 'conv_post_film', 'input_mixin_pre_film',
                'input_mixin_post_film', 'activation_pre_film', 'activation_post_film',
                'layer1x1_post_film', 'head1x1_post_film'):
        require(not layer.get(key, {}).get('active', False), f'Unsupported {key}')
    weights = model['weights']
    require(len(weights) == 1871 and all(math.isfinite(w) for w in weights), 'Invalid weights')
    require(math.isclose(weights[-1], config['head_scale'], rel_tol=1e-6), 'Inconsistent head scale')


def pack(model):
    validate(model)
    payload = struct.pack('<1871f', *model['weights'])
    header = struct.pack('<IHHIIIIII', 0x4E414D42, 1, PROFILE, 32 + len(payload), 32, 1871, 0, 0, 0)
    data = header + payload
    crc = zlib.crc32(data[:24] + data[28:])
    return data[:24] + struct.pack('<I', crc) + data[28:]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    model = select_model(json.loads(args.input.read_text(encoding='utf-8-sig')))
    data = pack(model)
    with args.output.open('xb') as output:
        output.write(data)
    print(f'{args.output}: {len(data)} bytes, 1871 weights, 48 kHz')
