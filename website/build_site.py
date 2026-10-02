#!/usr/bin/env python3
"""Builds the MeshPunk website.

Copies website/site/ into the output directory and mirrors the newest release
of the stable and the dev repository into firmware/, with the index the
flasher page reads.

    build_site.py --out DIR [--live URL] [--force]

stdout carries exactly one line, "changed=true" or "changed=false". Everything
else goes to stderr. With --live, nothing is built when the fingerprint of what
would be built equals the one in URL/build.json.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SITE_SRC = os.path.join(HERE, "site")
BOARDS_JSON = os.path.join(HERE, "boards.json")
CACHE_DIR = os.path.join(HERE, ".cache")

# Channel id -> GitHub repository whose newest release it offers.
CHANNELS = (
    ("stable", "PhilMo6/meshpunk"),
    ("dev", "PhilMo6/meshpunk-dev"),
)

API = "https://api.github.com"
USER_AGENT = "meshpunk-site-build"

# ESP32 partition table: 0xC00 bytes at flash offset 0x8000, 32-byte entries
# (magic, type, subtype, offset, size, label, flags), closed by an MD5 entry.
PT_OFFSET = 0x8000
PT_SIZE = 0xC00
PT_ENTRY = struct.Struct("<HBBII16sI")
PT_MAGIC = 0x50AA
PT_MD5_MAGIC = 0xEBEB
APP_TYPE = 0x00
SUBTYPE_OTA_0 = 0x10

# src/ota_tag.h: every main firmware image carries "MESHPUNK-BOARD:<slug>\0".
BOARD_TAG = b"MESHPUNK-" + b"BOARD:"
BOARD_SLUG_MAX = 31

SAFE_NAME = re.compile(r"^[A-Za-z0-9._-]+$")
OUT_MARKER = ".build_site"
TREE_SKIP = {"_site", ".cache", "__pycache__"}


class BuildError(Exception):
    pass


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def http_request(url, api):
    headers = {"User-Agent": USER_AGENT}
    if api:
        headers["Accept"] = "application/vnd.github+json"
        headers["X-GitHub-Api-Version"] = "2022-11-28"
        token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if token:
            headers["Authorization"] = "Bearer " + token
    return urllib.request.Request(url, headers=headers)


def latest_release(repo):
    """Newest published release of repo, or None when it has none (HTTP 404)."""
    url = "%s/repos/%s/releases/latest" % (API, repo)
    try:
        with urllib.request.urlopen(http_request(url, api=True), timeout=60) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise BuildError("%s: HTTP %d %s" % (url, e.code, e.reason))


def load_boards():
    with open(BOARDS_JSON, encoding="utf-8") as f:
        boards = json.load(f)
    for slug, b in boards.items():
        ok = (
            SAFE_NAME.match(slug)
            and isinstance(b.get("name"), str)
            and isinstance(b.get("chip"), str)
            and isinstance(b.get("usb"), dict)
            and isinstance(b["usb"].get("vid"), str)
            and isinstance(b["usb"].get("pid"), str)
        )
        if not ok:
            raise BuildError("boards.json: entry '%s' needs name, chip, usb.vid, usb.pid" % slug)
    return boards


def board_assets(release, boards):
    """slug -> {"merged": asset, "firmware": asset} for every board in the release."""
    tag = release["tag_name"]
    by_name = {a["name"]: a for a in release["assets"]}
    merged_re = re.compile(r"^meshpunk-(.+)-%s-merged\.bin$" % re.escape(tag))
    found = {}
    for name in sorted(by_name):
        m = merged_re.match(name)
        if not m:
            continue
        slug = m.group(1)
        if slug not in boards:
            raise BuildError("%s: board '%s' is not in boards.json" % (name, slug))
        fw_name = "meshpunk-%s-%s-firmware.bin" % (slug, tag)
        if fw_name not in by_name:
            raise BuildError("release %s has %s but no %s" % (tag, name, fw_name))
        found[slug] = {"merged": by_name[name], "firmware": by_name[fw_name]}
    if not found:
        raise BuildError("release %s has no meshpunk-<board>-%s-merged.bin asset" % (tag, tag))
    return found


def fetch_asset(asset):
    """Path of the asset in the local cache, downloading it when absent."""
    os.makedirs(CACHE_DIR, exist_ok=True)
    path = os.path.join(CACHE_DIR, "%d-%s" % (asset["id"], asset["name"]))
    if os.path.isfile(path) and os.path.getsize(path) == asset["size"]:
        return path
    log("  downloading %s (%d bytes)" % (asset["name"], asset["size"]))
    tmp = path + ".part"
    req = http_request(asset["browser_download_url"], api=False)
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    got = os.path.getsize(tmp)
    if got != asset["size"]:
        os.remove(tmp)
        raise BuildError("%s: downloaded %d bytes, the release lists %d" % (asset["name"], got, asset["size"]))
    os.replace(tmp, path)
    return path


def prune_cache(keep):
    if not os.path.isdir(CACHE_DIR):
        return
    for name in os.listdir(CACHE_DIR):
        path = os.path.join(CACHE_DIR, name)
        if path not in keep and os.path.isfile(path):
            os.remove(path)


def parse_partition_table(blob):
    """Entries of a partition table; BuildError when the blob is not a valid one."""
    entries = []
    for pos in range(0, len(blob) - PT_ENTRY.size + 1, PT_ENTRY.size):
        magic, ptype, subtype, offset, size, label, _flags = PT_ENTRY.unpack_from(blob, pos)
        if magic == PT_MAGIC:
            entries.append({
                "label": label.rstrip(b"\x00").decode("ascii", "replace"),
                "type": ptype,
                "subtype": subtype,
                "offset": offset,
                "size": size,
            })
            continue
        if magic == PT_MD5_MAGIC:
            if hashlib.md5(blob[:pos]).digest() != blob[pos + 16:pos + 32]:
                raise BuildError("partition table MD5 does not match its entries")
            if not entries:
                raise BuildError("partition table has no entries")
            return entries
        break
    raise BuildError("no partition table with an MD5 entry at 0x%X" % PT_OFFSET)


def board_tag(image):
    """Slug of the first complete board tag in image, or None."""
    pos = 0
    while True:
        pos = image.find(BOARD_TAG, pos)
        if pos < 0:
            return None
        start = pos + len(BOARD_TAG)
        end = image.find(b"\x00", start, start + BOARD_SLUG_MAX + 1)
        if end > start:
            return image[start:end].decode("ascii", "replace")
        pos = start


def validate_board(slug, merged_name, firmware_name, merged, firmware):
    """Checks one board's pair of images; returns the layout facts the flasher needs."""
    table = merged[PT_OFFSET:PT_OFFSET + PT_SIZE]
    if len(table) != PT_SIZE:
        raise BuildError("%s: too small to hold a partition table" % merged_name)
    try:
        entries = parse_partition_table(table)
    except BuildError as e:
        raise BuildError("%s: %s" % (merged_name, e))
    ota0 = [e for e in entries if e["type"] == APP_TYPE and e["subtype"] == SUBTYPE_OTA_0]
    if len(ota0) != 1:
        raise BuildError("%s: expected exactly one ota_0 app partition, found %d" % (merged_name, len(ota0)))
    app = ota0[0]
    if len(firmware) > app["size"]:
        raise BuildError("%s: %d bytes do not fit the 0x%X-byte app partition" % (firmware_name, len(firmware), app["size"]))
    if merged[app["offset"]:app["offset"] + len(firmware)] != firmware:
        raise BuildError("%s is not the app image inside %s at 0x%X" % (firmware_name, merged_name, app["offset"]))
    tag = board_tag(firmware)
    if tag != slug:
        raise BuildError("%s carries board tag %r, its name says %r" % (firmware_name, tag, slug))
    return {
        "app_offset": app["offset"],
        "app_size": app["size"],
        "table_sha256": hashlib.sha256(table).hexdigest(),
    }


