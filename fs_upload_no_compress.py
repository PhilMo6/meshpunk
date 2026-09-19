# fs_upload_no_compress.py — post script: `uploadfs` writes the littlefs
# image UNCOMPRESSED (esptool --no-compress in place of the platform's -z).
#
# The compressed path (the host deflates, the ESP32-S3 flasher stub
# inflates over USB-Serial-JTAG) fails on this project's filesystem image
# with "Failed to write compressed data to flash ... (result was C900: Too
# much data)". The failure is content-dependent — adding or removing one
# app's files flips it — while the byte-identical image writes clean with
# --no-compress at the same offset. Firmware uploads keep -z: they are far
# smaller and have never failed.
#
# The espressif32 builder (builder/main.py, esptool protocol) rebuilds
# UPLOADERFLAGS for the uploadfs target with a literal "-z" token; this
# post script runs after that and swaps just that token.
Import("env")

from SCons.Script import COMMAND_LINE_TARGETS

if "uploadfs" in COMMAND_LINE_TARGETS:
    env.Replace(UPLOADERFLAGS=[
        "--no-compress" if flag == "-z" else flag
        for flag in env.get("UPLOADERFLAGS", [])
    ])
