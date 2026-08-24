# RawImport Portable v2.1.0
===========================

1. Unzip this folder anywhere.
2. Drop RAW files (NRW, NEF, CR2, ARW) into the watch/ folder.
3. DNGs appear in output/. Originals move to archive/.
4. Failed conversions go to archive/failed/.

Config: edit .env.example and rename to .env, or set environment variables.
See .env.example for all options.

Requirements
------------
- dnglab.exe in tools/ (bundled)
- exiftool.exe in tools/ (bundled)

Notes
-----
- Everything runs locally. The program never contacts the internet.
- The database is a single local file (data/rawimport.db). Back it up if you
  care about your import history.
- Status page: http://localhost:8080/health
