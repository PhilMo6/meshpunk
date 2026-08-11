# Build Gwenesis (Sega Genesis / Mega Drive) as a loadable ELF module for
# ESP32-S3 T-Deck. Uses the Xtensa toolchain from PlatformIO.

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CC = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"

$SRC = "gwenesis-src"
$OUT = "genesis.app.elf"

# GNW_TARGET_MARIO selects gwenesis's embedded build throughout the core: the
# VDP writes RGB565 shorts straight into the frame buffer instead of expanding
# to RGB888 through a separate palette pass. It is the Game & Watch target
# name upstream, but it is really "small MCU with a 16-bit framebuffer".
$CFLAGS = @(
    "-shared", "-fPIC", "-fno-common",
    "-mlongcalls",
    "-ffunction-sections", "-fdata-sections",
    "-fno-strict-aliasing",
    "-DGNW_TARGET_MARIO=1",
    # BUILD_TABLES swaps Musashi's 65,536-entry const jump table for a BSS
    # array that m68k_init() fills at startup. The const form costs ~245KB of
    # function pointers plus a PIC relocation each -- 740KB of .rela.dyn -- in
    # BOTH the file and PSRAM. The loader holds the whole ELF in PSRAM while
    # it builds the image, so file size and image size both compete with the
    # ROM allocation; this drops ~985KB from the file and ~665KB from RAM in
    # exchange for a table build at init. The same builder fills m68ki_cycles.
    # TABLES_FULL comes along because the opcode table BUILD_TABLES walks
    # references m68k_op_1111 (the line-F trap), which upstream only compiles
    # under TABLES_FULL. It also fixes a real hole: the compact const table
    # has 61,376 entries covering opcodes 0x0000-0xEFFF, so a line-F opcode
    # indexes past its end. The built table is a full 0x10000.
    "-DBUILD_TABLES", "-DTABLES_FULL",
    "-DNDEBUG",
    # The core includes its headers by bare name across directories
    # (gwenesis_bus.c does #include "m68k.h"), so every source dir is on the
    # include path rather than rewriting upstream's includes.
    "-I$SRC", "-I$SRC/src",
    "-I$SRC/src/bus", "-I$SRC/src/io", "-I$SRC/src/vdp",
    "-I$SRC/src/sound", "-I$SRC/src/savestate",
    "-I$SRC/src/cpus/M68K", "-I$SRC/src/cpus/Z80"
)

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-lgcc",
    "-Wl,-e,main",
    "-Wl,--gc-sections"
)

# Emulator core: -O2 (68000/Z80 interpreters, per-line VDP, per-sample FM)
$core_sources = @(
    "src\cpus\M68K\m68kcpu.c",
    "src\cpus\Z80\Z80.c",
    "src\bus\gwenesis_bus.c",
    "src\io\gwenesis_io.c",
    "src\vdp\gwenesis_vdp_gfx.c",
    "src\vdp\gwenesis_vdp_mem.c",
    "src\sound\ym2612.c",
    "src\sound\gwenesis_sn76489.c",
    "src\sound\z80inst.c",
    "src\savestate\gwenesis_savestate.c"
) | ForEach-Object { "$SRC\$_" }
# Platform glue: -Os
$glue_sources = @("main_tdeck.c")

$obj_dir = "obj"
if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

# Newest header mtime anywhere under the source tree. Headers are shared, so a
# single header edit can affect any object; if ANY header is newer than an
# object, that object is stale and gets recompiled.
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

    if ((Test-Path $obj) `
        -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime) `
        -and ($newest_header -le (Get-Item $obj).LastWriteTime)) {
        continue
    }

    $opt = if ($glue_sources -contains $src) { "-Os" } else { "-O2" }
    # The VDP render chain is placed in .iram.text, which the loader copies to
    # internal SRAM — that changes its distance to everything else, so l32r
    # literals must live inside the section (-mtext-section-literals) and no
    # jump tables may be emitted into it (-fno-jump-tables), or the audit
    # below reads that data as code and reports false escapes.
    $extra = if ($name -eq "gwenesis_vdp_gfx" -or $name -eq "ym2612") {
        @("-mtext-section-literals", "-fno-jump-tables")
    } else { @() }
    Write-Host "  CC $name.c ($opt)"
    & $CC $CFLAGS $opt $extra -c -o $obj $src
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

# Musashi's 68000 dispatch is a 65,536-entry table of function pointers, so
# this module carries ~250KB of pointers and a PIC relocation for each. That
# is structural, but the debug/assembler metadata on top of it is not: .xt.prop
# is only needed by the linker for relaxation, and the symbol table is not read
# at load (the loader resolves imports through .dynsym, which strip-debug
# keeps). Together they are ~280KB of the file.
$OBJCOPY = "$toolchain\xtensa-esp32s3-elf-objcopy.exe"
& $OBJCOPY --strip-debug --remove-section=.xt.prop --remove-section=.comment $OUT
if ($LASTEXITCODE -ne 0) {
    Write-Host "Strip failed!"
    exit 1
}

$size = (Get-Item $OUT).Length
Write-Host "Success: $OUT ($([math]::Round($size/1024, 1)) KB)"

# .iram.text is copied into internal SRAM at load, which changes its distance
# to every other section. Absolute references survive (the loader relocates
# them), but l32r is PC-relative and fixed at link time, so every literal it
# reaches must live inside the section itself.
$OBJDUMP = "$toolchain\xtensa-esp32s3-elf-objdump.exe"
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
    # Reported, not fatal. -mtext-section-literals puts literal pools INSIDE
    # function bodies with a branch over them, and objdump disassembles that
    # data as instructions -- so a "leaked" l32r here is usually a misdecoded
    # literal word, not real code. Two tells: the flagged address sits just
    # after a `j` that skips it, and the operand register is a1 (the stack
    # pointer), which no compiler writes with l32r. A genuine escape faults in
    # the VDP the moment a line renders, so hardware is the real arbiter.
    if ($escapes -gt 0) {
        Write-Host "  $escapes of $literals l32r targets appear to fall outside."
        Write-Host "  Check each against the disassembly before trusting it:"
        Write-Host "  a literal pool jumped over by the code above it is benign."
    } else {
        Write-Host "  all $literals l32r targets resolve inside the section"
    }
} else {
    Write-Host ""
    Write-Host "WARNING: no .iram.text section - VDP will run from PSRAM"
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
