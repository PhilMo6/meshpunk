# Build SMS Plus (Sega Game Gear / Master System / SG-1000) as a loadable
# ELF module for ESP32-S3 T-Deck. Uses the Xtensa toolchain from PlatformIO.

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CC = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"

$SRC = "smsplus-src"
$OUT = "sega8.app.elf"

# NDEBUG: the core calls assert() on its own allocations; without it every
# translation unit pulls __assert_func, which is not a host export.
$CFLAGS = @(
    "-shared", "-fPIC", "-fno-common",
    "-mlongcalls",
    "-ffunction-sections", "-fdata-sections",
    "-fno-strict-aliasing",
    "-DNDEBUG",
    "-I$SRC"
)

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-lgcc",
    "-Wl,-e,main",
    "-Wl,--gc-sections"
)

# Emulator core: -O2 (per-scanline VDP + per-sample PSG hot paths)
$core_sources = @(
    "cpu\z80.c",
    "sms.c", "system.c", "memz80.c", "pio.c",
    "render.c", "tms.c", "vdp.c",
    "loadrom.c", "state.c",
    "sound\sound.c", "sound\sn76489.c", "sound\fmintf.c",
    "sound\emu2413.c", "sound\ym2413.c"
) | ForEach-Object { "$SRC\$_" }
# Platform glue: -Os
$glue_sources = @("main_tdeck.c")

$obj_dir = "obj"
if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

# Newest header mtime anywhere under the source tree. Headers are shared
# (shared.h pulls in every other one), so a single header edit can affect any
# object. We do not parse per-file #includes; instead, if ANY header is newer
# than an object, that object is stale and gets recompiled.
$headers = Get-ChildItem -Path $SRC -Filter *.h -Recurse -ErrorAction SilentlyContinue
$newest_header = if ($headers) {
    ($headers | Measure-Object -Property LastWriteTime -Maximum).Maximum
} else { [DateTime]::MinValue }

$objects = @()
$failed = $false

foreach ($src in ($core_sources + $glue_sources)) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = "$obj_dir/$name.o"
    $objects += $obj

    # Recompile if the source OR any header is newer than the object.
    if ((Test-Path $obj) `
        -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime) `
        -and ($newest_header -le (Get-Item $obj).LastWriteTime)) {
        continue
    }

    $opt = if ($glue_sources -contains $src) { "-Os" } else { "-O2" }
    Write-Host "  CC $name.c ($opt)"
    & $CC $CFLAGS $opt -c -o $obj $src
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED: $name.c"
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

# Every UND symbol must be resolvable from host_exports[] in src/elf_host.cpp,
# or the module will fail to load on device. Audit at build time instead.
Write-Host ""
Write-Host "Undefined symbols (each must be a host export):"
& $READELF --dyn-syms $OUT | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    if ($sym -and $sym -ne "UND") { Write-Host "  $sym" }
}
