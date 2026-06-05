# Building Custom Emoji for Meshpunk

This guide explains how to build your own emoji blob for the T-Deck firmware.

## Prerequisites

- [ImageMagick](https://imagemagick.org/script/download.php) (`magick` must be on your PATH)
- Bash shell (Linux/macOS terminal, or Git Bash on Windows)
- Noto Emoji PNG source files

## Getting the source PNGs

Download or clone the Noto Emoji repository:

```bash
git clone --depth 1 https://github.com/googlefonts/noto-emoji.git
```

The PNGs are in `noto-emoji/png/128/`. Filenames look like `emoji_u1f600.png`.

## Building the blob

From the meshpunk project root:

```bash
bash tools/build_emoji.sh noto-emoji/png/128 data/emojis.bin
```

This converts every single-codepoint emoji PNG into a 16x16 BGRA image and packs
them all into one `emojis.bin` blob file.

### Custom size

The default pixel size is 16. To use a different size:

```bash
bash tools/build_emoji.sh noto-emoji/png/128 data/emojis.bin 24
```

Note: the firmware's `emoji_font_create()` call must match the size you build with.

## Flashing

After building the blob, flash the filesystem to your T-Deck:

```bash
pio run -t uploadfs
```

The blob lives in `data/emojis.bin` which gets included in the LittleFS image automatically.

## What gets skipped

The build script automatically skips:

- Multi-codepoint sequences (ZWJ combos, flags) — filenames with underscores
- Variation selectors (U+FE0E, U+FE0F)
- Zero-width joiner (U+200D)
- Skin tone modifiers (U+1F3FB–U+1F3FF)
- Regional indicator symbols (U+1F1E6–U+1F1FF)

These are handled at runtime as zero-width blanks.
