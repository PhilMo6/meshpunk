# Build the mtlite LoRa-protocol module: mtlite.cpp + vendor/ (meshtastic-lite)
# compiled as a .loraproto.elf, loaded at boot by src/radio/proto_loader.cpp
# into the protocol pool.
#
# SDK template notes (same rules as the usbdrv modules):
#  - no -Wl,-e,main: protocols have no entry point; the loader elf_lookup()s
#    the exported `loraproto_ops` struct instead
#  - --no-relax + -z nocombreloc: the pcxt-discovered binutils workarounds
#  - -lm -lgcc STATIC: floor() (frequency slots) + compiler float helpers
#    link in; the UND audit below must then show ONLY proto_exports[] symbols
#    (src/elf_host.cpp): libc string/stdio, rand/srand, the mesh_* crypto seam

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CXX     = "$toolchain\xtensa-esp32s3-elf-g++.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"

$SRC = "mtlite.cpp"
$OUT = "mtlite.loraproto.elf"

$FLAGS = @(
    "-shared", "-fPIC", "-fno-common", "-Os", "-mlongcalls",
    "-ffunction-sections", "-fdata-sections",
    "-fno-rtti", "-fno-exceptions", "-std=gnu++17",
    "-I.",
    "-I..\..\src\radio",
    "-I..\..\lib\lua",
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-Wl,--gc-sections",
    "-Wl,--no-relax",
    "-Wl,-z,nocombreloc"
)

Write-Host "  CXX  $SRC -> $OUT"
& $CXX @FLAGS -o $OUT $SRC "-lm" "-lgcc"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Compilation failed!"
    exit 1
}

$size = (Get-Item $OUT).Length
Write-Host "Success: $OUT ($([math]::Round($size / 1KB)) KB)"

Write-Host ""
# UND audit, cross-checked against the ACTUAL proto_exports[] table. Manual
# eyeballing once passed a symbol that only existed in host_exports[] (the
# game-module table in the same file) — it failed at load on the device.
$hostSrc = Get-Content (Join-Path $PSScriptRoot "..\..\src\elf_host.cpp") -Raw
$tbl = [regex]::Match($hostSrc, 'proto_exports\[\]\s*=\s*\{(.*?)\n\};', 'Singleline')
if (-not $tbl.Success) {
    Write-Host "ERROR: could not parse proto_exports[] from src/elf_host.cpp"
    exit 1
}
$exported = @{}
foreach ($em in [regex]::Matches($tbl.Groups[1].Value, '\{\s*"([^"]+)"')) {
    $exported[$em.Groups[1].Value] = $true
}

$missing = @()
Write-Host "Undefined symbols (checked against proto_exports[], src/elf_host.cpp):"
# -W: without it readelf truncates long names ("mesh_aes_block_e[...]"),
# which would false-fail the table lookup.
& $READELF -W --dyn-syms $OUT | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    if ($sym -and $sym -ne "UND" -and $sym -ne "Name") {
        if ($exported.ContainsKey($sym)) {
            Write-Host "  $sym"
        } else {
            Write-Host "  $sym   <-- NOT IN proto_exports[]"
            $missing += $sym
        }
    }
}
if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "ERROR: $($missing.Count) symbol(s) missing from proto_exports[] - the elf WILL fail to load:"
    $missing | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "UND audit: all resolved by proto_exports[]"

Write-Host ""
$ops = & $READELF --dyn-syms $OUT | Select-String " loraproto_ops$"
if ($ops) {
    Write-Host "loraproto_ops export: OK"
} else {
    Write-Host "ERROR: loraproto_ops is NOT exported!"
    exit 1
}

# MTLite ships CATALOG-ONLY (release decision 2026-09-04): nothing is staged
# into the firmware data/ tree. Publishing a new build means copying $OUT to
# meshpunk-apps/protocols/mtlite/ AND syncing module-src/mtlite/ (cmp both).
Write-Host "Device pickup: copy $OUT to L:/meshpunk/lora_protos/mtlite/ (Tools > Files"
Write-Host "or a store update), then pick mtlite in Settings > Lora and reboot."
