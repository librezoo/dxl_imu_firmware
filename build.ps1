#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$MounRiverRoot = 'C:\MounRiver\MounRiver_Studio2',
    [string]$BuildDirectory = 'build-ch32'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-BuildTool {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Build tool failed with exit code ${LASTEXITCODE}: $Executable"
    }
}

$repoRoot = $PSScriptRoot
$toolchainBin = Join-Path $MounRiverRoot 'resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin'
$gcc = Join-Path $toolchainBin 'riscv-none-embed-gcc.exe'
$objcopy = Join-Path $toolchainBin 'riscv-none-embed-objcopy.exe'
$size = Join-Path $toolchainBin 'riscv-none-embed-size.exe'
foreach ($tool in @($gcc, $objcopy, $size)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Missing tool: $tool. Install MounRiver Studio 2 with the WCH RISC-V Embedded GCC toolchain, or set -MounRiverRoot."
    }
}

# Verify the exact SDK bytes recorded when the official package was imported.
$vendorRoot = Join-Path $repoRoot 'vendor\wch-ch32v20x'
$manifest = Get-Content -LiteralPath (Join-Path $vendorRoot 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($entry in $manifest.files) {
    $sdkFile = Join-Path $vendorRoot $entry.path
    if ((Get-FileHash -LiteralPath $sdkFile -Algorithm SHA256).Hash -ne $entry.sha256) {
        throw "SDK integrity check failed: $($entry.path)"
    }
}
Write-Host "Verified $($manifest.files.Count) bundled SDK files."
Invoke-BuildTool -Executable $gcc -Arguments @('--version')

if (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot $BuildDirectory
}
$outputRoot = [System.IO.Path]::GetFullPath($BuildDirectory)
$objectRoot = Join-Path $outputRoot 'obj'
New-Item -ItemType Directory -Path $objectRoot -Force | Out-Null

$evt = 'vendor/wch-ch32v20x/EVT'
$sdk = "$evt/EXAM/SRC"
$system = "$evt/EXAM/USART/USART_DMA/User"
$originalLinker = [System.IO.File]::ReadAllText((Join-Path $repoRoot "$sdk/Ld/Link.ld"))
if ([regex]::Matches($originalLinker, 'LENGTH = 64K').Count -ne 1) {
    throw 'Expected exactly one 64K flash region in the WCH linker script.'
}
$linker = Join-Path $outputRoot 'imu_power_link.ld'
[System.IO.File]::WriteAllText($linker, $originalLinker.Replace('LENGTH = 64K', 'LENGTH = 56K'), [System.Text.UTF8Encoding]::new($false))

# Keep the sources and flags aligned with CMakeLists.txt's imu_power_ch32 target.
$sources = @(
    "$sdk/Core/core_riscv.c",
    "$sdk/Peripheral/src/ch32v20x_gpio.c",
    "$sdk/Peripheral/src/ch32v20x_rcc.c",
    "$sdk/Peripheral/src/ch32v20x_spi.c",
    "$sdk/Peripheral/src/ch32v20x_usart.c",
    "$sdk/Peripheral/src/ch32v20x_dma.c",
    "$sdk/Peripheral/src/ch32v20x_exti.c",
    "$sdk/Peripheral/src/ch32v20x_flash.c",
    "$sdk/Peripheral/src/ch32v20x_iwdg.c",
    "$sdk/Peripheral/src/ch32v20x_misc.c",
    "$system/system_ch32v20x.c",
    'platform/ch32_main.c',
    'platform/ch32v203_port.c',
    "$sdk/Startup/startup_ch32v20x_D6.S",
    'src/config.c',
    'src/device.c',
    'src/dxl.c',
    'src/imu_wire.c',
    'src/maintenance.c'
)
$compileFlags = @(
    '-Os', '-march=rv32imac', '-mabi=ilp32', '-msmall-data-limit=8',
    '-mno-save-restore', '-ffunction-sections', '-fdata-sections',
    '-DIMU_POWER_CH32', '-DCH32V20x_D6', '-MMD', '-MP'
)
foreach ($include in @('include', 'platform', "$sdk/Core", "$sdk/Debug", "$sdk/Peripheral/inc", $system)) {
    $compileFlags += '-I' + (Join-Path $repoRoot $include)
}

$objects = @()
foreach ($source in $sources) {
    $sourceFile = Join-Path $repoRoot $source
    $objectFile = Join-Path $objectRoot ([System.IO.Path]::GetFileName($source) + '.o')
    $arguments = $compileFlags
    if ([System.IO.Path]::GetExtension($source) -eq '.c') {
        $arguments += '-std=gnu99'
    }
    Write-Host "Compiling $source"
    Invoke-BuildTool -Executable $gcc -Arguments ($arguments + @('-c', $sourceFile, '-o', $objectFile))
    $objects += $objectFile
}

$elf = Join-Path $outputRoot 'imu_power_ch32.elf'
$map = Join-Path $outputRoot 'imu_power_ch32.map'
$linkFlags = @(
    '-march=rv32imac', '-mabi=ilp32', '--specs=nano.specs', '--specs=nosys.specs',
    "-T$linker", '-Wl,--gc-sections', '-nostartfiles', "-Wl,-Map=$map"
)
Invoke-BuildTool -Executable $gcc -Arguments ($linkFlags + $objects + @('-o', $elf))
Invoke-BuildTool -Executable $objcopy -Arguments @('-O', 'ihex', $elf, (Join-Path $outputRoot 'imu_power_ch32.hex'))
Invoke-BuildTool -Executable $objcopy -Arguments @('-O', 'binary', $elf, (Join-Path $outputRoot 'imu_power_ch32.bin'))
Invoke-BuildTool -Executable $size -Arguments @($elf)
Write-Host "Firmware built in $outputRoot"
