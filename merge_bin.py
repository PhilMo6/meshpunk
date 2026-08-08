import hashlib
import os
import shutil
import struct
import subprocess
import sys
Import("env")

# Flash offsets - MUST stay in lockstep with meshpunk_custom_16Mb.csv.
OFFSETS = {
    "bootloader": "0x0000",
    "partitions": "0x8000",
    "firmware":   "0x10000",
    "littlefs":   "0x590000",
}

def git_version(project_dir):
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=project_dir, stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return "unknown"

# All release artifacts land here.
RELEASES_DIR_NAME = "releases"

# Launcher build (bmorcelli/Launcher): a merged image of bootloader + table +
# the RELEASE app + the littlefs payload. The table declares the data partition
# as "assets"; the Launcher creates it and copies the payload.
#
# Data partition sizing is version-dependent (Launcher src/sd_functions.cpp,
# updateFromSD):
#   2.8.0+   an entry that carries a payload, whose label is not "spiffs", and
#            whose declared size exceeds LAUNCHER_DEFAULT_SPIFFS_SIZE (0x70000
#            on >4 MB flash) gets the EXACT declared size. An entry with no
#            payload in the file gets LAUNCHER_DEFAULT_SPIFFS_SIZE instead
#            (sdPartitionIsEmpty short-circuit), which is why the littlefs image
#            must ship inside this file.
#   <=2.7.2  the label is ignored; declared > 5 MB fills the remaining flash,
#            <= 5 MB gets a 1 MB partition (too small) - keep the csv partition
#            above 5 MB. These versions have no littlefs superblock patch, so
#            the partition/image size mismatch leaves the fs unmountable and the
#            firmware reformats it; the app's embedded pack (MESHPUNK_EMBED_PACK)
#            then repopulates it on first boot.
#
# This label matches the one in meshpunk_custom_16Mb.csv, so Launcher and direct
# flashes land on the same partition name; the firmware only falls back to
# "spiffs" on tables written before that rename. Do NOT relabel this "spiffs":
# the Launcher reserves that name -- 2.8.0 excludes it from the exact-size path
# and gates its copy behind the askSpiffs prompt, and on 2.7.2 it yielded a
# 0-size partition (block_count 0 -> divide-by-zero in lfs_alloc on first write).
# It is also the ESP32 default name, so on a multi-firmware device it can belong
# to another firmware entirely.
LAUNCHER_FS_THRESHOLD = 0x500000  # Launcher <=2.7.2 LAUNCHER_DEFAULT_SPIFFS_THRESHOLD

def build_launcher_partition_table(fs_size):
    # ESP32 partition table: 32-byte entries (magic 0x50AA, type, subtype,
    # offset, size, 16-byte label, flags), MD5 entry, 0xFF padding to 0xC00.
    # Offsets/sizes MUST stay in lockstep with meshpunk_custom_16Mb.csv.
    entries = [
        (0x01, 0x02, 0x9000,   0x5000,   b"nvs"),
        (0x00, 0x00, 0x10000,  0x580000, b"app0"),
        (0x01, 0x82, 0x590000, fs_size,  b"assets"),
    ]
    blob = b""
    for ptype, subtype, offset, size, label in entries:
        blob += struct.pack("<HBBII16sI", 0x50AA, ptype, subtype, offset, size,
                            label.ljust(16, b"\x00"), 0)
    blob += b"\xEB\xEB" + b"\xFF" * 14 + hashlib.md5(blob).digest()
    return blob + b"\xFF" * (0xC00 - len(blob))

