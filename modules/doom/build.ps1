# Build Doom as a loadable ELF module for ESP32-S3 T-Deck
# Uses the Xtensa toolchain from PlatformIO

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CC = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
$SIZE = "$toolchain\xtensa-esp32s3-elf-size.exe"

$DG = "doomgeneric-src/doomgeneric"
$OUT = "doom.app.elf"

# Common compiler flags
$CFLAGS = @(
    "-shared", "-fPIC", "-fno-common",
    "-Os", "-mlongcalls",
    "-DDOOMGENERIC_RESX=320",
    "-DDOOMGENERIC_RESY=200",
    "-I$DG",
    "-Wno-implicit-function-declaration",
    "-Wno-int-conversion",
    "-Wno-pointer-to-int-cast"
)

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

$all_sources = $tdeck_sources + $dg_sources

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

    Write-Host "  CC $name.c"
    & $CC $CFLAGS -c -o $obj $src
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

    # Copy to LittleFS data dir so it's included in firmware flash
    $dest = "..\..\data\lua\apps\Games\Doom\doom.app.elf"
    Copy-Item $OUT $dest -Force
    Write-Host "Copied to $dest"
} else {
    Write-Host "Link failed!"
    exit 1
}
