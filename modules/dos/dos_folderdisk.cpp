// FolderDisk — read-only directory-backed FAT16 hard disk for the DOS module.
//
// Presents a folder tree (on the SD card) as C: by synthesizing a full FAT16
// hard disk image on the fly: MBR (active FAT16 partition), BPB/VBR, two FAT
// tables, a fixed root directory, subdirectories as cluster chains, and file
// data streamed straight from SD. Nothing is copied; sectors are generated on
// demand in genSector(). Read-only: writes are discarded.
//
// The module can't enumerate SD directories, so the Lua launcher walks the
// folder and writes a manifest:
//     line 1            : folder VFS root (e.g. /sd/dos/mspac)
//     each further line : relpath <TAB> size <TAB> isdir   (isdir 0|1)
// Directories are listed before their children.
//
// Geometry is heads=16, sects=63, partition at LBA 63, cylinders derived from
// the sector count. Under Faux86 those numbers had to be reverse-engineered
// from the MBR's end-CHS; tiny386's BlockDevice has a get_chs() hook, so the
// disk now reports the same geometry directly and the two cannot disagree.
//
// MESHPUNK: adapted from modules/pcxt/folderdisk.cpp — the FAT16 synthesis is
// unchanged; only the interface layer at the bottom differs (tiny386
// BlockDevice instead of Faux86 DiskInterface).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Host export (elf_host.cpp): the FAT VFS's truncate. Declared rather than
// pulled in from <unistd.h> so the module links against the host's symbol.
extern "C" int truncate(const char* path, long length);

// mkdir() is a host export; declare it directly rather than pulling in
// <sys/stat.h>, which the module build has no use for otherwise.
extern "C" int mkdir(const char *path, unsigned mode);





namespace
{
    const uint32_t SECTOR       = 512;
    const uint32_t CLUSTER_SECS = 4;                    // 2 KB clusters
    const uint32_t CLUSTER      = CLUSTER_SECS * SECTOR;
    const uint32_t HEADS        = 16;
    const uint32_t SECTS        = 63;
    const uint32_t SPC          = HEADS * SECTS;        // 1008 sectors / cylinder
    const uint32_t PART_LBA     = SECTS;                // partition after track 0 (LBA 63)
    const uint32_t ROOT_ENTRIES = 512;
    const uint32_t ROOT_SECS    = ROOT_ENTRIES * 32 / SECTOR;  // 32
    const uint32_t RESERVED     = 1;                    // just the VBR
    const uint32_t FAT16_MIN    = 4200;                 // keep cluster count in FAT16 range

    struct Entry
    {
        char*    relpath;      // from root, '/'-separated (malloc'd)
        int      parentIdx;    // -1 = root
        bool     isDir;
        uint32_t size;         // file size in bytes (0 for dirs)
        uint16_t firstCluster; // 0 for empty files
        uint16_t clusterCount;
        uint8_t* dirData;      // built directory bytes (dirs only)
        // Write-through: the 8.3 name we published to the guest for this
        // entry. Guest directory writes identify files by this name, so it is
        // how a write is matched back to a real file on the SD card.
        char     n83[11];
        bool     live;         // false = deleted by the guest, slot reusable
        bool     sizeDirty;    // physical file may be longer than `size`
        bool     shrunk;       // guest reduced the size: trim by any amount
    };

    inline uint32_t ceilDiv(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

    const char* leafOf(const char* rel)
    {
        const char* s = strrchr(rel, '/');
        return s ? s + 1 : rel;
    }

    // Build a raw 8.3 dir-entry name (11 bytes, space padded) from a leaf name,
    // ensuring uniqueness against names already emitted in this directory.
    void makeName83(const char* leaf, char used[][11], int nUsed, char out[11])
    {
        char base[8], ext[3];
        int nb = 0, ne = 0;
        const char* dot = strrchr(leaf, '.');
        for (const char* p = leaf; *p && nb < 8; ++p) {
            if (dot && p == dot) break;
            char c = *p;
            if (c >= 'a' && c <= 'z') c -= 32;
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  strchr("!#$%&'()-@^_`{}~", c))) c = '_';
            base[nb++] = c;
        }
        if (dot) {
            for (const char* p = dot + 1; *p && ne < 3; ++p) {
                char c = *p;
                if (c >= 'a' && c <= 'z') c -= 32;
                if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
                ext[ne++] = c;
            }
        }
        if (nb == 0) { base[0] = '_'; nb = 1; }

        for (int attempt = 0; ; ++attempt) {
            memset(out, ' ', 11);
            int keep = nb;
            char suffix[8]; int ns = 0;
            if (attempt > 0) {
                // "~N" tail; shrink base to fit within 8 chars
                ns = snprintf(suffix, sizeof(suffix), "~%d", attempt);
                if (keep > (int)(8 - ns)) keep = 8 - ns;
            }
            for (int i = 0; i < keep; ++i) out[i] = base[i];
            for (int i = 0; i < ns; ++i)   out[keep + i] = suffix[i];
            for (int i = 0; i < ne; ++i)   out[8 + i] = ext[i];

            bool clash = false;
            for (int i = 0; i < nUsed; ++i)
                if (memcmp(used[i], out, 11) == 0) { clash = true; break; }
            if (!clash) return;
        }
    }

    void putDirEntry(uint8_t* p, const char n83[11], uint8_t attr,
                     uint16_t firstClus, uint32_t size)
    {
        memset(p, 0, 32);
        memcpy(p, n83, 11);
        p[0x0B] = attr;
        p[0x16] = 0x00; p[0x17] = 0x00;   // time
        p[0x18] = 0x21; p[0x19] = 0x58;   // date 2024-01-01
        p[0x1A] = firstClus & 0xFF; p[0x1B] = (firstClus >> 8) & 0xFF;
        p[0x1C] = size & 0xFF; p[0x1D] = (size >> 8) & 0xFF;
        p[0x1E] = (size >> 16) & 0xFF; p[0x1F] = (size >> 24) & 0xFF;
    }
}

struct FolderDiskImpl
{
    bool     valid = false;
    uint64_t pos = 0;
    uint64_t diskBytes = 0;

    uint32_t fatSectors = 0;
    uint32_t rootStartSec = 0;   // partition-relative
    uint32_t dataStartSec = 0;   // partition-relative
    uint32_t partSectors = 0;
    uint32_t totalClusters = 0;

