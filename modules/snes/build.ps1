# Build snes9x (retro-go pure-C port) as a loadable ELF module
# for ESP32-S3 T-Deck. Uses the Xtensa toolchain from PlatformIO.

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CC = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
$OBJDUMP = "$toolchain\xtensa-esp32s3-elf-objdump.exe"

$S9X = "snes9x-src\src"
$OUT = "snes.app.elf"

# Common compiler flags (per-file -O flag added below)
$CFLAGS = @(
    "-shared", "-fPIC", "-fno-common",
    # Hidden visibility + -Bsymbolic below: internal globals bind directly
    # (RELATIVE relocs, no GOT double-hop). Plain -fno-PIC is rejected by
    # the xtensa BFD linker.
    "-fvisibility=hidden",
    "-mlongcalls",
    "-ffunction-sections", "-fdata-sections",
    "-fno-strict-aliasing",
    "-I$S9X",
    "-DLSB_FIRST",
    "-DRIGHTSHIFT_IS_SAR",
    "-DPIXEL_FORMAT=RGB565",
    "-include", "stdio.h",
    "-Wno-implicit-function-declaration",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-Wno-pointer-to-int-cast",
    "-Wno-int-conversion",
    "-Wno-format"
)

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-lgcc",
    "-Wl,-e,main",
    "-Wl,-Bsymbolic",
    # Keep .rel.dyn uncombined — combreloc trips an elf32-xtensa BFD assert.
    "-Wl,-z,nocombreloc",
    "-Wl,--gc-sections"
)

# 65C816 + SPC700 interpreters, PPU render and audio DSP: -O2. Rest: -Os.
# soundux.c is hot since pull audio: S9xMixSamples runs 22050 samples/s on
# the firmware sound task.
$hot = @(
    "cpuexec.c", "cpuops.c", "getset.c",
    "ppu.c", "gfx.c", "tile.c", "dma.c",
    "spc700.c", "apu_blargg.c", "dsp.c",
    "soundux.c", "fastrender.c",
    # GSU interpreter: a 603-case opcode dispatch, the one shape where -O2
    # reliably beats -Os. SuperFX carts are bound by it.
    "fxemu.c"
)

# snes9x core: every .c in the port
$core_sources = Get-ChildItem "$S9X\*.c" | ForEach-Object { $_.FullName }
# Platform glue + fast-path renderer
$glue_sources = @("main_tdeck.c", "fastrender.c")

$obj_dir = "obj"
if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

# Newest header mtime anywhere under the source tree. Headers are shared, so a
# single header edit can affect any object. We do not parse per-file #includes;
# if ANY header is newer than an object, that object is stale and recompiles.
$headers = Get-ChildItem -Path $S9X -Filter *.h -Recurse -ErrorAction SilentlyContinue
$newest_header = if ($headers) {
    ($headers | Measure-Object -Property LastWriteTime -Maximum).Maximum
} else { [DateTime]::MinValue }

$objects = @()
$failed = $false

foreach ($src in ($core_sources + ($glue_sources | ForEach-Object { (Get-Item $_).FullName }))) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = "$obj_dir/$name.o"
    $objects += $obj

    # Recompile if the source OR any header is newer than the object.
    if ((Test-Path $obj) `
        -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime) `
        -and ($newest_header -le (Get-Item $obj).LastWriteTime)) {
        continue
    }

    $srcname = [System.IO.Path]::GetFileName($src)
    $opt = if ($hot -contains $srcname) { "-O2" } else { "-Os" }
    # fastrender.c lands in .iram.text, which the ELF loader relocates into
    # internal SRAM as a unit. -mtext-section-literals emits each literal pool
    # inside that section; the default emits them to .literal, and the move
    # would change the l32r distance to them. Audited after linking.
    # fxemu.c also gets -fno-jump-tables: a switch table is emitted as data
    # inside .iram.text, where the l32r audit below reads it as code and
    # reports false escapes. Without it the switch becomes a decision tree.
    $extra = if ($srcname -eq "fastrender.c") { @("-mtext-section-literals") }
             elseif ($srcname -eq "fxemu.c") { @("-mtext-section-literals", "-fno-jump-tables") }
             else { @() }
    Write-Host "  CC $srcname ($opt)"
    & $CC $CFLAGS $opt $extra -c -o $obj $src
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED: $srcname"
        $failed = $true
    }
}

if ($failed) {
    Write-Host "Compilation failed!"
    exit 1
}

Write-Host "Linking..."
& $CC $CFLAGS $LDFLAGS -o $OUT @objects

if ($LASTEXITCODE -ne 0) {
    Write-Host "Link failed!"
    exit 1
}

$size = (Get-Item $OUT).Length
Write-Host "Success: $OUT ($([math]::Round($size/1024, 1)) KB)"

# .iram.text is copied into internal SRAM at load, which changes its distance
# to every other section. Absolute references survive (the loader relocates
# them), but l32r is PC-relative and fixed at link time, so every literal it
# reaches must live inside the section itself.
$iram = & $READELF -S -W $OUT | Select-String '\.iram\.text\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)'
if ($iram) {
    $sec_addr = [Convert]::ToUInt32($iram.Matches[0].Groups[1].Value, 16)
    $sec_size = [Convert]::ToUInt32($iram.Matches[0].Groups[2].Value, 16)
    $sec_end  = $sec_addr + $sec_size
    Write-Host ""
    Write-Host ".iram.text: $sec_size bytes of internal SRAM at load"

    $escapes = 0
    $literals = 0
    foreach ($line in (& $OBJDUMP -d --section=.iram.text $OUT)) {
        if ($line -match 'l32r\s+a\d+,\s*(?:0x)?([0-9a-fA-F]+)') {
            $literals++
            $t = [Convert]::ToUInt32($matches[1], 16)
            if ($t -lt $sec_addr -or $t -ge $sec_end) {
                if ($escapes -lt 5) { Write-Host "  ESCAPES SECTION: $($line.Trim())" }
                $escapes++
            }
        }
    }
    if ($escapes -gt 0) {
        Write-Host "  $escapes of $literals l32r targets fall outside .iram.text."
        Write-Host "  The section is not self-contained and will crash once moved."
        exit 1
    }
    Write-Host "  all $literals l32r targets resolve inside the section"
} else {
    Write-Host ""
    Write-Host "WARNING: no .iram.text section - renderer will run from PSRAM"
}

# Every UND symbol must be resolvable from host_exports[] in src/elf_host.cpp,
# or the module will fail to load on device. Audit at build time instead.
Write-Host ""
Write-Host "Undefined symbols (each must be a host export):"
& $READELF --dyn-syms $OUT | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    if ($sym -and $sym -ne "UND") { Write-Host "  $sym" }
}
