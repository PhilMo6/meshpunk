"""PlatformIO build middleware: exclude disabled LVGL vendor draw backends.

The Renesas Dave2D, NXP PXP/VGLite, and SDL draw backends are disabled in
lv_conf.h (LV_USE_DRAW_DAVE2D=0, etc.) but their .c files still get picked
up by PlatformIO's library dependency finder.  On espressif32@6.11.0 the
compiler produces no .o for empty translation units (#if 0 … #endif),
causing the archiver to fail with "No such file or directory".

This script filters those source files out before compilation so the
archiver never references them.  No library files are modified.
"""
Import("env")


def filter_lvgl_vendor_draws(env, node):
    path = node.get_path().replace("\\", "/")
    for d in ("draw/renesas/", "draw/nxp/", "draw/sdl/"):
        if d in path:
            return None
    return node


env.AddBuildMiddleware(filter_lvgl_vendor_draws)
