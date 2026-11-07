# Build the Pyxel game player (MicroPython + pyxel API) as a loadable ELF
# module for the ESP32-S3, and install it with its Python library into the
# firmware data tree (data/lua/apps/Games/Pyxel).

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CC = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
$OBJDUMP = "$toolchain\xtensa-esp32s3-elf-objdump.exe"

$OUT = "pyxel.app.elf"
$MP = "micropython"
$LODEPNG = "..\pico8\fake08-src\libs\lodepng"
$DEST = "..\..\data\lua\apps\Games\Pyxel"

$CFLAGS = @(
    "-shared", "-fPIC", "-fno-common",
    "-mlongcalls",
    "-ffunction-sections", "-fdata-sections",
    "-fno-strict-aliasing",
    "-std=gnu99",
    "-DNDEBUG",
    "-DLODEPNG_NO_COMPILE_ENCODER", "-DLODEPNG_NO_COMPILE_DISK",
    "-DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS", "-DLODEPNG_NO_COMPILE_CPP",
    "-I$MP", "-Isrc", "-I$LODEPNG",
    "-Wno-unused-variable", "-Wno-unused-but-set-variable"
)

# -z nocombreloc: keeps the R_XTENSA_RTLD relocs where BFD expects them
# (elf32-xtensa.c assert). No libm: math comes from host exports plus
# src/libm_extra.c.
$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-Wl,-e,main",
    "-Wl,--gc-sections",
    "-Wl,--no-relax",
    "-Wl,-z,nocombreloc"
)

# The interpreter loop, object model and all drawing: -O2. The rest: -Os.
$hot = @(
    "vm.c", "runtime.c", "map.c", "obj.c", "objfun.c", "objtype.c", "objlist.c",
    "objint.c", "objfloat.c", "objdict.c", "gc.c", "qstr.c", "nativeglue.c",
    "bc.c", "argcheck.c", "objmodule.c", "objtuple.c",
    "gfx.c", "display.c", "api.c", "input.c", "audio.c", "sound.c"
)

# Files holding .iram.text functions (MICROPY_WRAP_* and PX_IRAM in
# mpconfigport.h). The section is copied to internal SRAM as a unit, so its
# literal pools must live inside it (-mtext-section-literals), and switch
# tables must not be emitted into it (-fno-jump-tables). Audited after linking.
$iram_files = @(
    "vm.c", "map.c", "runtime.c", "obj.c", "objfun.c", "objtype.c", "objint.c",
    "objfloat.c", "objtuple.c", "objmodule.c", "gc.c", "qstr.c", "bc.c", "argcheck.c"
)

$sources = @()
$sources += Get-ChildItem "$MP\py\*.c" | ForEach-Object { $_.FullName }
$sources += (Get-Item "$MP\shared\runtime\gchelper_generic.c").FullName
$sources += (Get-Item "$MP\extmod\modjson.c").FullName
$sources += Get-ChildItem "src\*.c" | ForEach-Object { $_.FullName }
$sources += (Get-Item "$LODEPNG\lodepng.cpp").FullName

$obj_dir = "obj"
if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

# Any header newer than an object makes that object stale (no per-file deps).
$headers = @()
$headers += Get-ChildItem -Path $MP -Filter *.h -Recurse
$headers += Get-ChildItem -Path "src" -Filter *.h
$newest_header = ($headers | Measure-Object -Property LastWriteTime -Maximum).Maximum

$objects = @()
$failed = $false
foreach ($src in $sources) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $dir = Split-Path (Split-Path $src -Parent) -Leaf
    $obj = "$obj_dir/${dir}_$name.o"
    $objects += $obj
    if ((Test-Path $obj) `
        -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime) `
        -and ($newest_header -le (Get-Item $obj).LastWriteTime)) {
        continue
    }
    $srcname = [System.IO.Path]::GetFileName($src)
    $opt = if ($hot -contains $srcname) { "-O2" } else { "-Os" }
    $lang = if ($srcname -eq "lodepng.cpp") { @("-x", "c") } else { @() }
    # src/: no fused multiply-add (madd.s), so float rounding matches upstream's
    # (Rust never contracts) and blit transforms pick the same source pixels.
    $extra = @(if (($iram_files -contains $srcname) -and ($dir -eq "py")) {
        "-mtext-section-literals", "-fno-jump-tables"
    } elseif ($dir -eq "src") {
        "-ffp-contract=off"
    })
    Write-Host "  CC $srcname ($opt)"
    & $CC $CFLAGS $opt @lang @extra -c -o $obj $src
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
& $CC $CFLAGS $LDFLAGS -o $OUT @objects "-lgcc"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Link failed!"
    exit 1
}

$size = (Get-Item $OUT).Length
Write-Host "Success: $OUT ($([math]::Round($size/1024, 1)) KB)"

# l32r is PC-relative and fixed at link time: every literal an .iram.text
# instruction loads must lie inside the section, or it breaks once moved.
$iram = & $READELF -S -W $OUT | Select-String '\.iram\.text\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)'
if (-not $iram) {
    Write-Host "No .iram.text section!"
    exit 1
}
$sec_addr = [Convert]::ToUInt32($iram.Matches[0].Groups[1].Value, 16)
$sec_size = [Convert]::ToUInt32($iram.Matches[0].Groups[2].Value, 16)
$sec_end = $sec_addr + $sec_size
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
    exit 1
}
Write-Host "  all $literals l32r targets resolve inside the section"

# Every UND symbol must be in host_exports[] (src/elf_host.cpp) or the module
# fails to load: checked against the table itself, and fatal here.
$exports = @{}
Select-String -Path "..\..\src\elf_host.cpp" -Pattern '\{\s*"([A-Za-z_0-9]+)"' -AllMatches | ForEach-Object {
    foreach ($m in $_.Matches) { $exports[$m.Groups[1].Value] = $true }
}
$missing = @()
Write-Host ""
Write-Host "Undefined symbols:"
& $READELF --dyn-syms $OUT | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    if ($sym -and $sym -ne "UND") {
        if ($exports.ContainsKey($sym)) { Write-Host "  $sym" }
        else { Write-Host "  $sym   <-- NOT EXPORTED"; $missing += $sym }
    }
}
if ($missing.Count -gt 0) {
    Write-Host "Module imports $($missing.Count) symbol(s) the firmware does not export!"
    exit 1
}

if (-not (Test-Path $DEST)) { New-Item -ItemType Directory $DEST | Out-Null }
if (-not (Test-Path "$DEST\pylib")) { New-Item -ItemType Directory "$DEST\pylib" | Out-Null }
Copy-Item $OUT "$DEST\pyxel.app.elf" -Force
Copy-Item "pylib\*" "$DEST\pylib\" -Recurse -Force
Write-Host "Installed to $DEST"
