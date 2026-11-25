#!/usr/bin/env python3
"""
Generate synthetic but valid NRW (Nikon RAW) files that dnglab's NEF decoder can parse.
NEF/NRW files are TIFF-based with multiple IFDs:
- IFD 0: Main image (thumbnail/preview)
- IFD 1: Exif data + MakerNote (Nikon-specific)
"""
import struct
import os

def write_nrw_file(path, image_id):
    # --- Strings (placed after all IFDs) ---
    make = b'NIKON CORPORATION\x00'
    model = b'NIKON D70\x00'
    artist = f'Test Artist {image_id}\x00'.encode()
    desc = f'E2E Test Image {image_id}\x00'.encode()
    comment = f'Test comment {image_id}\x00'.encode()
    software = b'dnglab-test\x00'
    datetime = b'2024:01:01 00:00:00\x00'
    strings = [make, model, artist, desc, comment, software, datetime]
    
    # --- Pixel data (16-bit, 100x100 = 20000 bytes) ---
    pixel_data = bytearray(20000)
    for i in range(10000):
        val = (i * image_id * 7 + image_id * 13) % 65536
        pixel_data[2*i] = val & 0xFF
        pixel_data[2*i+1] = (val >> 8) & 0xFF
    
    # --- Exif IFD strings ---
    dt_orig = b'2024:01:01 00:00:00\x00'
    
    # --- MakerNote: proper Nikon format ---
    # Structure:
    # "Nikon\0\0\0" (8 bytes)
    # Version: 2 bytes (0x0200 big-endian)
    # Byte order: "II" (2 bytes) -- this is where MakerNote TIFF starts
    # Offset to IFD: 4 bytes (relative to byte order marker at offset 10)
    # IFD: standard TIFF IFD
    
    # MakerNote IFD entries (using Nikon tag numbers)
    maker_entries = []
    maker_entries.append((0x0001, 7, 4, struct.unpack('<I', b'0230')[0]))  # Version - "0230"
    maker_entries.append((0x0002, 3, 1, 200))    # ISO - SHORT
    maker_entries.append((0x0003, 3, 1, 1))      # ColorMode - SHORT
    maker_entries.append((0x0004, 3, 1, 2))      # Quality - SHORT
    maker_entries.append((0x0005, 3, 1, 0))      # WhiteBalance - SHORT
    maker_entries.append((0x0006, 3, 1, 2))      # Sharpening - SHORT
    maker_entries.append((0x0007, 3, 1, 1))      # FocusMode - SHORT
    
    maker_num_entries = len(maker_entries)
    # MakerNote header: 8 + 2 + 2 = 12 bytes before IFD offset
    # IFD offset (4 bytes) at bytes 10-13 of MakerNote
    # IFD starts at byte 16 of MakerNote (right after offset field), so offset = 6 from byte order marker
    
    maker_header = b'Nikon\x00\x00\x00' + struct.pack('>H', 0x0200) + b'II'
    maker_ifd_offset = 6  # relative to byte order marker (byte 10), IFD count at byte 16
    
    # Build MakerNote IFD entries
    maker_entry_data = b''
    for tag, typ, count, val in maker_entries:
        maker_entry_data += struct.pack('<HHII', tag, typ, count, val)
    
    maker_note_data = maker_header + struct.pack('<I', maker_ifd_offset)
    maker_note_data += struct.pack('<H', maker_num_entries)
    maker_note_data += maker_entry_data
    maker_note_data += struct.pack('<I', 0)  # next IFD = 0
    
    # --- IFD 0: Main image ---
    ifd0_entries = []
    ifd0_entries.append((256, 3, 1, 100))          # ImageWidth
    ifd0_entries.append((257, 3, 1, 100))          # ImageHeight
    ifd0_entries.append((258, 3, 1, 16))           # BitsPerSample
    ifd0_entries.append((259, 3, 1, 1))            # Compression = 1
    ifd0_entries.append((262, 3, 1, 32803))        # PhotometricInterpretation = CFA
    ifd0_entries.append((273, 4, 1, 0))            # StripOffsets (placeholder)
    ifd0_entries.append((277, 3, 1, 1))            # SamplesPerPixel
    ifd0_entries.append((278, 3, 1, 100))          # RowsPerStrip
    ifd0_entries.append((279, 4, 1, 20000))        # StripByteCounts
    ifd0_entries.append((271, 2, len(make), 0))    # Make (placeholder)
    ifd0_entries.append((272, 2, len(model), 0))   # Model (placeholder)
    ifd0_entries.append((34665, 4, 1, 0))          # ExifIFDPointer (placeholder)
    
    # --- IFD 1: Exif IFD ---
    ifd1_entries = []
    ifd1_entries.append((36864, 7, 4, struct.unpack('<I', b'0231')[0])) # ExifVersion inline
    ifd1_entries.append((36867, 2, len(dt_orig), 0)) # DateTimeOriginal (placeholder)
    ifd1_entries.append((37121, 7, 4, struct.unpack('<I', b'\x01\x02\x03\x00')[0])) # ComponentsConfiguration
    ifd1_entries.append((37122, 5, 1, 0))          # CompressedBitsPerPixel (placeholder)
    ifd1_entries.append((40961, 3, 1, 1))          # ColorSpace = 1
    ifd1_entries.append((40962, 3, 1, 100))        # ExifImageWidth
    ifd1_entries.append((40963, 3, 1, 100))        # ExifImageHeight
    ifd1_entries.append((37500, 7, len(maker_note_data), 0)) # MakerNote (placeholder)
    
    # --- Calculate offsets ---
    header = struct.pack('<2sHI', b'II', 42, 8)
    
    ifd0_offset = 8
    ifd0_num_entries = len(ifd0_entries)
    ifd0_size = 2 + ifd0_num_entries * 12 + 4
    ifd1_offset = ifd0_offset + ifd0_size
    ifd1_num_entries = len(ifd1_entries)
    ifd1_size = 2 + ifd1_num_entries * 12 + 4
    next_ifd_offset = ifd1_offset + ifd1_size
    
    strings_offset = next_ifd_offset
    str_offsets = []
    for s in strings:
        str_offsets.append(strings_offset)
        strings_offset += len(s)
    
    exif_strings_offset = strings_offset
    dt_orig_offset = exif_strings_offset
    exif_strings_offset += len(dt_orig)
    
    # CompressedBitsPerPixel rational (4/1)
    comp_bits_offset = exif_strings_offset
    exif_strings_offset += 8
    
    # MakerNote data
    makernote_offset = exif_strings_offset
    exif_strings_offset += len(maker_note_data)
    
    pixel_offset = exif_strings_offset
    
    # Build IFD0 entries with actual offsets
    ifd0_entry_data = b''
    for tag, typ, count, val in ifd0_entries:
        if tag == 273:  # StripOffsets
            val = pixel_offset
        elif tag == 271:  # Make
            val = str_offsets[0]
        elif tag == 272:  # Model
            val = str_offsets[1]
        elif tag == 34665:  # ExifIFDPointer
            val = ifd1_offset
        ifd0_entry_data += struct.pack('<HHII', tag, typ, count, val)
    
    # Build IFD1 entries with actual offsets
    ifd1_entry_data = b''
    for tag, typ, count, val in ifd1_entries:
        if tag == 36867:  # DateTimeOriginal
            val = dt_orig_offset
        elif tag == 37122:  # CompressedBitsPerPixel
            val = comp_bits_offset
        elif tag == 37500:  # MakerNote
            val = makernote_offset
        ifd1_entry_data += struct.pack('<HHII', tag, typ, count, val)
    
    # Write file
    with open(path, 'wb') as f:
        # TIFF header
        f.write(header)
        # IFD0
        f.write(struct.pack('<H', ifd0_num_entries))
        f.write(ifd0_entry_data)
        f.write(struct.pack('<I', ifd1_offset))
        # IFD1
        f.write(struct.pack('<H', ifd1_num_entries))
        f.write(ifd1_entry_data)
        f.write(struct.pack('<I', 0))
        # Strings (IFD0)
        for s in strings:
            f.write(s)
        # Exif IFD strings
        f.write(dt_orig)
        f.write(struct.pack('<II', 4, 1))  # CompressedBitsPerPixel rational
        # MakerNote
        f.write(maker_note_data)
        # Pixel data
        f.write(pixel_data)

os.makedirs('testsuite', exist_ok=True)
for i in range(1, 21):
    write_nrw_file(f'testsuite/DSCN{7026+i:04d}.NRW', i)
    print(f"Created testsuite/DSCN{7026+i:04d}.NRW")
