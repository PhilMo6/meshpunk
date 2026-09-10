import hashlib
import os
import shutil
import struct
import subprocess
import sys
Import("env")

# Flash offsets of the direct-flash layout (merged.bin) - MUST stay in lockstep
# with meshpunk_custom_16Mb.csv.
OFFSETS = {
    "bootloader": "0x0000",
    "partitions": "0x8000",
    "otadata":    "0xE000",     # boot_app0.bin: otadata selecting main (ota_0)
    "firmware":   "0x10000",    # main (ota_0)
    "updater":    "0x590000",   # updater (factory)
    "littlefs":   "0x610000",   # assets
}

# The Launcher image has its own table (build_launcher_partition_table) and
# its own file layout; both are independent of the CSV.
LAUNCHER_FIRMWARE_OFFSET = "0x10000"
LAUNCHER_FS_OFFSET       = "0x590000"

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

# Per-board release naming. The slug matches MESHPUNK_BOARD_NAME in
# src/boards/board_pins.h, which is also what the on-device guide tells the
# user to look for. The LAUNCHER artifact is the one exception: its filename
# pattern predates the slugs and the LauncherHub catalog entry downloads it
# by explicit URL, so its name must not change shape (T-Deck only - the
# Launcher firmware does not exist for the Heltec).
BOARD_SLUG = {
    "meshpunk":               "tdeck",
    "meshpunk_release":       "tdeck",
    "meshpunk_heltec":        "heltec_v4",
    "meshpunk_heltec_release": "heltec_v4",
}
RELEASE_ENVS  = {"meshpunk_release", "meshpunk_heltec_release"}
LAUNCHER_ENVS = {"meshpunk_release"}   # bmorcelli Launcher exists for T-Deck only

# The updater firmware of each board (src/updater/main_updater.cpp), built by
# its own env and included in merged.bin at OFFSETS["updater"].
UPDATER_ENV = {
    "meshpunk":                "meshpunk_updater",
    "meshpunk_release":        "meshpunk_updater",
    "meshpunk_heltec":         "meshpunk_heltec_updater",
    "meshpunk_heltec_release": "meshpunk_heltec_updater",
}

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
# The payload is the whole littlefs image of the build, so its size follows the
# assets partition of meshpunk_custom_16Mb.csv.
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
    # These entries are the Launcher's input (it creates its own partitions
    # from them); they are not the device's table and are independent of
    # meshpunk_custom_16Mb.csv. The updater partition is not part of this
    # image: Launcher installs update through the Launcher.
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
    pioenv      = env["PIOENV"]
    slug        = BOARD_SLUG.get(pioenv, pioenv)

    releases_dir = os.path.join(project_dir, RELEASES_DIR_NAME)
    os.makedirs(releases_dir, exist_ok=True)

    output      = os.path.join(releases_dir,
                               "meshpunk-%s-%s-merged.bin" % (slug, version))

    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    updater_env   = UPDATER_ENV.get(pioenv, "meshpunk_updater")

    bins = {
        "bootloader": os.path.join(build_dir, "bootloader.bin"),
        "partitions": os.path.join(build_dir, "partitions.bin"),
        "otadata":    os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin"),
        "firmware":   os.path.join(build_dir, "firmware.bin"),
        "updater":    os.path.join(os.path.dirname(build_dir), updater_env, "firmware.bin"),
        "littlefs":   os.path.join(build_dir, "littlefs.bin"),
    }

    # The littlefs image is identical across envs (same data/ + partition csv),
    # so if this env hasn't run buildfs, reuse the same board's dev-env image
    # instead of requiring a second buildfs run.
    if not os.path.isfile(bins["littlefs"]):
        dev_env = "meshpunk_heltec" if "heltec" in pioenv else "meshpunk"
        alt = os.path.join(os.path.dirname(build_dir), dev_env, "littlefs.bin")
        if os.path.isfile(alt):
            print("merge_bin: using littlefs.bin from %s env" % dev_env)
            bins["littlefs"] = alt

    for name, path in bins.items():
        if not os.path.isfile(path):
            if name == "updater":
                print("merge_bin: missing updater firmware %s - run 'pio run -e %s' first"
                      % (path, updater_env))
            else:
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
        OFFSETS["otadata"],    bins["otadata"],
        OFFSETS["firmware"],   bins["firmware"],
        OFFSETS["updater"],    bins["updater"],
        OFFSETS["littlefs"],   bins["littlefs"],
    ]

    print("merge_bin: creating %s" % output)
    subprocess.check_call(cmd)

    # Full filesystem image (the build's littlefs, for flashing the FS alone).
    littlefs_out = os.path.join(releases_dir,
                                "meshpunk-%s-%s-littlefs.bin" % (slug, version))
    shutil.copy2(bins["littlefs"], littlefs_out)
    print("merge_bin: copied full littlefs image to %s" % littlefs_out)

    # Updater firmware alone (flash at OFFSETS["updater"] to refresh just it).
    updater_out = os.path.join(releases_dir,
                               "meshpunk-%s-%s-updater.bin" % (slug, version))
    shutil.copy2(bins["updater"], updater_out)
    print("merge_bin: copied updater firmware to %s (flash offset %s)"
          % (updater_out, OFFSETS["updater"]))

    # Distribution artifacts below require a release env: its app embeds the
    # data pack (MESHPUNK_EMBED_PACK) and is self-contained. A dev app has no
    # pack, so publishing it as firmware.bin/launcher.bin would install with an
    # empty filesystem.
    if pioenv not in RELEASE_ENVS:
        print("merge_bin: dev env - skipping firmware/launcher artifacts"
              " (use 'pio run -e meshpunk_release' or"
              " 'pio run -e meshpunk_heltec_release' for release builds)")
        print("merge_bin: done")
        return

    # Self-contained app binary. Flash at the app offset (0x10000) via any
    # flasher, or hand it to the on-device updater (Settings/Firmware, which
    # downloads exactly this file from the release page); it populates its own
    # filesystem on first boot. Not for the Launcher: it carries no partition
    # table, so the Launcher creates no data partition for it.
    firmware_out = os.path.join(releases_dir,
                                "meshpunk-%s-%s-firmware.bin" % (slug, version))
    shutil.copy2(bins["firmware"], firmware_out)
    print("merge_bin: copied firmware (app) binary to %s" % firmware_out)

    # Launcher image: T-Deck only, and its filename pattern is LOAD-BEARING -
    # the LauncherHub catalog entry downloads it by explicit URL, so the name
    # keeps its original versioned shape with no board slug.
    if pioenv not in LAUNCHER_ENVS:
        print("merge_bin: no Launcher firmware for this board -"
              " skipping launcher artifact")
        print("merge_bin: done")
        return

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
        OFFSETS["bootloader"],    bins["bootloader"],
        OFFSETS["partitions"],    launcher_table,
        LAUNCHER_FIRMWARE_OFFSET, bins["firmware"],
        LAUNCHER_FS_OFFSET,       bins["littlefs"],
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