def tree_hash(root):
    """Hash of every file under root (paths and contents), skipping build outputs."""
    h = hashlib.sha256()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in TREE_SKIP)
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            with open(path, "rb") as f:
                digest = hashlib.sha256(f.read()).digest()
            h.update(rel.encode("utf-8") + b"\x00" + digest)
    return h.hexdigest()


def fingerprint(site_hash, releases):
    """Identifies one build: the site sources plus both releases and all their assets."""
    state = {"site": site_hash, "channels": []}
    for (channel_id, repo), rel in zip(CHANNELS, releases):
        entry = {"id": channel_id, "repo": repo, "release": None}
        if rel is not None:
            entry["release"] = {
                "id": rel["id"],
                "tag": rel["tag_name"],
                "name": rel.get("name"),
                "body": rel.get("body"),
                "published_at": rel.get("published_at"),
                "assets": sorted([a["id"], a["name"], a["size"], a["updated_at"]] for a in rel["assets"]),
            }
        state["channels"].append(entry)
    return hashlib.sha256(json.dumps(state, sort_keys=True).encode("utf-8")).hexdigest()


def live_fingerprint(site_url):
    """Fingerprint of the deployed site, or None when it has no build.json (HTTP 404)."""
    url = "%s/build.json?t=%d" % (site_url.rstrip("/"), int(time.time()))
    try:
        with urllib.request.urlopen(http_request(url, api=False), timeout=60) as r:
            return json.load(r).get("fingerprint")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise BuildError("%s: HTTP %d %s" % (url, e.code, e.reason))


