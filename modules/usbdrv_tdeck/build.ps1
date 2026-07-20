# Build the TDECK peer-link dynamic driver (match 0A/00/00, Espressif VID).
# Self-contained module source (main.cpp + usb_shim.h).
#
# SDK template notes:
#  - no -Wl,-e,main: drivers have no entry point; the core elf_lookup()s the
#    exported `usbdrv_ops` struct instead
#  - --no-relax + -z nocombreloc: the pcxt-discovered binutils workarounds
#  - UND audit at the end must show ONLY driver_exports[] symbols
#    (src/elf_host.cpp): mem*/str*/snprintf/vsnprintf

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CXX     = "$toolchain\xtensa-esp32s3-elf-g++.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"

$SRC = "main.cpp"
$OUT = "tdeck.drv.elf"

$FLAGS = @(
    "-shared", "-fPIC", "-fno-common", "-Os", "-mlongcalls",
    "-ffunction-sections", "-fdata-sections",
    "-fno-rtti", "-fno-exceptions", "-std=gnu++17",
    "-DUSB_DRV_MODULE",
    "-I.",
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-Wl,--gc-sections",
    "-Wl,--no-relax",
    "-Wl,-z,nocombreloc"
)

Write-Host "  CXX  $SRC -> $OUT"
& $CXX @FLAGS -o $OUT $SRC "-lgcc"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Compilation failed!"
    exit 1
}

$size = (Get-Item $OUT).Length
Write-Host "Success: $OUT ($([math]::Round($size / 1KB)) KB)"

Write-Host ""
Write-Host "Undefined symbols (each must be in driver_exports[], src/elf_host.cpp):"
& $READELF --dyn-syms $OUT | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    if ($sym -and $sym -ne "UND" -and $sym -ne "Name") { Write-Host "  $sym" }
}

Write-Host ""
$ops = & $READELF --dyn-syms $OUT | Select-String " usbdrv_ops$"
if ($ops) {
    Write-Host "usbdrv_ops export: OK"
} else {
    Write-Host "ERROR: usbdrv_ops is NOT exported!"
    exit 1
}

Write-Host ""
Write-Host "Install: copy tdeck.drv.elf + match to L:/usb_drivers/tdeck/ (or the"
Write-Host "SD mirror) on the HOST-role T-Deck, start USB host, cable to the peer."
Write-Host "NOTE: requires firmware with the link socket (FW_API >= 2)."
