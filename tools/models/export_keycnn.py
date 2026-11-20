"""Exports madmom 0.16.1's key CNN (models/key/2018/key_cnn.pkl) and its log filterbank to
models/keycnn.bin for engines/keycnn.cpp. Needs a Python with madmom installed:
    python tools/models/export_keycnn.py models/keycnn.bin
Format (little endian): "RBKC", int bands, binLo, binHi, float filterbank[binHi-binLo][bands],
int nLayers, then per layer an int type: 1 pad (int width), 2 conv (int ci, co, kh, kw, float
w[ci][co][kh][kw], flipped for correlation), 3 batchnorm (int c, elu, float scale[c], shift[c]),
4 maxpool 2x2, 5 average over time and frequency.
"""
import struct
import sys

import numpy as np
from madmom.audio.filters import LogarithmicFilterbank
from madmom.audio.stft import fft_frequencies
from madmom.features.key import CNNKeyRecognitionProcessor


def main(path):
    net = CNNKeyRecognitionProcessor().processors[4].processors[0].processors[0]
    fb = np.asarray(LogarithmicFilterbank(fft_frequencies(4096, 44100), num_bands=24, fmin=65, fmax=2100,
                                          fref=440., norm_filters=True, unique_filters=True))
    lo, hi = np.nonzero(fb.any(1))[0][[0, -1]]
    out = open(path, 'wb')
    out.write(b'RBKC' + struct.pack('<3i', fb.shape[1], lo, hi + 1) + fb[lo:hi + 1].astype('<f4').tobytes())
    out.write(struct.pack('<i', len(net.layers)))
    for l in net.layers:
        name, act = type(l).__name__, getattr(getattr(l, 'activation_fn', None), '__name__', '')
        if name == 'PadLayer':
            out.write(struct.pack('<2i', 1, l.width))
        elif name == 'ConvolutionalLayer':
            assert np.all(np.asarray(l.bias) == 0) and act == 'linear'
            w = np.ascontiguousarray(np.asarray(l.weights)[:, :, ::-1, ::-1])  # madmom convolves; we correlate
            out.write(struct.pack('<5i', 2, *w.shape) + w.astype('<f4').tobytes())
        elif name == 'BatchNormLayer':
            assert act in ('elu', 'linear')
            scale = l.gamma * l.inv_std
            out.write(struct.pack('<3i', 3, len(scale), act == 'elu'))
            out.write(scale.astype('<f4').tobytes() + (l.beta - l.mean * scale).astype('<f4').tobytes())
        elif name == 'MaxPoolLayer':
            assert tuple(l.size) == (2, 2) and tuple(l.stride) == (2, 2)
            out.write(struct.pack('<i', 4))
        elif name == 'AverageLayer':
            out.write(struct.pack('<i', 5))
        else:
            raise SystemExit('unsupported layer ' + name)


if __name__ == '__main__':
    main(sys.argv[1])
