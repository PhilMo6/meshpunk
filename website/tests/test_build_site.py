"""python -m unittest discover -s website/tests

Covers build_site.py's release checks with synthetic images: the mistakes a
hand-uploaded release can contain must stop the build.
"""

import hashlib
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import build_site as bs  # noqa: E402

LAYOUT = [
    ("nvs", 1, 0x02, 0x9000, 0x5000),
    ("otadata", 1, 0x00, 0xE000, 0x2000),
    ("main", 0, 0x10, 0x10000, 0x580000),
    ("updater", 0, 0x00, 0x590000, 0x80000),
    ("assets", 1, 0x82, 0x610000, 0x9F0000),
]

BOARDS = {"tdeck": {}, "heltec_v4": {}}


def partition_table(entries, corrupt_md5=False):
    blob = b""
    for label, ptype, subtype, offset, size in entries:
        blob += bs.PT_ENTRY.pack(bs.PT_MAGIC, ptype, subtype, offset, size, label.encode(), 0)
    md5 = hashlib.md5(blob).digest()
    if corrupt_md5:
        md5 = bytes([md5[0] ^ 0xFF]) + md5[1:]
    blob += struct.pack("<H", bs.PT_MD5_MAGIC) + b"\xFF" * 14 + md5
    return blob + b"\xFF" * (bs.PT_SIZE - len(blob))


def app_image(slug="tdeck", size=4096):
    body = bytearray((i * 31) & 0xFF for i in range(size))
    body[0] = 0xE9
    if slug is not None:
        tag = bs.BOARD_TAG + slug.encode() + b"\x00"
        body[64:64 + len(tag)] = tag
    return bytes(body)


def merged_image(app, table=None, app_offset=0x10000):
    table = partition_table(LAYOUT) if table is None else table
    image = bytearray(b"\xFF" * (app_offset + len(app)))
    image[bs.PT_OFFSET:bs.PT_OFFSET + bs.PT_SIZE] = table
    image[app_offset:app_offset + len(app)] = app
    return bytes(image)


def release(tag, names):
    return {"tag_name": tag, "assets": [{"name": n} for n in names]}


class PartitionTable(unittest.TestCase):
    def test_reads_entries(self):
        entries = bs.parse_partition_table(partition_table(LAYOUT))
        self.assertEqual([(e["label"], e["offset"], e["size"]) for e in entries],
                         [(l, o, s) for l, _, _, o, s in LAYOUT])

    def test_rejects_bad_md5(self):
        with self.assertRaisesRegex(bs.BuildError, "MD5"):
            bs.parse_partition_table(partition_table(LAYOUT, corrupt_md5=True))

    def test_rejects_blank_and_empty(self):
        with self.assertRaises(bs.BuildError):
            bs.parse_partition_table(b"\xFF" * bs.PT_SIZE)
        with self.assertRaises(bs.BuildError):
            bs.parse_partition_table(partition_table([]))


class BoardTag(unittest.TestCase):
    def test_first_complete_tag(self):
        self.assertEqual(bs.board_tag(app_image("wio_l2")), "wio_l2")
        self.assertIsNone(bs.board_tag(app_image(None)))
        self.assertEqual(bs.board_tag(bs.BOARD_TAG + b"\x00xx" + bs.BOARD_TAG + b"tdeck\x00"), "tdeck")
        self.assertIsNone(bs.board_tag(bs.BOARD_TAG + b"a" * 40 + b"\x00"))
        self.assertIsNone(bs.board_tag(bs.BOARD_TAG + b"unterminated"))


