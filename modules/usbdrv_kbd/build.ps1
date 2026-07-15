# Build the DOGFOOD dynamic USB keyboard driver: the firmware's own
# src/usb/usb_drv_hid.cpp compiled as a .drv.elf module (-DUSB_DRV_MODULE).
# Loading it (L:/usb_drivers/kbd/ + replug) suppresses the built-in "kbd"
# driver — identical behavior through the dynamic path is the M2 ABI proof.
#
# SDK template notes (copy for new drivers):
#  - no -Wl,-e,main: drivers have no entry point; the core elf_lookup()s the
#    exported `usbdrv_ops` struct instead
#  - --no-relax + -z nocombreloc: the pcxt-discovered binutils workarounds
#  - UND audit at the end must show ONLY driver_exports[] symbols
#    (src/elf_host.cpp): mem*/str*/snprintf/vsnprintf

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CXX     = "$toolchain\xtensa-esp32s3-elf-g++.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"

$SRC = "..\..\src\usb\usb_drv_hid.cpp"
$OUT = "kbd.drv.elf"

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
Write-Host "Install (dogfood test): copy kbd.drv.elf + match to L:/usb_drivers/kbd/"
Write-Host "on the device, then replug the keyboard. '.disabled' file restores"
Write-Host "the built-in driver on the next plug."
