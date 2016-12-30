#!/usr/bin/env python3
# -*- coding: utf-8 -#-

import binascii

chars = ['A', 'å']
encodings = ['utf-8', 'latin1', 'utf-16-le', 'utf-16-be']

for ch in chars:
    for enc in encodings:
        b = ch.encode(enc)
        print(ch, 'in', enc, '-->', binascii.hexlify(b))
