import os
import shutil
import subprocess
import sys
Import("env")

OFFSETS = {
    "bootloader": "0x0000",
    "partitions": "0x8000",
    "boot_app0": "0xe000",
    "firmware":   "0x10000",
    "littlefs":   "0xc90000",
}

def git_version(project_dir):
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=project_dir, stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return "unknown"

def merge_bin(source, target, env):
    build_dir   = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    version     = git_version(project_dir)
    output      = os.path.join(project_dir, "meshpunk-%s-merged.bin" % version)

    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")

    bins = {
        "bootloader": os.path.join(build_dir, "bootloader.bin"),
        "partitions": os.path.join(build_dir, "partitions.bin"),
        "boot_app0":  os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin"),
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
        OFFSETS["boot_app0"],  bins["boot_app0"],
        OFFSETS["firmware"],   bins["firmware"],
        OFFSETS["littlefs"],   bins["littlefs"],
    ]

    print("merge_bin: creating %s" % output)
    subprocess.check_call(cmd)

    update = os.path.join(project_dir, "meshpunk-%s-update.bin" % version)
    shutil.copy2(bins["firmware"], update)
    print("merge_bin: copied update binary to %s" % update)
    print("merge_bin: done")

env.AddPostAction("buildprog", merge_bin)
env.AlwaysBuild(env.Alias("mergebin", None, merge_bin))