    uint8_t  mbr[512];
    uint8_t  vbr[512];
    uint8_t* fatBuf = nullptr;   // fatSectors * 512
    uint8_t* root = nullptr;     // ROOT_SECS * 512
    uint16_t* fat = nullptr;     // alias into fatBuf

    Entry*   ent = nullptr;
    int      nent = 0;
    int      entCap = 0;      // allocated slots (grows when the guest creates files)
    char*    rootPath = nullptr;

    FILE*    cacheFile = nullptr;
    int      cacheEnt = -1;
    bool     cacheWritable = false;

    // ---- write-through state ----
    // Cluster -> owning entry (+1; 0 = unowned) and the cluster's position in
    // that entry's chain, rebuilt by reconcile() from the guest's live FAT and
    // directory contents. This is what turns a raw sector write into a
    // (file, byte offset) pair.
    uint16_t* clusOwner = nullptr;
    uint16_t* clusPos = nullptr;
    bool      dirsDirty = false;
    bool      readOnly = false;      // safety valve (-cwrite 0)

    // Data written to a cluster whose owner is not known yet. DOS allocates
    // clusters and streams data before it commits the directory entry that
    // names the file, so these arrive early and are replayed by reconcile()
    // once the owner appears.
    struct Pending { uint32_t cluster; uint32_t secInClus; uint8_t data[SECTOR]; };
    Pending* pend = nullptr;
    int      npend = 0;
    int      pendCap = 0;

    ~FolderDiskImpl()
    {
        if (cacheFile) fclose(cacheFile);
        if (fatBuf) free(fatBuf);
        if (root) free(root);
        if (ent) {
            for (int i = 0; i < nent; ++i) {
                if (ent[i].relpath) free(ent[i].relpath);
                if (ent[i].dirData) free(ent[i].dirData);
            }
            free(ent);
        }
        if (rootPath) free(rootPath);
    }

    int findByRelpath(const char* rel, int len)
    {
        for (int i = 0; i < nent; ++i)
            if ((int)strlen(ent[i].relpath) == len &&
                memcmp(ent[i].relpath, rel, len) == 0)
                return i;
        return -1;
    }

    bool build(const char* manifestPath);
    void buildDirInto(uint8_t* buf, int dirIdx /* -1 = root */);
    void genSector(uint32_t lba, uint8_t* out);
    void readFileSector(int e, uint32_t fileOffset, uint8_t* out);

    // ---- write-through ----
    void  putSector(uint32_t lba, const uint8_t* in);
    void  reconcile();
    void  rebuildOwners();
    void  syncDir(int dirIdx, uint8_t* dirBuf, uint32_t capEntries);
    void  flushPending();
    void  flushAll();
    void  stashPending(uint32_t cluster, uint32_t secInClus, const uint8_t* in);
    void  writeFileSector(int e, uint32_t fileOffset, const uint8_t* in);
    FILE* openEntry(int e, bool forWrite);
    void  entryPath(int e, char* out, size_t cap);
    int   findChild(int parentIdx, const char n83[11]);
    int   addEntry(int parentIdx, const char n83[11], bool isDir,
                   uint16_t firstCluster, uint32_t size);
    void  truncateEntry(int e, uint32_t newSize);
    uint32_t physicalSize(int e);
    void  renameEntry(int e, int dirIdx, const char n83[11]);
    void  buildRelPath(int parentIdx, const char n83[11], char* out, size_t cap);
};

// Name a file the way DOS gave it to us: an 8.3 directory field is 11 raw
// bytes ("NAME    EXT"), which becomes "NAME.EXT" on disk.
static void name83ToDisplay(const char n83[11], char* out /* >=13 */)
{
    int o = 0;
    for (int i = 0; i < 8 && n83[i] != ' '; ++i) out[o++] = n83[i];
    if (n83[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && n83[i] != ' '; ++i) out[o++] = n83[i];
    }
    out[o] = 0;
}

