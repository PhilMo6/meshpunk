import os
import struct
import subprocess
import zlib
Import("env")

# Packs data/ into pack/data_pack.bin for embedding into the release app image
# (board_build.embed_files). The firmware extracts it into LittleFS on first
# boot when /lua/main.lua is missing (see extract_data_pack in main.cpp), which
# makes the app binary self-contained: no littlefs payload has to survive a
# Launcher or flasher install.
#
# Pack layout (all integers little-endian):
#   header:  magic "MPK1" | u32 version=1 | u32 file_count | u32 index_size
#   index:   file_count entries:
#            u16 path_len | u16 flags (bit0 = deflate) |
#            u32 uncompressed_size | u32 stored_size | u32 data_offset |
#            path bytes (no NUL, forward slashes, leading '/')
#   data:    blobs at data_offset (relative to pack start)
# Compression is raw DEFLATE (zlib wbits=-15) to match the ESP32-S3 ROM's
# tinfl_decompress; files that don't shrink are stored raw.

PACK_DIR  = "pack"
PACK_NAME = "data_pack.bin"

def git_version(project_dir):
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=project_dir, stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return "unknown"

def build_pack(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    data_dir    = os.path.join(project_dir, "data")
    out_dir     = os.path.join(project_dir, PACK_DIR)
    out_path    = os.path.join(out_dir, PACK_NAME)

    if not os.path.isdir(data_dir):
        print("make_data_pack: no data/ directory, skipping")
        return
    os.makedirs(out_dir, exist_ok=True)

    # (path, content bytes) pairs. The synthetic /.pack_version entry (git
    # describe) is the firmware's extraction trigger AND completion marker: it
    # re-extracts whenever the installed version differs, and is written last so
    # interrupted extractions retry. Always stored raw so the firmware can
    # memcmp it without inflating.
    files = [("/.pack_version", git_version(project_dir).encode("utf-8"))]
    for root, dirs, names in os.walk(data_dir):
        dirs.sort()
        for name in sorted(names):
            full = os.path.join(root, name)
            rel = "/" + os.path.relpath(full, data_dir).replace(os.sep, "/")
            with open(full, "rb") as f:
                files.append((rel, f.read()))

    index_size = 0
    for rel, _ in files:
        index_size += 16 + len(rel.encode("utf-8"))

    header_size = 16
    data_offset = header_size + index_size

    index = b""
    blobs = b""
    raw_total = 0
    for rel, raw in files:
        raw_total += len(raw)
        if rel == "/.pack_version":
            flags, stored = 0, raw
        else:
            co = zlib.compressobj(9, zlib.DEFLATED, -15)  # raw deflate for ROM tinfl
            comp = co.compress(raw) + co.flush()
            if len(comp) < len(raw):
                flags, stored = 1, comp
            else:
                flags, stored = 0, raw
        path = rel.encode("utf-8")
        index += struct.pack("<HHIII", len(path), flags, len(raw), len(stored),
                             data_offset + len(blobs))
        index += path
        blobs += stored

    assert len(index) == index_size
    pack = b"MPK1" + struct.pack("<III", 1, len(files), index_size) + index + blobs
    with open(out_path, "wb") as f:
        f.write(pack)
    print("make_data_pack: %d files, %.2f MB raw -> %.2f MB packed (%s)"
          % (len(files), raw_total / 1048576.0, len(pack) / 1048576.0, out_path))

# Build the pack immediately at script load (pre: script) so the file exists
# before the embed_files objcopy step is configured.
build_pack(None, None, env)