def prepare_out(out):
    if os.path.isdir(out) and os.listdir(out):
        if not os.path.isfile(os.path.join(out, OUT_MARKER)):
            raise BuildError("%s is not empty and was not written by this script; not replacing it" % out)
        shutil.rmtree(out)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, OUT_MARKER), "w", encoding="utf-8") as f:
        f.write("written by website/build_site.py\n")


def write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(value, f, indent=1, ensure_ascii=False)
        f.write("\n")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def mirror_release(out, channel_id, release, boards, used_cache):
    tag = release["tag_name"]
    if not SAFE_NAME.match(tag):
        raise BuildError("release tag %r cannot be used as a directory name" % tag)
    assets = board_assets(release, boards)
    rel_dir = "firmware/%s/%s" % (channel_id, tag)
    os.makedirs(os.path.join(out, rel_dir), exist_ok=True)
    entries = {}
    for slug in boards:
        if slug not in assets:
            continue
        pair = assets[slug]
        paths = {kind: fetch_asset(pair[kind]) for kind in ("merged", "firmware")}
        used_cache.update(paths.values())
        with open(paths["merged"], "rb") as f:
            merged = f.read()
        with open(paths["firmware"], "rb") as f:
            firmware = f.read()
        facts = validate_board(slug, pair["merged"]["name"], pair["firmware"]["name"], merged, firmware)
        entry = dict(facts)
        for kind in ("merged", "firmware"):
            name = pair[kind]["name"]
            if not SAFE_NAME.match(name):
                raise BuildError("asset name %r cannot be used as a file name" % name)
            shutil.copyfile(paths[kind], os.path.join(out, rel_dir, name))
            entry[kind] = {
                "path": "%s/%s" % (rel_dir, name),
                "size": pair[kind]["size"],
                "sha256": sha256_file(paths[kind]),
            }
        entries[slug] = entry
        log("  %-10s ok  app @0x%X  %s  %s" % (slug, facts["app_offset"], pair["merged"]["name"], pair["firmware"]["name"]))
    return {
        "tag": tag,
        "name": release.get("name") or tag,
        "published": release.get("published_at"),
        "url": release["html_url"],
        "notes": release.get("body") or "",
        "boards": entries,
        "downloads": [
            {"name": a["name"], "size": a["size"], "url": a["browser_download_url"]}
            for a in sorted(release["assets"], key=lambda a: a["name"])
        ],
    }


def build(out, releases, boards, fp):
    prepare_out(out)
    shutil.copytree(SITE_SRC, out, dirs_exist_ok=True)
    used_cache = set()
    channels = {}
    for (channel_id, repo), release in zip(CHANNELS, releases):
        channel = {"repo": repo, "release": None}
        if release is None:
            log("%s: %s has no published release" % (channel_id, repo))
        else:
            log("%s: %s %s" % (channel_id, repo, release["tag_name"]))
            channel["release"] = mirror_release(out, channel_id, release, boards, used_cache)
        channels[channel_id] = channel
    generated = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    write_json(os.path.join(out, "firmware", "index.json"), {
        "generated": generated,
        "boards": boards,
        "channels": channels,
    })
    write_json(os.path.join(out, "build.json"), {
        "fingerprint": fp,
        "generated": generated,
        "channels": {cid: (c["release"]["tag"] if c["release"] else None) for cid, c in channels.items()},
    })
    prune_cache(used_cache)


def main():
    ap = argparse.ArgumentParser(description="Build the MeshPunk website.")
    ap.add_argument("--out", required=True, help="output directory (replaced)")
    ap.add_argument("--live", metavar="URL", help="deployed site; skip the build when its build.json matches")
    ap.add_argument("--force", action="store_true", help="build even when the deployed site matches")
    args = ap.parse_args()
    try:
        boards = load_boards()
        releases = [latest_release(repo) for _, repo in CHANNELS]
        fp = fingerprint(tree_hash(HERE), releases)
        if args.live and not args.force and live_fingerprint(args.live) == fp:
            log("deployed site already matches %s; nothing to build" % fp[:12])
            print("changed=false")
            return 0
        build(os.path.abspath(args.out), releases, boards, fp)
        log("built %s  fingerprint %s" % (args.out, fp[:12]))
        print("changed=true")
        return 0
    except BuildError as e:
        log("ERROR: %s" % e)
        return 1
    except urllib.error.URLError as e:
        log("ERROR: network: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
