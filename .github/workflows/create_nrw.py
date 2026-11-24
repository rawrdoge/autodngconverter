#!/usr/bin/env python3
import struct
import os

def write_tiff_nrw(path, image_id):
    header = struct.pack('<2sHI', b'II', 42, 8)
    num_entries = 12
    
    make = b'NIKON CORPORATION\x00'
    model = b'NIKON D70\x00'
    artist = f'Test Artist {image_id}\x00'.encode()
    desc = f'E2E Test Image {image_id}\x00'.encode()
    comment = f'Test comment {image_id}\x00'.encode()
    software = b'dnglab-test\x00'
    datetime = b'2024:01:01 00:00:00\x00'
    
    strings = [make, model, artist, desc, comment, software, datetime]
    
    pixel_data = bytearray(20000)
    for i in range(10000):
        val = (i * image_id * 7 + image_id * 13) % 65536
        pixel_data[2*i] = val & 0xFF
        pixel_data[2*i+1] = (val >> 8) & 0xFF
    
    ifd_size = 2 + num_entries * 12 + 4
    str_offset = 8 + ifd_size
    str_offsets = []
    for s in strings:
        str_offsets.append(str_offset)
        str_offset += len(s)
    
    pixel_offset = str_offset
    
    entries = []
    entries.append(struct.pack('<HHII', 256, 3, 1, 100))
    entries.append(struct.pack('<HHII', 257, 3, 1, 100))
    entries.append(struct.pack('<HHII', 258, 3, 1, 16))
    entries.append(struct.pack('<HHII', 259, 3, 1, 1))
    entries.append(struct.pack('<HHII', 262, 3, 1, 32803))
    entries.append(struct.pack('<HHII', 273, 4, 1, pixel_offset))
    entries.append(struct.pack('<HHII', 277, 3, 1, 1))
    entries.append(struct.pack('<HHII', 278, 3, 1, 100))
    entries.append(struct.pack('<HHII', 279, 4, 1, 20000))
    entries.append(struct.pack('<HHII', 271, 2, len(make), str_offsets[0]))
    entries.append(struct.pack('<HHII', 272, 2, len(model), str_offsets[1]))
    entries.append(struct.pack('<HHII', 315, 2, len(artist), str_offsets[2]))
    
    with open(path, 'wb') as f:
        f.write(header)
        f.write(struct.pack('<H', num_entries))
        for e in entries:
            f.write(e)
        f.write(struct.pack('<I', 0))
        for s in strings:
            f.write(s)
        f.write(pixel_data)

os.makedirs('testsuite', exist_ok=True)
for i in range(1, 6):
    write_tiff_nrw(f'testsuite/DSCN{7026+i:04d}.NRW', i)