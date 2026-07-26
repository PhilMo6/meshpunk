"""Minimal FAT12 reader/writer for the DOS boot floppy images in freedos/.

Enough to list the root directory, read a file out, and add or replace one.
Only what the boot-disk build needs: root directory only (no subdirectory
writes), 8.3 names, no long filenames.

Note on FAT12 packing: entries are 12 bits, so two of them share three bytes.
The byte offset is c*3//2 and it MUST be a truncating divide -- rounding 139.5
up to 140 walks the wrong chain and silently returns garbage.
"""

import struct


class Fat12:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.b = bytearray(f.read())

        b = self.b
        self.bytes_per_sec = struct.unpack_from("<H", b, 11)[0]
        self.sec_per_clus = b[13]
        self.reserved = struct.unpack_from("<H", b, 14)[0]
        self.num_fats = b[16]
        self.root_ents = struct.unpack_from("<H", b, 17)[0]
        self.total_sec = struct.unpack_from("<H", b, 19)[0]
        self.sec_per_fat = struct.unpack_from("<H", b, 22)[0]

        self.fat_start = self.reserved
        self.root_start = self.reserved + self.num_fats * self.sec_per_fat
        self.root_secs = (self.root_ents * 32) // self.bytes_per_sec
        self.data_start = self.root_start + self.root_secs
        self.clus_size = self.sec_per_clus * self.bytes_per_sec
        self.max_clus = 2 + (self.total_sec - self.data_start) // self.sec_per_clus

    # ---- FAT ----------------------------------------------------------
    def _fat_off(self, c):
        return self.fat_start * self.bytes_per_sec + (c * 3) // 2

    def fat_get(self, c):
        o = self._fat_off(c)
        if c % 2 == 0:
            return self.b[o] | ((self.b[o + 1] & 0x0F) << 8)
        return (self.b[o] >> 4) | (self.b[o + 1] << 4)

    def fat_set(self, c, v):
        v &= 0xFFF
        # Every FAT copy must agree or DOS may read the stale one.
        for n in range(self.num_fats):
            base = (self.fat_start + n * self.sec_per_fat) * self.bytes_per_sec
            o = base + (c * 3) // 2
            if c % 2 == 0:
                self.b[o] = v & 0xFF
                self.b[o + 1] = (self.b[o + 1] & 0xF0) | ((v >> 8) & 0x0F)
            else:
                self.b[o] = (self.b[o] & 0x0F) | ((v << 4) & 0xF0)
                self.b[o + 1] = (v >> 4) & 0xFF

    def free_clusters(self):
        return [c for c in range(2, self.max_clus) if self.fat_get(c) == 0]

    def chain(self, first):
        out, c, guard = [], first, 0
        while 2 <= c < 0xFF0:
            out.append(c)
            c = self.fat_get(c)
            guard += 1
            if guard > self.max_clus:
                raise RuntimeError("cyclic FAT chain")
        return out

    def free_chain(self, first):
        for c in self.chain(first):
            self.fat_set(c, 0)

    # ---- clusters -----------------------------------------------------
    def clus_off(self, c):
        return (self.data_start + (c - 2) * self.sec_per_clus) * self.bytes_per_sec

    # ---- root directory -----------------------------------------------
    def _root_off(self, i):
        return self.root_start * self.bytes_per_sec + i * 32

    def entries(self):
        out = []
        for i in range(self.root_ents):
            o = self._root_off(i)
            first = self.b[o]
            if first == 0x00:
                break
            if first == 0xE5:
                continue
            attr = self.b[o + 11]
            if attr & 0x0F == 0x0F or attr & 0x08:
                continue          # long-filename fragment / volume label
            out.append({
                "slot": i,
                "name": bytes(self.b[o:o + 11]).decode("latin-1"),
                "attr": attr,
                "clus": struct.unpack_from("<H", self.b, o + 26)[0],
                "size": struct.unpack_from("<I", self.b, o + 28)[0],
            })
        return out

    def find(self, name83):
        for e in self.entries():
            if e["name"] == name83:
                return e
        return None

    def read_file(self, name83):
        e = self.find(name83)
        if not e:
            raise FileNotFoundError(name83)
        data = bytearray()
        for c in self.chain(e["clus"]):
            o = self.clus_off(c)
            data += self.b[o:o + self.clus_size]
        return bytes(data[:e["size"]])

    def write_file(self, name83, data):
        """Add or replace a root-directory file. Returns the entry slot."""
        assert len(name83) == 11, "name must be a raw 8.3 field"
        e = self.find(name83)

        if e and e["clus"]:
            self.free_chain(e["clus"])      # release the old contents first

        need = (len(data) + self.clus_size - 1) // self.clus_size
        free = self.free_clusters()
        if len(free) < need:
            raise RuntimeError(
                "not enough free space: need %d clusters, have %d" % (need, len(free)))
        use = free[:need]

        for i, c in enumerate(use):
            o = self.clus_off(c)
            chunk = data[i * self.clus_size:(i + 1) * self.clus_size]
            self.b[o:o + len(chunk)] = chunk
            if len(chunk) < self.clus_size:      # don't leak the old tail
                self.b[o + len(chunk):o + self.clus_size] = b"\0" * (self.clus_size - len(chunk))
            self.fat_set(c, use[i + 1] if i + 1 < need else 0xFFF)

        if e:
            slot = e["slot"]
        else:
            slot = None
            for i in range(self.root_ents):
                o = self._root_off(i)
                if self.b[o] in (0x00, 0xE5):
                    slot = i
                    break
            if slot is None:
                raise RuntimeError("root directory full")

        o = self._root_off(slot)
        if not e:
            self.b[o:o + 32] = b"\0" * 32
            self.b[o:o + 11] = name83.encode("latin-1")
            self.b[o + 11] = 0x20                       # archive
            self.b[o + 22:o + 24] = struct.pack("<H", 0)      # time
            self.b[o + 24:o + 26] = struct.pack("<H", 0x5821)  # date 2024-01-01
        struct.pack_into("<H", self.b, o + 26, use[0] if need else 0)
        struct.pack_into("<I", self.b, o + 28, len(data))
        return slot

    def save(self, path):
        with open(path, "wb") as f:
            f.write(self.b)


def name83(dosname):
    """'HIMEMX.EXE' -> the raw 11-byte field 'HIMEMX  EXE'."""
    dosname = dosname.upper()
    base, _, ext = dosname.partition(".")
    if len(base) > 8 or len(ext) > 3:
        raise ValueError("not an 8.3 name: " + dosname)
    return base.ljust(8) + ext.ljust(3)
