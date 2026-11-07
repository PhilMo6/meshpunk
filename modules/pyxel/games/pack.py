# Packs a game folder into a .pyxapp (a ZIP holding the folder and a
# .pyxapp_startup_script naming the entry script), as `pyxel package` does.
# Development files are left out: bot.py, anything starting with "_", and
# __pycache__ folders.
#
#   python pack.py mesh_siege [out.pyxapp]
import os
import sys
import zipfile


def keep(name):
    return not name.startswith("_") and name != "bot.py" and not name.endswith(".pyc")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    game = sys.argv[1].rstrip("/\\")
    src = os.path.join(here, game)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, game + ".pyxapp")
    names = []
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if keep(d)]
        for f in sorted(files):
            if keep(f):
                names.append(os.path.join(root, f))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for path in names:
            rel = os.path.relpath(path, here).replace(os.sep, "/")
            z.write(path, rel)
        z.writestr(game + "/.pyxapp_startup_script", "main.py")
    print("%s: %d files, %d bytes" % (out, len(names), os.path.getsize(out)))


main()
