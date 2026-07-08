# Build Faux86-remake (PC-XT emulator) as a loadable ELF module
# for ESP32-S3 T-Deck. Uses the Xtensa toolchain from PlatformIO.

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CXX = "$toolchain\xtensa-esp32s3-elf-g++.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
$libdir = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\xtensa-esp32s3-elf\lib\no-rtti"

$SRC = "faux86-src"
$OUT = "pcxt.app.elf"

# ARDUINO selects the upstream embedded path: 16bpp blit, plain libc includes,
# cooperative TaskManager (no ESP32 define -> no esp32-hal.h).
$CXXFLAGS = @(
    "-shared", "-fPIC", "-fno-common",
    "-mlongcalls",
    "-ffunction-sections", "-fdata-sections",
    "-fno-strict-aliasing",
    "-fno-rtti", "-fno-exceptions",
    "-std=gnu++17",
    "-DARDUINO",
    # The ARDUINO code path assumes Arduino.h pre-supplied libc basics
    # (size_t, atoi, str*); force-include them instead of editing vendor files.
    "-include", "cstddef", "-include", "cstdlib",
    "-include", "cstring", "-include", "cstdio",
    "-I$SRC", "-I."
)

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-L$libdir",
    "-Wl,-e,main",
    "-Wl,--gc-sections",
    "-Wl,--no-relax",  # BFD elf32-xtensa relaxation is buggy in this binutils
    # elf32-xtensa places R_XTENSA_RTLD placeholder relocs at the head of
    # .rela.got and asserts (elf32-xtensa.c:3288/3299) they are still there in
    # finish_dynamic_sections — but the default -z combreloc sort runs first
    # and shuffles them. Disable the sort; our elf_loader walks relocs
    # linearly and ignores order.
    "-Wl,-z,nocombreloc"
)

# Hot paths get -O2; the rest -Os
$hot = @(
    "CPU.cpp", "Ram.cpp", "Ports.cpp", "Video.cpp", "Renderer.cpp",
    "Audio.cpp", "Adlib.cpp", "SoundBlaster.cpp", "PCSpeaker.cpp",
    "opl.cpp", "opl3.cpp", "PIT.cpp", "PIC.cpp", "DMA.cpp", "Timing.cpp"
)
# Platform/network shells we don't build
$exclude = @("console.cpp", "netcard.cpp", "packet.cpp")

$core_sources = Get-ChildItem "$SRC\*.cpp" | Where-Object {
    $exclude -notcontains $_.Name
} | ForEach-Object { $_.FullName }

$glue_sources = @("tdeck_host.cpp", "folderdisk.cpp", "main_tdeck.cpp", "cxxstubs.cpp")
$all_sources = $core_sources + ($glue_sources | ForEach-Object { (Get-Item $_).FullName })

$obj_dir = "obj"
if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

$objects = @()
$failed = $false

foreach ($src in $all_sources) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = "$obj_dir/$name.o"
    $objects += $obj

    if ((Test-Path $obj) -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime)) {
        continue
    }

    $srcname = [System.IO.Path]::GetFileName($src)
    $opt = if ($hot -contains $srcname) { "-O2" } else { "-Os" }
    Write-Host "  CXX $srcname ($opt)"
    & $CXX $CXXFLAGS $opt -c -o $obj $src
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
& $CXX $CXXFLAGS $LDFLAGS -o $OUT @objects "-lstdc++" "-lgcc"

if ($LASTEXITCODE -ne 0) {
    Write-Host "Link failed!"
    exit 1
}

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

# .init_array must be either empty or walked by run_static_ctors (main_tdeck)
Write-Host ""
& $READELF -S $OUT | Select-String "init_array"

$dest_dir = "..\..\data\lua\apps\Games\PC-XT"
if (-not (Test-Path $dest_dir)) { New-Item -ItemType Directory $dest_dir | Out-Null }
Copy-Item $OUT "$dest_dir\pcxt.app.elf" -Force
Write-Host ""
Write-Host "Copied to $dest_dir\pcxt.app.elf"
