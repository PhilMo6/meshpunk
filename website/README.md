# MeshPunk website

The landing page and the web flasher published at
`https://philmo6.github.io/meshpunk/`. This folder lives in the stable repo
only.

```
website/
├─ site/                 published as-is: pages, css, js, images, vendored esptool-js
├─ boards.json           the devices the flasher offers
├─ build_site.py         builds the site and mirrors the newest stable and dev release
├─ serve.py              local preview server
└─ tests/                generator tests (Python) and flasher tests (Node)
.github/workflows/website.yml   builds and deploys to GitHub Pages
```

## How a release reaches the site

A web page cannot download a GitHub release asset (GitHub sends no CORS header
for them), so the workflow copies the files into the site:

1. `build_site.py` reads `releases/latest` of `PhilMo6/meshpunk` (channel
   **stable**) and `PhilMo6/meshpunk-dev` (channel **dev**).
2. For every board in a release it downloads `meshpunk-<slug>-<tag>-merged.bin`
   and `meshpunk-<slug>-<tag>-firmware.bin`, checks them, and writes
   `firmware/index.json`, which the flasher page reads.
3. The result is deployed to GitHub Pages. Nothing binary is committed.

The workflow runs:

- when a stable release is **published** (it starts a normal run on the default
  branch);
- **hourly**, which is how a dev release and any asset added after publishing
  are picked up — the run ends early when the deployed site is already current;
- on a push that changes `website/` or the workflow file;
- by hand: Actions → Website → **Run workflow** (tick *force* to rebuild
  regardless).

GitHub pauses scheduled workflows after 60 days without repository activity;
the Actions tab shows a button to re-enable them.

### What a release must contain

Per board, both `-merged.bin` and `-firmware.bin`, named as `merge_bin.py`
names them. The build **fails** — and the site keeps its previous state — when:

- a release has a board slug that is not in `boards.json`;
- a board has a `-merged.bin` but no `-firmware.bin`;
- the partition table in `-merged.bin` is invalid or has no single ota_0 app
  partition;
- `-firmware.bin` is not byte-for-byte the app inside `-merged.bin`;
- the image's `MESHPUNK-BOARD:<slug>` tag (`src/ota_tag.h`) differs from the
  slug in its file name.

The failed run names the file. Fix the release asset and run the workflow
again.

## Adding a board

Add an entry to `boards.json`, keyed by the slug (`MESHPUNK_BOARD_NAME`):

```json
"tdeck": {
  "name": "LilyGo T-Deck",
  "chip": "ESP32-S3",
  "usb": { "vid": "0x303A", "pid": "0x1001" }
}
```

- `chip` is the name esptool-js reports for the chip; a device reporting
  another chip is refused.
- `usb` filters the browser's port chooser.

A release that carries a new slug fails the build until this entry exists.

## What the flasher does

| | Fresh install | Update |
|---|---|---|
| Writes | `-merged.bin` at 0x0 | `-firmware.bin` at the app partition |
| Keeps user data | no | yes |
| Checks first | flash size ≥ image | the device's partition table hashes to the release's |

Both check the chip, verify the download against the SHA-256 in the index,
compare the MD5 of what was written, and restart the device. The Update check
refuses devices with another layout, such as Launcher installs.

The page logic is in `site/js/`: `flash.js` (UI), `flasher.js` (download and
device sequence), `image.js` (partition table, app image, board tag),
`md5.js`. esptool-js 0.6.1 is vendored in `site/vendor/` (see `SOURCE.txt`
there for origin and checksum).

## Local preview

```
C:\Users\noahl\.platformio\penv\Scripts\python.exe website\build_site.py --out website\_site
C:\Users\noahl\.platformio\penv\Scripts\python.exe website\serve.py
```

Then open `http://localhost:8137/` in Chrome or another compatible Chromium browser. Web Serial works on
localhost, so a device can be flashed from the preview. Downloads are cached in
`website/.cache/`.

## Tests

```
C:\Users\noahl\.platformio\penv\Scripts\python.exe -m unittest discover -s website\tests
node --test website\tests\core.test.mjs
```

The Node test also compares the page's image readers with
`website/_site/firmware/index.json` when a built site is present.

## One-time GitHub setup

1. Settings → Pages → Build and deployment → Source: **GitHub Actions**.
2. Push `website/` and `.github/workflows/website.yml` to the default branch.
3. Actions tab: the **Website** workflow is listed and enabled.

## Promotion sync

The dev → stable `robocopy /MIR` deletes whatever stable has and dev lacks.
`website\` and `.github\` exist only in stable, so the command must exclude
them by full destination path:

```
robocopy <dev> <stable> /MIR /XD .git .pio __pycache__ releases <stable>\website <stable>\.github /XF README.md
```
