// FolderDisk — read-only directory-backed FAT16 hard disk for the PC-XT module.
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
// Geometry is chosen so DriveManager's (MESHPUNK-fixed) MBR parse derives it
// cleanly: heads=16, sects=63, partition at LBA 63, cyls from the end-CHS.

#include "tdeck_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

namespace Faux86
{

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

struct FolderDisk::Impl
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
    char*    rootPath = nullptr;

    FILE*    cacheFile = nullptr;
    int      cacheEnt = -1;

    ~Impl()
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
};

bool FolderDisk::Impl::build(const char* manifestPath)
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
    ent = (Entry*)calloc(cap, sizeof(Entry));
    if (!ent) { free(mtext); return false; }

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
    // Hard FAT16 ceiling: every entry's chain is written into the FAT sized
    // from totalClusters below, so entries past the cap would scribble beyond
    // fatBuf. A folder that doesn't fit is rejected whole — isValid() goes
    // false, Faux86 skips C:, and A: still boots.
    if (used + 128 > 60000) {
        printf("[folderdisk] %s: needs %u clusters, max 59872 (~117MB) — folder too big\n",
               rootPath, (unsigned)(used + 128));
        return false;
    }
    totalClusters = used + 128;
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

    valid = true;
    printf("[folderdisk] %s: %d entries, %u clusters, %u KB disk\n",
           rootPath, nent, totalClusters, (unsigned)(diskBytes / 1024));
    return true;
}

// Fill a directory buffer with entries. dirIdx == -1 builds the root (no . / ..).
void FolderDisk::Impl::buildDirInto(uint8_t* buf, int dirIdx)
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
        if (ent[j].parentIdx != dirIdx) continue;
        char n83[11];
        makeName83(leafOf(ent[j].relpath), used, (int)idx, n83);
        memcpy(used[idx], n83, 11);
        putDirEntry(buf + idx * 32, n83, ent[j].isDir ? 0x10 : 0x20,
                    ent[j].firstCluster, ent[j].size);
        idx++;
    }
    free(used);
}

void FolderDisk::Impl::readFileSector(int e, uint32_t fileOffset, uint8_t* out)
{
    if (cacheEnt != e) {
        if (cacheFile) { fclose(cacheFile); cacheFile = nullptr; }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", rootPath, ent[e].relpath);
        cacheFile = fopen(path, "rb");
        cacheEnt = e;
    }
    if (!cacheFile) { memset(out, 0, SECTOR); return; }
    if (fileOffset >= ent[e].size) { memset(out, 0, SECTOR); return; }
    fseek(cacheFile, fileOffset, SEEK_SET);
    size_t n = fread(out, 1, SECTOR, cacheFile);
    if (n < SECTOR) memset(out + n, 0, SECTOR - n);
}

void FolderDisk::Impl::genSector(uint32_t lba, uint8_t* out)
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

    // data region
    uint32_t rel = s - dataStartSec;
    uint32_t cluster = 2 + rel / CLUSTER_SECS;
    uint32_t secInClus = rel % CLUSTER_SECS;
    for (int i = 0; i < nent; ++i) {
        if (ent[i].clusterCount == 0) continue;
        uint32_t c0 = ent[i].firstCluster;
        if (cluster < c0 || cluster >= c0 + ent[i].clusterCount) continue;
        uint32_t off = (cluster - c0) * CLUSTER + secInClus * SECTOR;
        if (ent[i].isDir) memcpy(out, ent[i].dirData + off, SECTOR);
        else              readFileSector(i, off, out);
        return;
    }
    memset(out, 0, SECTOR);                          // free/unused cluster
}

// ---- DiskInterface ----

FolderDisk::FolderDisk(const char* manifestPath)
{
    m_impl = new Impl();
    m_impl->build(manifestPath);
}

FolderDisk::~FolderDisk() { delete m_impl; }

bool     FolderDisk::isValid()  { return m_impl && m_impl->valid; }
uint64_t FolderDisk::getSize()  { return m_impl ? m_impl->diskBytes : 0; }

uint64_t FolderDisk::seek(uint64_t offset) { m_impl->pos = offset; return offset; }

int FolderDisk::write(const uint8_t*, unsigned count) { return (int)count; } // discard

int FolderDisk::read(uint8_t* buffer, unsigned count)
{
    Impl* d = m_impl;
    unsigned done = 0;
    uint8_t sec[SECTOR];
    while (done < count) {
        uint32_t lba = (uint32_t)(d->pos / SECTOR);
        uint32_t within = (uint32_t)(d->pos % SECTOR);
        uint32_t chunk = SECTOR - within;
        if (chunk > count - done) chunk = count - done;
        if (within == 0 && chunk == SECTOR) {
            d->genSector(lba, buffer + done);
        } else {
            d->genSector(lba, sec);
            memcpy(buffer + done, sec + within, chunk);
        }
        done += chunk;
        d->pos += chunk;
    }
    return (int)count;
}

} // namespace Faux86
