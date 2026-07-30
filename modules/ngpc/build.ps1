# Build RACE (Neo Geo Pocket / Color emulator) as a loadable ELF module
# for ESP32-S3 T-Deck. Uses the Xtensa toolchain from PlatformIO.

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CC = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"

$RACE = "race-src"
$OUT = "ngpc.app.elf"

# Common compiler flags (per-file -O flag added below).
# -fvisibility=hidden: module-internal globals are non-preemptible, so PIC
# addressing uses a direct literal (RELATIVE reloc) instead of the
# literal->GOT->data double load. Only host imports go through the GOT.
$CFLAGS = @(
    "-shared", "-fPIC", "-fvisibility=hidden", "-fno-common",
    "-mlongcalls",
    "-ffunction-sections", "-fdata-sections",
    "-fno-strict-aliasing",
    "-I$RACE",
    "-I$RACE\libretro",
    "-I$RACE\libretro-common\include",
    "-I$RACE\deps\blip",
    "-DLSB_FIRST",
    "-DINLINE=inline",
    "-D_MAX_PATH=256",
    "-DCZ80",
    "-include", "stdio.h",
    "-Wno-implicit-function-declaration",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-Wno-format"
)

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-lgcc",
    "-Wl,-e,main",
    "-Wl,--gc-sections",
    # Bind internal references to their local definitions (nothing here is
    # interposable).
    "-Wl,-Bsymbolic",
    # elf32-xtensa places R_XTENSA_RTLD placeholder relocs at the head of
    # .rela.got and asserts (elf32-xtensa.c:3299) they are still there in
    # finish_dynamic_sections — but the default -z combreloc sort runs first
    # and shuffles them. This module's link trips that assert; disable the
    # sort. elf_loader walks relocs linearly and ignores order.
    "-Wl,-z,nocombreloc"
)

# CPU interpreters + the memory dispatch every emulated access funnels
# through: -O3. Sound synthesis and render: -O2. Rest: -Os.
$hot3 = @(
    "tlcs900h.c", "cz80.c", "cz80_support.c", "race-memory.c"
)
$hot = @(
    "neopopsound.c", "neopop_blip.c", "sound.c",
    "graphics.c", "Blip_Buffer.c"
)

# RACE core (portable C only — DrZ80 asm excluded, CZ80 used instead)
$core_sources = @(
    "$RACE\main.c", "$RACE\flash.c", "$RACE\graphics.c",
    "$RACE\race-memory.c", "$RACE\ngpBios.c", "$RACE\neopopsound.c",
    "$RACE\neopop_blip.c", "$RACE\sound.c", "$RACE\state.c", "$RACE\tlcs900h.c",
    "$RACE\cz80.c", "$RACE\cz80_support.c",
    "$RACE\deps\blip\Blip_Buffer.c",
    "$RACE\libretro\libretro.c", "$RACE\libretro\log.c"
)
# Platform glue
$glue_sources = @("main_tdeck.c")

$obj_dir = "obj"
if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

# Newest header mtime anywhere under the source tree. Headers are shared, so a
# single header edit can affect any object. We do not parse per-file #includes;
# if ANY header is newer than an object, that object is stale and recompiles.
$headers = Get-ChildItem -Path $RACE -Filter *.h -Recurse -ErrorAction SilentlyContinue
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

    $srcname = [System.IO.Path]::GetFileName($src)
    $opt = if ($hot3 -contains $srcname) { "-O3" }
           elseif ($hot -contains $srcname) { "-O2" }
           else { "-Os" }
    Write-Host "  CC $srcname ($opt)"
    & $CC $CFLAGS $opt -c -o $obj $src
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

# Every UND symbol must be resolvable from host_exports[] in src/elf_host.cpp,
# or the module will fail to load on device. Audit at build time instead.
Write-Host ""
Write-Host "Undefined symbols (each must be a host export):"
& $READELF --dyn-syms $OUT | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    if ($sym -and $sym -ne "UND") { Write-Host "  $sym" }
}
