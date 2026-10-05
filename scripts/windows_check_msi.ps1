# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Installs an MSI per-user without elevation, fails unless XR Pilot, the runtime with its manifest, the
# PC VR driver and the Start-menu shortcut land and the product registers in the per-user context,
# runs nothing, then uninstalls and fails unless every one of them is gone.
#   scripts/windows_check_msi.ps1 <package.msi>
#   scripts/windows_check_msi.ps1 -SelfTest    # an empty folder must fail the file check
param([string]$Msi, [switch]$SelfTest)
$ErrorActionPreference = 'Stop'

$required = @(
    'bin\xr-pilot.exe',
    'bin\resources\tray_idle.png',
    'runtime\liboxrsys-runtime.dll',
    'runtime\oxrsys-runtime.json',
    'driver\oxrsys\driver.vrdrivermanifest',
    'driver\oxrsys\bin\win64\driver_oxrsys.dll',
    'licenses\LICENSE-APACHE',
    'licenses\LICENSE-MIT',
    'licenses\LICENSE-MPL-2.0'
)
$root = Join-Path $env:LOCALAPPDATA 'Programs\XR Pilot'
$shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\XR Pilot.lnk'

function Get-Missing([string]$base) {
    $required | Where-Object { -not (Test-Path -PathType Leaf (Join-Path $base $_)) }
}

if ($SelfTest) {
    $empty = Join-Path ([System.IO.Path]::GetTempPath()) ('xrpilot-msi-control-' + [guid]::NewGuid())
    New-Item -ItemType Directory $empty | Out-Null
    $missing = @(Get-Missing $empty)
    Remove-Item -Recurse -Force $empty
    if ($missing.Count -ne $required.Count) { Write-Host "control: an empty folder passed $($required.Count - $missing.Count) checks"; exit 1 }
    Write-Host "OK control: an empty folder fails all $($required.Count) file checks"
    exit 0
}

$msiPath = (Resolve-Path $Msi).Path
$log = Join-Path ([System.IO.Path]::GetTempPath()) 'xrpilot-msi-install.log'
$p = Start-Process msiexec -Wait -PassThru -ArgumentList '/i', "`"$msiPath`"", '/qn', '/l*v', "`"$log`""
if ($p.ExitCode -ne 0) { Get-Content $log -Tail 40; throw "install failed ($($p.ExitCode))" }

$failed = 0
foreach ($m in Get-Missing $root) { Write-Host "MISSING $m"; $failed++ }
if (-not (Test-Path $shortcut)) { Write-Host "MISSING shortcut $shortcut"; $failed++ }
$installer = New-Object -ComObject WindowsInstaller.Installer
$product = $installer.ProductsEx('', '', 7) | Where-Object { $_.InstallProperty('ProductName') -eq 'XR Pilot' }
# AssignmentType 0 is a per-user install, 1 per-machine.
if (-not $product) { Write-Host 'MISSING product registration'; $failed++ }
elseif ($product.InstallProperty('AssignmentType') -ne '0') {
    Write-Host "AssignmentType $($product.InstallProperty('AssignmentType')), not per-user (0)"; $failed++
}
if ($failed) { exit 1 }
Write-Host "OK installed per-user: $($required.Count) files and the shortcut under $root"

$p = Start-Process msiexec -Wait -PassThru -ArgumentList '/x', "`"$msiPath`"", '/qn'
if ($p.ExitCode -ne 0) { throw "uninstall failed ($($p.ExitCode))" }
if (Test-Path $root) { Get-ChildItem -Recurse $root | ForEach-Object { Write-Host "LEFT $($_.FullName)" }; exit 1 }
if (Test-Path $shortcut) { Write-Host "LEFT $shortcut"; exit 1 }
Write-Host 'OK uninstalled: the install folder and the shortcut are gone'
exit 0
