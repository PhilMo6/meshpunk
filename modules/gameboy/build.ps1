# Build gnuboy (Game Boy / Game Boy Color) as a loadable ELF module
# for ESP32-S3 T-Deck. Uses the Xtensa toolchain from PlatformIO.

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CC = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"

$SRC = "gnuboy-src"
$OUT = "gameboy.app.elf"

# Common compiler flags (per-file -O flag added below)
$CFLAGS = @(
    "-shared", "-fPIC", "-fno-common",
    "-mlongcalls",
    "-ffunction-sections", "-fdata-sections",
    "-fno-strict-aliasing",
    "-I$SRC"
)

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-lgcc",
    "-Wl,-e,main",
    "-Wl,--gc-sections"
)

# Emulator core: -O2 (per-scanline LCD + per-sample APU hot paths, PICO-8 precedent)
$core_sources = @("cpu.c", "hw.c", "lcd.c", "sound.c", "gnuboy.c") | ForEach-Object { "$SRC\$_" }
# Platform glue: -Os
$glue_sources = @("main_tdeck.c")

$obj_dir = "obj"
if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

$objects = @()
$failed = $false

foreach ($src in ($core_sources + $glue_sources)) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = "$obj_dir/$name.o"
    $objects += $obj

    # Only recompile if source is newer than object
    if ((Test-Path $obj) -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime)) {
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

# Copy to LittleFS data dir so it's included in firmware flash
$dest_dir = "..\..\data\lua\apps\Games\GameBoy"
if (-not (Test-Path $dest_dir)) { New-Item -ItemType Directory $dest_dir | Out-Null }
Copy-Item $OUT "$dest_dir\gameboy.app.elf" -Force
Write-Host ""
Write-Host "Copied to $dest_dir\gameboy.app.elf"