class ValidateBoard(unittest.TestCase):
    def check(self, slug, merged, firmware):
        return bs.validate_board(slug, "m.bin", "f.bin", merged, firmware)

    def test_accepts_matching_pair(self):
        app = app_image("tdeck")
        facts = self.check("tdeck", merged_image(app), app)
        self.assertEqual(facts["app_offset"], 0x10000)
        self.assertEqual(facts["app_size"], 0x580000)
        self.assertEqual(facts["table_sha256"], hashlib.sha256(partition_table(LAYOUT)).hexdigest())

    def test_reads_the_app_offset_from_the_table(self):
        layout = [("main", 0, 0x10, 0x20000, 0x100000)]
        app = app_image("tdeck")
        facts = self.check("tdeck", merged_image(app, partition_table(layout), app_offset=0x20000), app)
        self.assertEqual(facts["app_offset"], 0x20000)

    def test_rejects_firmware_that_is_not_in_merged(self):
        app = app_image("tdeck")
        other = app[:200] + b"\x00" + app[201:]
        with self.assertRaisesRegex(bs.BuildError, "not the app image inside"):
            self.check("tdeck", merged_image(app), other)

    def test_rejects_another_boards_image(self):
        app = app_image("heltec_v4")
        with self.assertRaisesRegex(bs.BuildError, "board tag 'heltec_v4'"):
            self.check("tdeck", merged_image(app), app)

    def test_rejects_untagged_image(self):
        app = app_image(None)
        with self.assertRaisesRegex(bs.BuildError, "board tag None"):
            self.check("tdeck", merged_image(app), app)

    def test_rejects_table_without_single_ota0(self):
        app = app_image("tdeck")
        factory_only = partition_table([("app0", 0, 0x00, 0x10000, 0x580000)])
        with self.assertRaisesRegex(bs.BuildError, "exactly one ota_0"):
            self.check("tdeck", merged_image(app, factory_only), app)

    def test_rejects_merged_without_table(self):
        app = app_image("tdeck")
        with self.assertRaisesRegex(bs.BuildError, "partition table"):
            self.check("tdeck", merged_image(app, b"\xFF" * bs.PT_SIZE), app)

    def test_rejects_firmware_larger_than_the_partition(self):
        layout = [("main", 0, 0x10, 0x10000, 0x800)]
        app = app_image("tdeck")
        with self.assertRaisesRegex(bs.BuildError, "do not fit"):
            self.check("tdeck", merged_image(app, partition_table(layout)), app)


class BoardAssets(unittest.TestCase):
    def test_pairs_by_slug_and_tag(self):
        rel = release("v0.5.0-dev1", [
            "meshpunk-tdeck-v0.5.0-dev1-merged.bin",
            "meshpunk-tdeck-v0.5.0-dev1-firmware.bin",
            "meshpunk-tdeck-v0.5.0-dev1-launcher.bin",
            "meshpunk-heltec_v4-v0.5.0-dev1-merged.bin",
            "meshpunk-heltec_v4-v0.5.0-dev1-firmware.bin",
            "meshpunk-heltec_v4-v0.5.0-dev1-littlefs.bin",
        ])
        found = bs.board_assets(rel, BOARDS)
        self.assertEqual(sorted(found), ["heltec_v4", "tdeck"])
        self.assertEqual(found["tdeck"]["firmware"]["name"], "meshpunk-tdeck-v0.5.0-dev1-firmware.bin")

    def test_unslugged_launcher_is_not_a_board(self):
        rel = release("v0.4.3", [
            "meshpunk-tdeck-v0.4.3-merged.bin",
            "meshpunk-tdeck-v0.4.3-firmware.bin",
            "meshpunk-v0.4.3-launcher.bin",
        ])
        self.assertEqual(list(bs.board_assets(rel, BOARDS)), ["tdeck"])

    def test_unknown_board_stops_the_build(self):
        rel = release("v1.0.0", ["meshpunk-tdisplay_p4-v1.0.0-merged.bin", "meshpunk-tdisplay_p4-v1.0.0-firmware.bin"])
        with self.assertRaisesRegex(bs.BuildError, "not in boards.json"):
            bs.board_assets(rel, BOARDS)

    def test_missing_firmware_stops_the_build(self):
        rel = release("v1.0.0", ["meshpunk-tdeck-v1.0.0-merged.bin"])
        with self.assertRaisesRegex(bs.BuildError, "no meshpunk-tdeck-v1.0.0-firmware.bin"):
            bs.board_assets(rel, BOARDS)

    def test_release_without_boards_stops_the_build(self):
        with self.assertRaisesRegex(bs.BuildError, "no meshpunk-<board>"):
            bs.board_assets(release("v1.0.0", ["notes.txt"]), BOARDS)


class Fingerprint(unittest.TestCase):
    REL = {
        "id": 1, "tag_name": "v1", "name": "one", "body": "notes", "published_at": "2026-01-01T00:00:00Z",
        "assets": [{"id": 10, "name": "a.bin", "size": 5, "updated_at": "2026-01-01T00:00:00Z"}],
    }

    def test_changes_with_site_release_and_assets(self):
        base = bs.fingerprint("site", [self.REL, None])
        self.assertEqual(base, bs.fingerprint("site", [dict(self.REL), None]))
        self.assertNotEqual(base, bs.fingerprint("site2", [self.REL, None]))
        self.assertNotEqual(base, bs.fingerprint("site", [self.REL, self.REL]))
        self.assertNotEqual(base, bs.fingerprint("site", [dict(self.REL, body="edited"), None]))
        reuploaded = dict(self.REL, assets=[dict(self.REL["assets"][0], id=11)])
        self.assertNotEqual(base, bs.fingerprint("site", [reuploaded, None]))


if __name__ == "__main__":
    unittest.main()
