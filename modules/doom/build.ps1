# Build Doom as a loadable ELF module for ESP32-S3 T-Deck
# Uses the Xtensa toolchain from PlatformIO

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CC = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
$SIZE = "$toolchain\xtensa-esp32s3-elf-size.exe"

$DG = "doomgeneric-src/doomgeneric"
$OUT = "doom.app.elf"

# Common compiler flags (per-file -Os/-O2 chosen below)
$CFLAGS = @(
    "-shared", "-fPIC", "-fno-common",
    "-mlongcalls",
    "-DDOOMGENERIC_RESX=320",
    "-DDOOMGENERIC_RESY=200",
    "-DNDEBUG",                # assert() in vendored opl/ code would pull __assert_func
    "-I$DG", "-Iopl",
    "-Wno-implicit-function-declaration",
    "-Wno-int-conversion",
    "-Wno-pointer-to-int-cast"
)

# Hot audio path gets -O2 (pcxt precedent); everything else -Os
$hot = @("dbopl.c", "opl_tdeck.c")

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-lgcc",
    "-Wl,-e,main"
)

# All doomgeneric source files EXCEPT platform-specific ones we replace
$DG_EXCLUDE = @(
    "doomgeneric_sdl.c", "doomgeneric_win.c", "doomgeneric_xlib.c",
    "doomgeneric_soso.c", "doomgeneric_sosox.c", "doomgeneric_emscripten.c",
    "doomgeneric_allegro.c", "doomgeneric_linuxvt.c",
    # w_file_stdc.c is INCLUDED — uses streaming fopen/fread (SPI-locked by host)
    "i_sdlmusic.c", "i_sdlsound.c",  # no SDL
    "i_allegromusic.c", "i_allegrosound.c",  # no Allegro
    "icon.c"                   # SDL icon, not needed
)

$dg_sources = Get-ChildItem "$DG\*.c" | Where-Object {
    $DG_EXCLUDE -notcontains $_.Name
} | ForEach-Object { $_.FullName }

# Our platform files
$tdeck_sources = @(
    "main_tdeck.c",
    "doomgeneric_tdeck.c",
    "i_tdeck_sound.c"
)

# OPL music: chocolate-doom 2.2.1 player + DBOPL synth + T-Deck backend
$opl_sources = @(
    "opl/opl_tdeck.c",
    "opl/dbopl.c",
    "opl/opl_queue.c",
    "opl/i_oplmusic.c",
    "opl/midifile.c"
)

$all_sources = $tdeck_sources + $opl_sources + $dg_sources

Write-Host "Compiling Doom module ($($all_sources.Count) source files)..."
Write-Host "  Resolution: 320x200"

# Compile all sources into object files first, then link
$obj_dir = "obj"
if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

$objects = @()
$failed = $false

foreach ($src in $all_sources) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = "$obj_dir/$name.o"
    $objects += $obj

    # Only recompile if source is newer than object
    if ((Test-Path $obj) -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime)) {
        continue
    }

    $srcname = [System.IO.Path]::GetFileName($src)
    $opt = if ($hot -contains $srcname) { "-O2" } else { "-Os" }
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

if ($LASTEXITCODE -eq 0) {
    $size = (Get-Item $OUT).Length
    Write-Host "Success: $OUT ($([math]::Round($size/1024, 1)) KB)"

    # Every UND symbol must be resolvable from host_exports[] in src/elf_host.cpp
    Write-Host ""
    Write-Host "Undefined symbols (each must be a host export):"
    & $READELF --dyn-syms $OUT | Select-String "\bUND\b" | ForEach-Object {
        $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
        $sym = $parts[$parts.Length - 1]
        if ($sym -and $sym -ne "UND") { Write-Host "  $sym" }
    }
    Write-Host ""

    # Copy to LittleFS data dir so it's included in firmware flash
    $dest = "..\..\data\lua\apps\Games\Doom\doom.app.elf"
    Copy-Item $OUT $dest -Force
    Write-Host "Copied to $dest"
} else {
    Write-Host "Link failed!"
    exit 1
}