bool FolderDiskImpl::build(const char* manifestPath)
{
    // ---- slurp manifest ----
    FILE* mf = fopen(manifestPath, "rb");
    if (!mf) { printf("[folderdisk] manifest open fail: %s\n", manifestPath); return false; }
    fseek(mf, 0, SEEK_END);
    long msz = ftell(mf);
    fseek(mf, 0, SEEK_SET);
    if (msz <= 0) { fclose(mf); return false; }
    char* mtext = (char*)malloc(msz + 1);
    if (!mtext) { fclose(mf); return false; }
    fread(mtext, 1, msz, mf);
    mtext[msz] = 0;
    fclose(mf);

    // ---- line 1: root path ----
    char* p = mtext;
    char* nl = strpbrk(p, "\r\n");
    if (!nl) { free(mtext); return false; }
    *nl = 0;
    rootPath = (char*)malloc(strlen(p) + 1);
    strcpy(rootPath, p);
    p = nl + 1;

    // count remaining non-empty lines
    int cap = 0;
    for (char* q = p; *q; ++q) if (*q == '\n') cap++;
    cap += 2;
    // Slack above the manifest count: the drive is read-write, so the guest
    // can create files of its own and they need entry slots too.
    cap += 64;
    ent = (Entry*)calloc(cap, sizeof(Entry));
    if (!ent) { free(mtext); return false; }
    entCap = cap;

    // ---- parse entries ----
    while (*p) {
        while (*p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        char* line = p;
        char* e = strpbrk(p, "\r\n");
        if (e) { *e = 0; p = e + 1; } else { p += strlen(p); }
        // relpath \t size \t isdir
        char* t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        char* t2 = strchr(t1 + 1, '\t');
        uint32_t size = (uint32_t)strtoul(t1 + 1, nullptr, 10);
        int isdir = t2 ? atoi(t2 + 1) : 0;
        if (line[0] == 0) continue;
        Entry& en = ent[nent];
        en.relpath = (char*)malloc(strlen(line) + 1);
        strcpy(en.relpath, line);
        en.isDir = isdir != 0;
        en.size = en.isDir ? 0 : size;
        en.parentIdx = -1;
        en.live = true;
        nent++;
    }
    free(mtext);
    if (nent == 0) { /* empty folder still yields a valid blank disk */ }

    // ---- resolve parents ----
    for (int i = 0; i < nent; ++i) {
        const char* rel = ent[i].relpath;
        const char* slash = strrchr(rel, '/');
        if (slash) ent[i].parentIdx = findByRelpath(rel, (int)(slash - rel));
        else ent[i].parentIdx = -1;
    }

    // ---- cluster counts ----
    uint32_t used = 0;
    for (int i = 0; i < nent; ++i) {
        if (ent[i].isDir) {
            int kids = 0;
            for (int j = 0; j < nent; ++j) if (ent[j].parentIdx == i) kids++;
            uint32_t bytes = (uint32_t)(kids + 2) * 32;         // + "." and ".."
            ent[i].clusterCount = (uint16_t)ceilDiv(bytes, CLUSTER);
            if (ent[i].clusterCount == 0) ent[i].clusterCount = 1;
        } else {
            ent[i].clusterCount = ent[i].size ? (uint16_t)ceilDiv(ent[i].size, CLUSTER) : 0;
        }
        used += ent[i].clusterCount;
    }

    // ---- geometry ----
    // FREE_CLUSTERS is the guest's writable headroom: the drive is read-write,
    // so it needs somewhere to put save games, config files and new
    // directories. 4096 clusters = 8MB, which dwarfs what a DOS game writes
    // while costing nothing but FAT entries (2 bytes per cluster).
    const uint32_t FREE_CLUSTERS = 4096;
    // Hard FAT16 ceiling: every entry's chain is written into the FAT sized
    // from totalClusters below, so entries past the cap would scribble beyond
    // fatBuf. A folder that doesn't fit is rejected whole — isValid() goes
    // false, the module skips C:, and A: still boots.
    if (used + FREE_CLUSTERS > 60000) {
        printf("[folderdisk] %s: needs %u clusters, max 60000 (~117MB) — folder too big\n",
               rootPath, (unsigned)(used + FREE_CLUSTERS));
        return false;
    }
    totalClusters = used + FREE_CLUSTERS;
    if (totalClusters < FAT16_MIN) totalClusters = FAT16_MIN;

    fatSectors  = ceilDiv((totalClusters + 2) * 2, SECTOR);
    rootStartSec = RESERVED + 2 * fatSectors;
    dataStartSec = rootStartSec + ROOT_SECS;
    partSectors  = dataStartSec + totalClusters * CLUSTER_SECS;

    uint32_t rawSectors = PART_LBA + partSectors;
    uint32_t cyls = ceilDiv(rawSectors, SPC);
    uint32_t diskSectors = cyls * SPC;
    diskBytes = (uint64_t)diskSectors * SECTOR;

    // ---- assign clusters ----
    uint32_t next = 2;
    for (int i = 0; i < nent; ++i) {
        if (ent[i].clusterCount == 0) { ent[i].firstCluster = 0; continue; }
        ent[i].firstCluster = (uint16_t)next;
        next += ent[i].clusterCount;
    }

    // ---- allocate + build FAT ----
    fatBuf = (uint8_t*)calloc(fatSectors, SECTOR);
    if (!fatBuf) return false;
    fat = (uint16_t*)fatBuf;
    fat[0] = 0xFFF8; fat[1] = 0xFFFF;
    for (int i = 0; i < nent; ++i) {
        uint16_t c = ent[i].firstCluster, cc = ent[i].clusterCount;
        for (uint16_t k = 0; k < cc; ++k)
            fat[c + k] = (k + 1 < cc) ? (uint16_t)(c + k + 1) : 0xFFFF;
    }

    // ---- root directory ----
    root = (uint8_t*)calloc(ROOT_SECS, SECTOR);
    if (!root) return false;
    buildDirInto(root, -1);

    // ---- subdirectories ----
    for (int i = 0; i < nent; ++i) {
        if (!ent[i].isDir) continue;
        ent[i].dirData = (uint8_t*)calloc(ent[i].clusterCount, CLUSTER);
        if (!ent[i].dirData) return false;
        buildDirInto(ent[i].dirData, i);
    }

    // ---- MBR ----
    memset(mbr, 0, sizeof(mbr));
    uint32_t endCyl = cyls - 1;
    uint8_t* pe = mbr + 0x1BE;
    pe[0] = 0x80;                                   // active
    pe[1] = 1;                                      // start head
    pe[2] = 1;                                      // start sector (cyl-high 0)
    pe[3] = 0;                                      // start cyl
    pe[4] = 0x06;                                   // FAT16
    pe[5] = HEADS - 1;                              // end head
    pe[6] = (uint8_t)((SECTS & 0x3F) | (((endCyl >> 8) & 0x3) << 6));
    pe[7] = (uint8_t)(endCyl & 0xFF);
    pe[8]  = PART_LBA & 0xFF; pe[9]  = (PART_LBA >> 8) & 0xFF;
    pe[10] = (PART_LBA >> 16) & 0xFF; pe[11] = (PART_LBA >> 24) & 0xFF;
    pe[12] = partSectors & 0xFF; pe[13] = (partSectors >> 8) & 0xFF;
    pe[14] = (partSectors >> 16) & 0xFF; pe[15] = (partSectors >> 24) & 0xFF;
    mbr[0x1FE] = 0x55; mbr[0x1FF] = 0xAA;

    // ---- VBR / BPB (FAT16) ----
    memset(vbr, 0, sizeof(vbr));
    vbr[0] = 0xEB; vbr[1] = 0x3C; vbr[2] = 0x90;
    memcpy(vbr + 3, "MSDOS5.0", 8);
    vbr[0x0B] = SECTOR & 0xFF; vbr[0x0C] = (SECTOR >> 8) & 0xFF;
    vbr[0x0D] = CLUSTER_SECS;
    vbr[0x0E] = RESERVED & 0xFF; vbr[0x0F] = (RESERVED >> 8) & 0xFF;
    vbr[0x10] = 2;                                  // num FATs
    vbr[0x11] = ROOT_ENTRIES & 0xFF; vbr[0x12] = (ROOT_ENTRIES >> 8) & 0xFF;
    if (partSectors < 65536) { vbr[0x13] = partSectors & 0xFF; vbr[0x14] = (partSectors >> 8) & 0xFF; }
    vbr[0x15] = 0xF8;                               // media
    vbr[0x16] = fatSectors & 0xFF; vbr[0x17] = (fatSectors >> 8) & 0xFF;
    vbr[0x18] = SECTS & 0xFF; vbr[0x19] = (SECTS >> 8) & 0xFF;
    vbr[0x1A] = HEADS & 0xFF; vbr[0x1B] = (HEADS >> 8) & 0xFF;
    vbr[0x1C] = PART_LBA & 0xFF; vbr[0x1D] = (PART_LBA >> 8) & 0xFF;   // hidden sectors
    vbr[0x1E] = (PART_LBA >> 16) & 0xFF; vbr[0x1F] = (PART_LBA >> 24) & 0xFF;
    if (partSectors >= 65536) {
        vbr[0x20] = partSectors & 0xFF; vbr[0x21] = (partSectors >> 8) & 0xFF;
        vbr[0x22] = (partSectors >> 16) & 0xFF; vbr[0x23] = (partSectors >> 24) & 0xFF;
    }
    vbr[0x24] = 0x80;                               // drive number
    vbr[0x26] = 0x29;                               // ext boot sig
    vbr[0x27] = 0x12; vbr[0x28] = 0x34; vbr[0x29] = 0x56; vbr[0x2A] = 0x78; // volume id
    memcpy(vbr + 0x2B, "NO NAME    ", 11);
    memcpy(vbr + 0x36, "FAT16   ", 8);
    vbr[0x1FE] = 0x55; vbr[0x1FF] = 0xAA;

    // ---- write-through: cluster ownership map ----
    // Sized for every cluster on the disk so a guest write anywhere in the
    // data region can be attributed without a search. ~2 bytes/cluster each.
    clusOwner = (uint16_t*)calloc(totalClusters + 2, sizeof(uint16_t));
    clusPos   = (uint16_t*)calloc(totalClusters + 2, sizeof(uint16_t));
    if (!clusOwner || !clusPos) return false;
    rebuildOwners();

    valid = true;
    printf("[folderdisk] %s: %d entries, %u clusters, %u KB disk (%u KB free, %s)\n",
           rootPath, nent, totalClusters, (unsigned)(diskBytes / 1024),
           (unsigned)((totalClusters - used) * CLUSTER / 1024),
           readOnly ? "read-only" : "read-write");
    return true;
}

// Fill a directory buffer with entries. dirIdx == -1 builds the root (no . / ..).
void FolderDiskImpl::buildDirInto(uint8_t* buf, int dirIdx)
{
    uint32_t cap = (dirIdx < 0) ? ROOT_ENTRIES : (ent[dirIdx].clusterCount * CLUSTER / 32);
    uint32_t idx = 0;
    char (*used)[11] = (char(*)[11])calloc(cap ? cap : 1, 11);

    if (dirIdx >= 0) {
        char dot[11];  memset(dot, ' ', 11); dot[0] = '.';
        char dd[11];   memset(dd, ' ', 11);  dd[0] = '.'; dd[1] = '.';
        putDirEntry(buf + idx * 32, dot, 0x10, ent[dirIdx].firstCluster, 0); idx++;
        uint16_t parentClus = (ent[dirIdx].parentIdx < 0) ? 0
                              : ent[ent[dirIdx].parentIdx].firstCluster;
        putDirEntry(buf + idx * 32, dd, 0x10, parentClus, 0); idx++;
    }

    for (int j = 0; j < nent && idx < cap; ++j) {
        if (!ent[j].live || ent[j].parentIdx != dirIdx) continue;
        char n83[11];
        makeName83(leafOf(ent[j].relpath), used, (int)idx, n83);
        memcpy(used[idx], n83, 11);
        memcpy(ent[j].n83, n83, 11);   // remember it for write-through matching
        putDirEntry(buf + idx * 32, n83, ent[j].isDir ? 0x10 : 0x20,
                    ent[j].firstCluster, ent[j].size);
        idx++;
    }
    free(used);
}

void FolderDiskImpl::readFileSector(int e, uint32_t fileOffset, uint8_t* out)
{
    // Shares the one cached handle with the write path, so a read never sees
    // stale bytes through a second descriptor.
    FILE* f = openEntry(e, false);
    if (!f) { memset(out, 0, SECTOR); return; }
    if (fileOffset >= ent[e].size) { memset(out, 0, SECTOR); return; }
    fseek(f, (long)fileOffset, SEEK_SET);
    size_t n = fread(out, 1, SECTOR, f);
    if (n < SECTOR) memset(out + n, 0, SECTOR - n);
}

void FolderDiskImpl::genSector(uint32_t lba, uint8_t* out)
{
    if (lba == 0) { memcpy(out, mbr, SECTOR); return; }
    if (lba < PART_LBA) { memset(out, 0, SECTOR); return; }

    uint32_t s = lba - PART_LBA;                    // partition-relative
    if (s == 0) { memcpy(out, vbr, SECTOR); return; }
    if (s < RESERVED + fatSectors) {                // FAT1
        memcpy(out, fatBuf + (s - RESERVED) * SECTOR, SECTOR); return;
    }
    if (s < RESERVED + 2 * fatSectors) {            // FAT2 (mirror)
        memcpy(out, fatBuf + (s - RESERVED - fatSectors) * SECTOR, SECTOR); return;
    }
    if (s < dataStartSec) {                          // root directory
        memcpy(out, root + (s - rootStartSec) * SECTOR, SECTOR); return;
    }

    // data region. Attribution goes through the same clusOwner/clusPos map the
    // WRITE path uses, which follows the FAT chain -- it must, because the
    // guest allocates from free space and a grown file or directory is under
    // no obligation to be contiguous. (Scanning entries for a contiguous
    // [firstCluster, +clusterCount) run was right only for the layout we
    // fabricate at build time: once the MechWarrior installer filled a
    // directory and DOS chained a second cluster onto it, reads of that
    // cluster matched nothing and returned zeros, so the guest saw the entry
    // it had just written vanish -- and retried forever.)
    uint32_t rel = s - dataStartSec;
    uint32_t cluster = 2 + rel / CLUSTER_SECS;
    uint32_t secInClus = rel % CLUSTER_SECS;
    uint16_t own = (cluster < totalClusters + 2) ? clusOwner[cluster] : 0;
    if (own) {
        int i = own - 1;
        uint32_t off = (uint32_t)clusPos[cluster] * CLUSTER + secInClus * SECTOR;
        if (ent[i].isDir) {
            uint32_t capBytes = (uint32_t)ent[i].clusterCount * CLUSTER;
            if (ent[i].dirData && off + SECTOR <= capBytes) {
                memcpy(out, ent[i].dirData + off, SECTOR);
                return;
            }
        } else {
            readFileSector(i, off, out);
            return;
        }
    }
    memset(out, 0, SECTOR);                          // free/unused cluster
}


// ===========================================================================
// Write-through
//
// The guest owns a real FAT16 volume: it allocates clusters, writes directory
// entries and streams file data. Nothing about that is stored on our side --
// the volume is fabricated -- so every write has to be translated back into an
// operation on a real file in the SD folder.
//
// Three kinds of write arrive, and they are handled very differently:
//   * FAT and directory sectors land in our in-memory shadow (fatBuf, root,
//     ent[].dirData) verbatim. They are the guest's data structures; we do not
//     second-guess them, we just keep a copy so reads stay coherent and so we
//     can read the guest's intent back out.
//   * Data sectors are attributed to an owning entry via clusOwner/clusPos
//     (rebuilt from the shadow) and written straight into the SD file.
//   * Data for a cluster whose owner is not yet known is stashed. DOS streams
//     file contents before it commits the naming directory entry, so this is
//     the normal path for a new file, not an error case.
//
// After a write batch touches FAT or directory data, reconcile() diffs the
// shadow against our file table and applies creates / deletes / renames /
// truncations to the SD card, then replays anything stashed.
// ===========================================================================

void FolderDiskImpl::entryPath(int e, char* out, size_t cap)
{
    snprintf(out, cap, "%s/%s", rootPath, ent[e].relpath);
}

// One cached handle, opened read-write when possible so reads and writes to
// the same file cannot see stale data through separate descriptors.
FILE* FolderDiskImpl::openEntry(int e, bool forWrite)
{
    if (cacheEnt == e && cacheFile && (cacheWritable || !forWrite))
        return cacheFile;

    if (cacheFile) { fclose(cacheFile); cacheFile = nullptr; }
    char path[320];
    entryPath(e, path, sizeof(path));

    cacheFile = fopen(path, "r+b");          // read-write, keeps contents
    cacheWritable = (cacheFile != nullptr);
    if (!cacheFile) {
        cacheFile = fopen(path, "rb");       // read-only fallback
        cacheWritable = false;
    }
    cacheEnt = e;
    return cacheFile;
}

void FolderDiskImpl::writeFileSector(int e, uint32_t fileOffset, const uint8_t* in)
{
    // Directories live entirely in RAM: their bytes are the volume's own
    // metadata, and reconcile() is what turns them into real SD directories.
    if (ent[e].isDir) {
        uint32_t capBytes = (uint32_t)ent[e].clusterCount * CLUSTER;
        if (ent[e].dirData && fileOffset + SECTOR <= capBytes) {
            memcpy(ent[e].dirData + fileOffset, in, SECTOR);
            dirsDirty = true;
        }
        return;
    }

    FILE* f = openEntry(e, true);
    if (!f || !cacheWritable) {
        printf("[folderdisk] cannot write %s (f=%p writable=%d)\n",
               ent[e].relpath, (void*)f, (int)cacheWritable);
        return;
    }
    if (fseek(f, (long)fileOffset, SEEK_SET) != 0) return;
    fwrite(in, 1, SECTOR, f);
    fflush(f);
    // Writes are whole sectors, so this may have pushed the file past the
    // guest's logical size; flushAll() trims it at exit.
    ent[e].sizeDirty = true;
}

void FolderDiskImpl::stashPending(uint32_t cluster, uint32_t secInClus,
                                  const uint8_t* in)
{
    // Replace an existing stash for the same sector rather than growing:
    // a guest that rewrites a sector before committing the directory entry
    // should end up with the newest bytes.
    for (int i = 0; i < npend; ++i) {
        if (pend[i].cluster == cluster && pend[i].secInClus == secInClus) {
            memcpy(pend[i].data, in, SECTOR);
            return;
        }
    }
    if (npend == pendCap) {
        int ncap = pendCap ? pendCap * 2 : 64;
        // Bound the stash. A guest streaming megabytes into a file it never
        // names is either broken or doing something we cannot model; dropping
        // is better than exhausting PSRAM.
        if (ncap > 2048) {
            printf("[folderdisk] pending overflow, dropping cluster %u\n",
                   (unsigned)cluster);
            return;
        }
        Pending* np = (Pending*)realloc(pend, (size_t)ncap * sizeof(Pending));
        if (!np) return;
        pend = np;
        pendCap = ncap;
    }
    pend[npend].cluster = cluster;
    pend[npend].secInClus = secInClus;
    memcpy(pend[npend].data, in, SECTOR);
    npend++;
}

void FolderDiskImpl::flushPending()
{
    int kept = 0;
    for (int i = 0; i < npend; ++i) {
        uint32_t c = pend[i].cluster;
        uint16_t own = (c < totalClusters + 2) ? clusOwner[c] : 0;
        if (!own) {
            if (kept != i) pend[kept] = pend[i];
            kept++;                       // still unattributable, keep waiting
            continue;
        }
        int e = own - 1;
        uint32_t off = (uint32_t)clusPos[c] * CLUSTER + pend[i].secInClus * SECTOR;
        writeFileSector(e, off, pend[i].data);
    }
    if (npend != kept)
    npend = kept;
}

void FolderDiskImpl::rebuildOwners()
{
    memset(clusOwner, 0, (size_t)(totalClusters + 2) * sizeof(uint16_t));
    memset(clusPos, 0, (size_t)(totalClusters + 2) * sizeof(uint16_t));

    for (int i = 0; i < nent; ++i) {
        if (!ent[i].live) continue;
        uint32_t c = ent[i].firstCluster;
        uint32_t pos = 0;
        uint32_t guard = 0;
        while (c >= 2 && c < totalClusters + 2) {
            if (clusOwner[c]) break;              // already claimed: crosslink
            if (++guard > totalClusters) break;   // cyclic chain
            clusOwner[c] = (uint16_t)(i + 1);
            clusPos[c] = (uint16_t)pos;
            uint16_t next = fat[c];
            if (next < 2 || next >= 0xFFF0) break;
            c = next;
            pos++;
        }
        uint16_t newCount = ent[i].firstCluster ? (uint16_t)(pos + 1) : 0;
        // A directory that outgrows 64 entries gets a second cluster chained
        // on by the guest -- the RAM copy MUST grow with it, because every
        // dir-sector read/write and syncDir bounds itself by clusterCount *
        // CLUSTER. Growing the count without the buffer is a heap overflow
        // (hit by the MechWarrior installer: >64 files in one directory
        // smashed the 2KB dirData until the device watchdogged).
        if (ent[i].isDir && ent[i].dirData && newCount > ent[i].clusterCount) {
            uint8_t* nd = (uint8_t*)realloc(ent[i].dirData,
                                            (size_t)newCount * CLUSTER);
            if (nd) {
                memset(nd + (size_t)ent[i].clusterCount * CLUSTER, 0,
                       (size_t)(newCount - ent[i].clusterCount) * CLUSTER);
                ent[i].dirData = nd;
            } else {
                newCount = ent[i].clusterCount;  // OOM: keep the old bound
            }
        }
        ent[i].clusterCount = newCount;
    }
}

int FolderDiskImpl::findChild(int parentIdx, const char n83[11])
{
    for (int i = 0; i < nent; ++i)
        if (ent[i].live && ent[i].parentIdx == parentIdx &&
            memcmp(ent[i].n83, n83, 11) == 0)
            return i;
    return -1;
}

int FolderDiskImpl::addEntry(int parentIdx, const char n83[11], bool isDir,
                             uint16_t firstCluster, uint32_t size)
{
    if (nent == entCap) {
        int ncap = entCap * 2;
        Entry* ne = (Entry*)realloc(ent, (size_t)ncap * sizeof(Entry));
        if (!ne) return -1;
        memset(ne + entCap, 0, (size_t)(ncap - entCap) * sizeof(Entry));
        ent = ne;
        entCap = ncap;
    }

    char disp[16];
    name83ToDisplay(n83, disp);

    // relpath is parent's path + '/' + name, matching the manifest's form.
    char rel[512];
    if (parentIdx < 0) snprintf(rel, sizeof(rel), "%s", disp);
    else               snprintf(rel, sizeof(rel), "%s/%s", ent[parentIdx].relpath, disp);

    int e = nent++;
    memset(&ent[e], 0, sizeof(Entry));
    ent[e].relpath = (char*)malloc(strlen(rel) + 1);
    if (!ent[e].relpath) { nent--; return -1; }
    strcpy(ent[e].relpath, rel);
    ent[e].parentIdx = parentIdx;
    ent[e].isDir = isDir;
    ent[e].size = isDir ? 0 : size;
    ent[e].firstCluster = firstCluster;
    ent[e].live = true;
    memcpy(ent[e].n83, n83, 11);

    char path[320];
    entryPath(e, path, sizeof(path));
    if (isDir) {
        mkdir(path, 0777);
        // Directory contents live in RAM; the guest has already allocated the
        // cluster, so give it somewhere to land.
        uint32_t bytes = CLUSTER;
        ent[e].dirData = (uint8_t*)calloc(1, bytes);
        ent[e].clusterCount = 1;
        printf("[folderdisk] guest created dir %s\n", ent[e].relpath);
    } else {
        FILE* f = fopen(path, "wb");     // create empty; data arrives as writes
        if (f) fclose(f);
        else printf("[folderdisk] cannot create %s\n", ent[e].relpath);
        printf("[folderdisk] guest created %s (clus=%u size=%u)\n",
               ent[e].relpath, (unsigned)firstCluster, (unsigned)size);
    }
    return e;
}

void FolderDiskImpl::buildRelPath(int parentIdx, const char n83[11],
                                  char* out, size_t cap)
{
    char disp[16];
    name83ToDisplay(n83, disp);
    if (parentIdx < 0) snprintf(out, cap, "%s", disp);
    else               snprintf(out, cap, "%s/%s", ent[parentIdx].relpath, disp);
}

// Actual bytes on the SD card. There is no stat export, so measure by seeking.
uint32_t FolderDiskImpl::physicalSize(int e)
{
    FILE* f = openEntry(e, false);
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long n = ftell(f);
    return n > 0 ? (uint32_t)n : 0;
}

// The guest kept the same cluster chain but changed the name: a rename, not a
// create+delete. Renaming a directory moves its whole subtree, so every
// descendant's cached relpath has to be rewritten too.
void FolderDiskImpl::renameEntry(int e, int dirIdx, const char n83[11])
{
    char newRel[512];
    buildRelPath(dirIdx, n83, newRel, sizeof(newRel));

    char oldPath[320], newPath[336];
    entryPath(e, oldPath, sizeof(oldPath));
    snprintf(newPath, sizeof(newPath), "%s/%s", rootPath, newRel);

    if (cacheEnt == e && cacheFile) {
        fclose(cacheFile); cacheFile = nullptr; cacheEnt = -1;
    }
    if (rename(oldPath, newPath) != 0) {
        printf("[folderdisk] rename %s -> %s FAILED\n", ent[e].relpath, newRel);
        return;
    }
    printf("[folderdisk] guest renamed %s -> %s\n", ent[e].relpath, newRel);

    // Re-parent the subtree: children hold full relpaths, not back-pointers.
    size_t oldLen = strlen(ent[e].relpath);
    for (int i = 0; i < nent; ++i) {
        if (i == e || !ent[i].live || !ent[i].relpath) continue;
        if (strncmp(ent[i].relpath, ent[e].relpath, oldLen) != 0) continue;
        if (ent[i].relpath[oldLen] != '/') continue;
        char sub[512];
        snprintf(sub, sizeof(sub), "%s%s", newRel, ent[i].relpath + oldLen);
        char* np = (char*)malloc(strlen(sub) + 1);
        if (!np) continue;
        strcpy(np, sub);
        free(ent[i].relpath);
        ent[i].relpath = np;
    }

    char* np = (char*)malloc(strlen(newRel) + 1);
    if (np) { strcpy(np, newRel); free(ent[e].relpath); ent[e].relpath = np; }
    memcpy(ent[e].n83, n83, 11);
}

// Shrink a file to newSize. The host exports no truncate, so stream the bytes
// we keep into a sibling temp file and swap it in with remove()+rename().
void FolderDiskImpl::truncateEntry(int e, uint32_t newSize)
{
    char path[320], tmp[336];
    entryPath(e, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    // Close our cached handle FIRST. FatFs does not commit a file's size to
    // the directory until the handle is closed or synced, so a second handle
    // opened while ours still holds written-but-uncommitted data reads the
    // file as 0 bytes -- the copy below then produces an empty file and the
    // swap destroys the contents outright. (physicalSize() disagreed with it
    // precisely because that one reads through the cached handle.)
    if (cacheEnt == e && cacheFile) {
        fclose(cacheFile); cacheFile = nullptr; cacheEnt = -1;
    }

    // A real truncate when the host offers one: after a large install this
    // runs for every file that carries sector padding, and copying each one
    // through the stream path below is what made the exit crawl.
    if (truncate(path, (long)newSize) == 0)
        return;

    FILE* in = fopen(path, "rb");
    if (!in) return;
    FILE* out = fopen(tmp, "wb");
    if (!out) { fclose(in); return; }

    uint8_t buf[512];
    uint32_t left = newSize;
    bool ok = true;
    while (left) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : left;
        size_t got = fread(buf, 1, want, in);
        if (!got) break;                       // source shorter than newSize
        if (fwrite(buf, 1, got, out) != got) { ok = false; break; }
        left -= (uint32_t)got;
    }
    fclose(in);
    fclose(out);

    if (ok) { remove(path); rename(tmp, path); }
    else    { remove(tmp); }
}

// Diff one directory's guest-visible contents against our file table.
//
// Three-way, because a rename looks exactly like a create plus a delete if you
// only compare names: the guest edits the name field in place and keeps the
// cluster chain. Matching leftovers by first cluster recovers the rename, which
// matters doubly for directories -- we cannot remove those at all (see below),
// so treating a rename as create+delete would leave the old one behind.
void FolderDiskImpl::syncDir(int dirIdx, uint8_t* dirBuf, uint32_t capEntries)
{
    if (!dirBuf) return;

    struct GEnt { char n83[11]; bool isDir; uint16_t first; uint32_t size; bool matched; };
    GEnt* g = (GEnt*)calloc(capEntries ? capEntries : 1, sizeof(GEnt));
    if (!g) return;
    uint32_t ng = 0;

    for (uint32_t k = 0; k < capEntries; ++k) {
        uint8_t* d = dirBuf + k * 32;
        uint8_t b0 = d[0];
        if (b0 == 0x00 || b0 == 0xE5) continue;    // never used / deleted
        uint8_t attr = d[0x0B];
        if ((attr & 0x0F) == 0x0F) continue;       // long-filename fragment
        if (attr & 0x08) continue;                 // volume label
        if (b0 == '.') continue;                   // "." and ".."

        memcpy(g[ng].n83, d, 11);
        g[ng].isDir = (attr & 0x10) != 0;
        g[ng].first = (uint16_t)(d[0x1A] | (d[0x1B] << 8));
        g[ng].size = (uint32_t)d[0x1C] | ((uint32_t)d[0x1D] << 8) |
                     ((uint32_t)d[0x1E] << 16) | ((uint32_t)d[0x1F] << 24);
        g[ng].matched = false;
        ng++;
    }

    bool* seen = (bool*)calloc(nent ? nent : 1, sizeof(bool));
    if (!seen) { free(g); return; }

    // --- pass 1: same name -> same entry, just refresh it ---
    for (uint32_t k = 0; k < ng; ++k) {
        int e = findChild(dirIdx, g[k].n83);
        if (e < 0) continue;
        g[k].matched = true;
        seen[e] = true;
        if (!g[k].isDir && g[k].size != ent[e].size) {
            if (g[k].size < ent[e].size) ent[e].shrunk = true;
            ent[e].size = g[k].size;
            // Do NOT trim here: the guest publishes the size before the last
            // data sector has been replayed out of the pending stash, so the
            // file is still short at this point and the check would pass. The
            // sweep at the end of reconcile() runs after flushPending().
            ent[e].sizeDirty = true;
        }
        ent[e].firstCluster = g[k].first;
    }

    // --- pass 2: unmatched name but a known cluster chain -> rename ---
    for (uint32_t k = 0; k < ng; ++k) {
        if (g[k].matched || g[k].first == 0) continue;
        for (int i = 0; i < nent; ++i) {
            if (!ent[i].live || ent[i].parentIdx != dirIdx || seen[i]) continue;
            if (ent[i].firstCluster != g[k].first) continue;
            renameEntry(i, dirIdx, g[k].n83);
            if (!g[k].isDir) ent[i].size = g[k].size;
            g[k].matched = true;
            seen[i] = true;
            break;
        }
    }

    // --- pass 3: still unmatched -> genuinely new ---
    for (uint32_t k = 0; k < ng; ++k) {
        if (g[k].matched) continue;
        int e = addEntry(dirIdx, g[k].n83, g[k].isDir, g[k].first, g[k].size);
        if (e < 0) continue;
        bool* ns = (bool*)realloc(seen, (size_t)nent * sizeof(bool));
        if (!ns) break;
        seen = ns;
        memset(seen + nent - 1, 0, sizeof(bool));
        seen[e] = true;
    }

    // --- pass 4: ours but no longer listed -> deleted ---
    for (int i = 0; i < nent; ++i) {
        if (!ent[i].live || ent[i].parentIdx != dirIdx) continue;
        if (i < nent && seen[i]) continue;
        char path[320];
        entryPath(i, path, sizeof(path));
        if (cacheEnt == i && cacheFile) {
            fclose(cacheFile); cacheFile = nullptr; cacheEnt = -1;
        }
        if (ent[i].isDir) {
            // remove() is unlink(): it will not take a directory, and the host
            // exports no rmdir. Drop it from the guest's view so the volume
            // stays consistent, but say plainly that the SD copy remains.
            printf("[folderdisk] guest deleted dir %s — left on SD (no rmdir export)\n",
                   ent[i].relpath);
        } else if (remove(path) != 0) {
            printf("[folderdisk] delete %s FAILED\n", ent[i].relpath);
        } else {
            printf("[folderdisk] guest deleted %s\n", ent[i].relpath);
        }
        ent[i].live = false;
    }
    free(seen);
    free(g);
}

void FolderDiskImpl::reconcile()
{
    dirsDirty = false;

    syncDir(-1, root, ROOT_ENTRIES);
    // nent grows as directories are discovered, so index rather than snapshot.
    for (int i = 0; i < nent; ++i) {
        if (!ent[i].live || !ent[i].isDir || !ent[i].dirData) continue;
        syncDir(i, ent[i].dirData,
                (uint32_t)ent[i].clusterCount * CLUSTER / 32);
    }

    rebuildOwners();
    flushPending();

    // NO trimming here. The hardware trace settled the ordering: DOS writes
    // the directory entry with the new size BEFORE it writes the data for it.
    // So at every reconcile the file on disk still holds the PREVIOUS
    // contents, and a data write always follows and re-pads it -- trimming
    // here can only ever fight that write, and rewrites the whole file each
    // time it tries. Padding is cosmetic (DOS reads clamp to the directory
    // size), so it is deferred to flushAll() at module exit, when the guest
    // has stopped writing and the answer is stable.
}

// Called once when the module is shutting down: every write is done, so this
// is the only point where "physical longer than logical" unambiguously means
// leftover sector padding rather than a write still in flight.
void FolderDiskImpl::flushAll()
{
    for (int i = 0; i < nent; ++i) {
        if (!ent[i].live || ent[i].isDir) continue;
        uint32_t phys = physicalSize(i);
        if (phys <= ent[i].size) continue;
        truncateEntry(i, ent[i].size);
    }
    // Drop the cached handle so FatFs commits the last file's size.
    if (cacheFile) { fclose(cacheFile); cacheFile = nullptr; cacheEnt = -1; }
}

void FolderDiskImpl::putSector(uint32_t lba, const uint8_t* in)
{
    if (readOnly) return;
    if (lba < PART_LBA) return;                  // MBR / track 0: not ours to change

    uint32_t s = lba - PART_LBA;
    if (s == 0) { memcpy(vbr, in, SECTOR); return; }

    if (s < RESERVED + fatSectors) {             // FAT 1
        memcpy(fatBuf + (s - RESERVED) * SECTOR, in, SECTOR);
        dirsDirty = true;
        return;
    }
    if (s < RESERVED + 2 * fatSectors) {         // FAT 2 (mirror of the same data)
        memcpy(fatBuf + (s - RESERVED - fatSectors) * SECTOR, in, SECTOR);
        dirsDirty = true;
        return;
    }
    if (s < dataStartSec) {                      // root directory
        memcpy(root + (s - rootStartSec) * SECTOR, in, SECTOR);
        dirsDirty = true;
        return;
    }

    uint32_t rel = s - dataStartSec;
    uint32_t cluster = 2 + rel / CLUSTER_SECS;
    uint32_t secInClus = rel % CLUSTER_SECS;
    if (cluster >= totalClusters + 2) return;

    uint16_t own = clusOwner[cluster];
    if (!own) {
        stashPending(cluster, secInClus, in);
        return;
    }

    int e = own - 1;
    uint32_t off = (uint32_t)clusPos[cluster] * CLUSTER + secInClus * SECTOR;
    writeFileSector(e, off, in);
}

// ---------------------------------------------------------------------------
// tiny386 BlockDevice adapter
//
// genSector() already works a sector at a time, so read_async() is a loop over
// it. The IDE layer calls these synchronously and ignores the completion
// callback when we return 0, matching the file-backed backend in ide.c.
// ---------------------------------------------------------------------------

extern "C" {
#include "tiny386-src/ide.h"
}

static int64_t fd_get_sector_count(BlockDevice *bs)
{
    FolderDiskImpl *d = (FolderDiskImpl *)bs->opaque;
    return (int64_t)(d->diskBytes / SECTOR);
}

static int fd_get_chs(BlockDevice *bs, int *cylinders, int *heads, int *sectors)
{
    FolderDiskImpl *d = (FolderDiskImpl *)bs->opaque;
    uint32_t total = (uint32_t)(d->diskBytes / SECTOR);
    *heads = HEADS;
    *sectors = SECTS;
    *cylinders = (int)(total / SPC);
    return 0;
}

static int fd_read_async(BlockDevice *bs, uint64_t sector_num, uint8_t *buf,
                         int n, BlockDeviceCompletionFunc *cb, void *opaque)
{
    (void)cb; (void)opaque;
    FolderDiskImpl *d = (FolderDiskImpl *)bs->opaque;
    for (int i = 0; i < n; i++)
        d->genSector((uint32_t)(sector_num + i), buf + (size_t)i * SECTOR);
    return 0;
}

static int fd_write_async(BlockDevice *bs, uint64_t sector_num,
                          const uint8_t *buf, int n,
                          BlockDeviceCompletionFunc *cb, void *opaque)
{
    (void)cb; (void)opaque;
    FolderDiskImpl *d = (FolderDiskImpl *)bs->opaque;
    if (d->readOnly) return -1;      // report the refusal instead of pretending

    for (int i = 0; i < n; i++)
        d->putSector((uint32_t)(sector_num + i), buf + (size_t)i * SECTOR);

    // Reconcile once per command rather than per sector: DOS writes a run of
    // FAT or directory sectors together, and each reconcile walks every
    // directory. Doing it at the end of the batch keeps that off the hot path
    // while still landing before the guest can observe the result.
    if (d->dirsDirty) d->reconcile();
    return 0;
}

extern "C" BlockDevice *dos_folderdisk_open(const char *manifest_path, int read_only)
{
    FolderDiskImpl *d = new FolderDiskImpl();
    if (!d) return 0;
    d->readOnly = (read_only != 0);
    if (!d->build(manifest_path) || !d->valid) {
        printf("[folderdisk] build failed for %s\n", manifest_path);
        delete d;
        return 0;
    }
    BlockDevice *bs = (BlockDevice *)calloc(1, sizeof(BlockDevice));
    if (!bs) { delete d; return 0; }
    bs->get_sector_count = fd_get_sector_count;
    bs->get_chs          = fd_get_chs;
    bs->read_async       = fd_read_async;
    bs->write_async      = fd_write_async;
    bs->opaque           = d;
    printf("[folderdisk] C: ready, %u sectors\n",
           (unsigned)(d->diskBytes / SECTOR));
    return bs;
}

// Called from the module's exit path so the last writes are committed and any
// leftover sector padding is trimmed while the answer is still stable.
extern "C" void dos_folderdisk_flush(BlockDevice *bs)
{
    if (!bs || !bs->opaque) return;
    ((FolderDiskImpl *)bs->opaque)->flushAll();
}