def merge_bin(source, target, env):
    build_dir   = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    version     = git_version(project_dir)

    releases_dir = os.path.join(project_dir, RELEASES_DIR_NAME)
    os.makedirs(releases_dir, exist_ok=True)

    output      = os.path.join(releases_dir, "meshpunk-%s-merged.bin" % version)

    bins = {
        "bootloader": os.path.join(build_dir, "bootloader.bin"),
        "partitions": os.path.join(build_dir, "partitions.bin"),
        "firmware":   os.path.join(build_dir, "firmware.bin"),
        "littlefs":   os.path.join(build_dir, "littlefs.bin"),
    }

    # The littlefs image is identical across envs (same data/ + partition csv),
    # so if this env hasn't run buildfs, reuse the dev env's image instead of
    # requiring a second buildfs run.
    if not os.path.isfile(bins["littlefs"]):
        alt = os.path.join(os.path.dirname(build_dir), "meshpunk", "littlefs.bin")
        if os.path.isfile(alt):
            print("merge_bin: using littlefs.bin from dev env")
            bins["littlefs"] = alt

    for name, path in bins.items():
        if not os.path.isfile(path):
            print("merge_bin: missing %s - run 'pio run --target buildfs' first?" % name)
            return

    esptool = os.path.join(
        env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py"
    )

    cmd = [
        sys.executable, esptool,
        "--chip", "esp32s3",
        "merge_bin",
        "--target-offset", "0x0000",
        "--output", output,
        "--flash_mode", "keep",
        "--flash_freq", "keep",
        "--flash_size", "keep",
        OFFSETS["bootloader"], bins["bootloader"],
        OFFSETS["partitions"], bins["partitions"],
        OFFSETS["firmware"],   bins["firmware"],
        OFFSETS["littlefs"],   bins["littlefs"],
    ]

    print("merge_bin: creating %s" % output)
    subprocess.check_call(cmd)

    # Full filesystem image (the build's littlefs, for flashing the FS alone).
    littlefs_out = os.path.join(releases_dir, "meshpunk-%s-littlefs.bin" % version)
    shutil.copy2(bins["littlefs"], littlefs_out)
    print("merge_bin: copied full littlefs image to %s" % littlefs_out)

    # Distribution artifacts below require the release env: its app embeds the
    # data pack (MESHPUNK_EMBED_PACK) and is self-contained. A dev app has no
    # pack, so publishing it as firmware.bin/launcher.bin would install with an
    # empty filesystem.
    if env["PIOENV"] != "meshpunk_release":
        print("merge_bin: dev env - skipping firmware/launcher artifacts"
              " (use 'pio run -e meshpunk_release' for release builds)")
        print("merge_bin: done")
        return

    # Self-contained app binary: THE universal file. Flash at the app offset
    # (0x10000) via any flasher, or install through the Launcher; it populates
    # its own filesystem on first boot.
    firmware_out = os.path.join(releases_dir, "meshpunk-%s-firmware.bin" % version)
    shutil.copy2(bins["firmware"], firmware_out)
    print("merge_bin: copied firmware (app) binary to %s" % firmware_out)

    # ---- Launcher build (bmorcelli/Launcher) ----------------------------------
    # Launcher installs a MERGED image, NOT an app-only bin: it reads the
    # partition table at file offset 0x8000, creates the partitions it declares,
    # and copies the payloads that exist in the file. The declared size must
    # equal the littlefs image size -- see the note at the constants above.
    fs_size = os.path.getsize(bins["littlefs"])
    if fs_size <= LAUNCHER_FS_THRESHOLD:
        print("merge_bin: WARNING: declared fs %.1f MB is <= 5 MB; Launcher"
              " <=2.7.2 would create a 1 MB partition, too small for the pack"
              % (fs_size / (1024.0 * 1024.0)))

    launcher_table = os.path.join(build_dir, "partitions_launcher.bin")
    with open(launcher_table, "wb") as f:
        f.write(build_launcher_partition_table(fs_size))

    launcher_img = os.path.join(releases_dir, "meshpunk-%s-launcher.bin" % version)
    launcher_cmd = [
        sys.executable, esptool,
        "--chip", "esp32s3",
        "merge_bin",
        "--target-offset", "0x0000",
        "--output", launcher_img,
        "--flash_mode", "keep",
        "--flash_freq", "keep",
        "--flash_size", "keep",
        OFFSETS["bootloader"], bins["bootloader"],
        OFFSETS["partitions"], launcher_table,
        OFFSETS["firmware"],   bins["firmware"],
        OFFSETS["littlefs"],   bins["littlefs"],
    ]
    print("merge_bin: creating Launcher image %s" % launcher_img)
    subprocess.check_call(launcher_cmd)
    print("merge_bin: Launcher image done, %.1f MB incl. %.1f MB fs payload"
          " (users install THIS file via Launcher)"
          % (os.path.getsize(launcher_img) / (1024.0 * 1024.0),
             fs_size / (1024.0 * 1024.0)))

    print("merge_bin: done")

env.AddPostAction("buildprog", merge_bin)
env.AlwaysBuild(env.Alias("mergebin", None, merge_bin))