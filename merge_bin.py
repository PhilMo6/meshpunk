import os
import shutil
import subprocess
import sys
Import("env")

OFFSETS = {
    "bootloader": "0x0000",
    "partitions": "0x8000",
    "firmware":   "0x10000",
    "littlefs":   "0x410000",
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

# Launcher build (bmorcelli/Launcher): the full 12 MB data image won't fit
# alongside the resident Launcher on a 16 MB device, so the Launcher gets a
# smaller, purpose-built filesystem image from the same data/ folder.
LAUNCHER_FS_SIZE = 6 * 1024 * 1024          # 6 MB (multiple of block size)
# LittleFS geometry for ESP32 - must match what PlatformIO bakes the normal
# image with, or the firmware's LittleFS.begin() won't mount it.
LITTLEFS_PAGE  = 256
LITTLEFS_BLOCK = 4096

def mklittlefs_path(env):
    pkg = env.PioPlatform().get_package_dir("tool-mklittlefs")
    if not pkg:
        return None
    for name in ("mklittlefs.exe", "mklittlefs"):
        cand = os.path.join(pkg, name)
        if os.path.isfile(cand):
            return cand
    return None

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

    # App-only binary: the application image, flashed at the app offset (0x10000).
    firmware_out = os.path.join(releases_dir, "meshpunk-%s-firmware.bin" % version)
    shutil.copy2(bins["firmware"], firmware_out)
    print("merge_bin: copied firmware (app) binary to %s" % firmware_out)

    # Full filesystem image (the build's 12 MB littlefs, for flashing the FS alone).
    littlefs_out = os.path.join(releases_dir, "meshpunk-%s-littlefs.bin" % version)
    shutil.copy2(bins["littlefs"], littlefs_out)
    print("merge_bin: copied full littlefs image to %s" % littlefs_out)

    # ---- Launcher build (bmorcelli/Launcher) ----------------------------------
    # Launcher installs a MERGED image, NOT an app-only bin. updateFromSD() reads
    # the partition table at offset 0x8000, creates the app + spiffs partitions it
    # declares, and copies the app and the embedded filesystem payload into them.
    # An app-only binary has no table -> Launcher makes no filesystem ("no
    # filesystem" warning); a bare fs image gets mistaken for an app and errors.
    # The normal 12 MB image is too big to sit beside the resident Launcher, so we
    # emit a second merged image carrying a 6 MB LittleFS payload. The embedded
    # table still declares the 12 MB spiffs (label "spiffs"), which exceeds
    # Launcher's 5 MB threshold, so Launcher sizes the real partition to the free
    # space, copies our 6 MB payload, and patches the LittleFS superblock to fit.
    data_dir   = os.path.join(project_dir, "data")
    mklittlefs = mklittlefs_path(env)
    if mklittlefs and os.path.isdir(data_dir):
        launcher_fs = os.path.join(build_dir, "littlefs_launcher.bin")
        fs_cmd = [
            mklittlefs,
            "-c", data_dir,
            "-p", str(LITTLEFS_PAGE),
            "-b", str(LITTLEFS_BLOCK),
            "-s", str(LAUNCHER_FS_SIZE),
            launcher_fs,
        ]
        print("merge_bin: building %d MB Launcher fs payload"
              % (LAUNCHER_FS_SIZE // (1024 * 1024)))
        subprocess.check_call(fs_cmd)

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
            OFFSETS["partitions"], bins["partitions"],
            OFFSETS["firmware"],   bins["firmware"],
            OFFSETS["littlefs"],   launcher_fs,
        ]
        print("merge_bin: creating Launcher image %s" % launcher_img)
        subprocess.check_call(launcher_cmd)
        print("merge_bin: Launcher image done (users install THIS file via Launcher)")
    else:
        print("merge_bin: skipped Launcher image (mklittlefs or data/ not found)")

    print("merge_bin: done")

env.AddPostAction("buildprog", merge_bin)
env.AlwaysBuild(env.Alias("mergebin", None, merge_bin))