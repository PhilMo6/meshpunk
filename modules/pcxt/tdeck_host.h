// T-Deck host-interface implementations for the Faux86-remake core.
// Bridges Faux86::HostSystemInterface (framebuffer/timer/audio/disk) onto the
// meshpunk firmware's host_* ELF ABI. No OS, no SDL — same model as the
// gnuboy/doom/pico8 modules.

#pragma once

#include "faux86-src/HostSystemInterface.h"
#include "faux86-src/DriveManager.h"
#include "faux86-src/Renderer.h"

#include <stdio.h>

namespace Faux86
{

// Disk image (or ROM file) backed by the host's SPI-locked stdio exports.
class FileDisk : public DiskInterface
{
public:
    explicit FileDisk(const char* path);
    virtual ~FileDisk();

    int read(uint8_t* buffer, unsigned count) override;
    int write(const uint8_t* buffer, unsigned count) override;
    uint64_t seek(uint64_t offset) override;
    uint64_t getSize() override { return m_size; }
    bool isValid() override { return m_file != nullptr; }

private:
    FILE*    m_file = nullptr;
    uint64_t m_size = 0;
    bool     m_readOnly = false;
};

// Read-only "VVFAT": presents a folder tree on the SD card as a FAT16 hard
// disk (C:), synthesizing the MBR/BPB/FAT/directories on the fly and streaming
// file bytes from SD on demand. See folderdisk.cpp. Built from a manifest the
// Lua launcher writes (the module can't enumerate SD directories itself).
class FolderDisk : public DiskInterface
{
public:
    explicit FolderDisk(const char* manifestPath);
    virtual ~FolderDisk();

    int read(uint8_t* buffer, unsigned count) override;
    int write(const uint8_t* buffer, unsigned count) override;
    uint64_t seek(uint64_t offset) override;
    uint64_t getSize() override;
    bool isValid() override;

private:
    struct Impl;
    Impl* m_impl;
};

class TDeckFrameBuffer : public FrameBufferInterface
{
public:
    void init(uint32_t desiredWidth, uint32_t desiredHeight) override;
    void resize(uint32_t desiredWidth, uint32_t desiredHeight) override;
    RenderSurface* getSurface() override { return m_surface; }
    void setPalette(Palette* palette) override {} // dead path in remake (16bpp)
    void blit(uint16_t* pixels, int w, int h, int stride) override;

private:
    void rebuildScaleMaps(int srcW, int srcH);

    RenderSurface* m_surface = nullptr;
    int m_srcW = 0, m_srcH = 0;     // last blit source size
    int m_outW = 0, m_outH = 0;     // scaled output size (<= 320x240)
};

class TDeckTimer : public TimerInterface
{
public:
    uint64_t getHostFreq() override { return 1000000; } // microseconds
    uint64_t getTicks() override;

private:
    uint32_t m_lastLow = 0;
    uint32_t m_high = 0;
};

class TDeckAudio : public AudioInterface
{
public:
    void init(VM& vm) override;
    void shutdown() override;

    // Called from the main loop: drain mixed samples and push to the I2S mixer.
    void pump();

private:
    VM* m_vm = nullptr;
};

class TDeckHostInterface : public HostSystemInterface
{
public:
    FrameBufferInterface& getFrameBuffer() override { return m_frameBuffer; }
    TimerInterface& getTimer() override { return m_timer; }
    AudioInterface& getAudio() override { return m_audio; }
    DiskInterface* openFile(const char* filename) override;

    TDeckAudio& audio() { return m_audio; }

private:
    TDeckFrameBuffer m_frameBuffer;
    TDeckTimer       m_timer;
    TDeckAudio       m_audio;
};

} // namespace Faux86
