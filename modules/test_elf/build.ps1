# Build a loadable ELF module for ESP32-S3
# Uses the Xtensa toolchain from PlatformIO

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CC = "$toolchain\xtensa-esp32s3-elf-gcc.exe"

$OUT = "test_elf.app.elf"

Write-Host "Compiling test_elf module..."

# Compile as position-independent shared object.
# Undefined symbols (host_* functions) are resolved at load time by the ELF loader.
& $CC -shared -fPIC -fno-common `
    -Os -mlongcalls `
    -nostartfiles -nodefaultlibs -nostdlib `
    "-Wl,-e,main" `
    -o $OUT `
    main.c

if ($LASTEXITCODE -eq 0) {
    $size = (Get-Item $OUT).Length
    Write-Host "Success: $OUT ($size bytes)"

    # Show ELF info
    $READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
    Write-Host "`n--- ELF Header ---"
    & $READELF -h $OUT
    Write-Host "`n--- Dynamic symbols ---"
    & $READELF --dyn-syms $OUT
} else {
    Write-Host "Build failed!"
    exit 1
}
