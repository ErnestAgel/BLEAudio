# Command-line build for NCS v3.4.2 (same toolchain as nRF Connect for VS Code)
# Usage: ./build.ps1 [-Pristine]
param([switch]$Pristine)

$tc  = 'c:\ncs\toolchains\5f5ef0cea0'
$sdk = 'F:\Buletooth\nRF52840\sdk\v3.4.2'
$env:PATH = "$tc\opt\bin;$tc\opt\bin\Scripts;$tc\bin;$tc\opt\nanopb\generator-bin;$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin;$env:PATH"
$env:ZEPHYR_BASE = "$sdk\zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR = "$tc\opt\zephyr-sdk"
$env:ZEPHYR_TOOLCHAIN_VARIANT = 'zephyr'

Push-Location $PSScriptRoot
$p = if ($Pristine) { 'always' } else { 'auto' }
python -m west build -b nrf52840dk/nrf52840 --sysbuild -d build -p $p .
Pop-Location
