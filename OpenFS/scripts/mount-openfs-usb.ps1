#requires -Version 5.1
<#
.SYNOPSIS
Safely mount or unmount an already formatted OpenFS USB disk through the OpenFS CLI.

.DESCRIPTION
Mount mode verifies the exact Windows disk UniqueId and USB/boot/system properties,
takes an online disk offline, and runs the blocking OpenFS mount command. When that
command returns after an unmount, the script restores the disk to its previous
online state if this script took it offline.

Unmount mode only sends the unmount request. Run it from a second elevated PowerShell
window while the mount-mode window remains open.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Mount", "Unmount", "Format")]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [string]$OpenFsExe,

    [Parameter(Mandatory = $true)]
    [ValidatePattern("^[A-Za-z]:?$")]
    [string]$MountPoint,

    [int]$DiskNumber,

    [string]$UniqueId,

    [string]$ConfirmEraseAllData
)

$ErrorActionPreference = "Stop"

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "OpenFS USB mount management requires an elevated PowerShell window."
}

$exePath = (Resolve-Path -LiteralPath $OpenFsExe).Path
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "OpenFS executable not found: $OpenFsExe"
}

if ($MountPoint.EndsWith(":")) {
    $MountPoint = $MountPoint + "\"
}

if ($Action -eq "Unmount") {
    & $exePath unmount $MountPoint
    if ($LASTEXITCODE -ne 0) {
        throw "OpenFS unmount failed with exit code $LASTEXITCODE."
    }
    Write-Host "Unmount request sent. Wait for the mount window to finish before unplugging the USB disk."
    return
}

if ($Action -eq "Format" -and $ConfirmEraseAllData -cne "ERASE-ALL-DATA") {
    throw "Formatting erases the entire USB disk. Pass -ConfirmEraseAllData ERASE-ALL-DATA exactly to continue."
}

if (-not $PSBoundParameters.ContainsKey("DiskNumber") -or $DiskNumber -lt 0) {
    throw "DiskNumber must identify the dedicated USB disk."
}
if ([string]::IsNullOrWhiteSpace($UniqueId)) {
    throw "The exact UniqueId from Get-Disk is required."
}

$disk = Get-Disk -Number $DiskNumber
if ($disk.BusType -ne "USB") {
    throw "Refusing disk $DiskNumber: BusType is '$($disk.BusType)', not USB."
}
if (-not [string]::Equals([string]$disk.UniqueId, $UniqueId, [StringComparison]::Ordinal)) {
    throw "Refusing disk $DiskNumber: its UniqueId does not exactly match the supplied value."
}
if ($disk.IsBoot -or $disk.IsSystem) {
    throw "Refusing disk $DiskNumber: Windows identifies it as a boot or system disk."
}
if ($disk.IsReadOnly) {
    throw "Refusing disk $DiskNumber: the disk is read-only."
}

$changedOfflineState = -not $disk.IsOffline
try {
    if ($changedOfflineState) {
        Set-Disk -Number $DiskNumber -IsOffline $true
        $offline = $false
        for ($attempt = 0; $attempt -lt 20; $attempt++) {
            Start-Sleep -Milliseconds 250
            $offline = (Get-Disk -Number $DiskNumber).IsOffline
            if ($offline) { break }
        }
        if (-not $offline) {
            throw "Windows did not confirm that disk $DiskNumber went offline."
        }
    }

    if ($Action -eq "Format") {
        Write-Warning "ERASING ALL DATA AND PARTITIONS on USB disk $DiskNumber ($UniqueId)."
        & $exePath format-usb $DiskNumber $UniqueId ERASE-ALL-DATA
        if ($LASTEXITCODE -ne 0) {
            throw "OpenFS format-usb failed with exit code $LASTEXITCODE."
        }
    }
    else {
        Write-Host "Starting OpenFS on USB disk $DiskNumber at $MountPoint."
        Write-Host "Keep this window open. Use a second elevated PowerShell window to unmount."
        & $exePath mount-usb $DiskNumber $UniqueId $MountPoint
        if ($LASTEXITCODE -ne 0) {
            throw "OpenFS mount-usb failed with exit code $LASTEXITCODE."
        }
    }
}
finally {
    if ($changedOfflineState) {
        Write-Host "Restoring disk $DiskNumber to its previous online state..."
        Set-Disk -Number $DiskNumber -IsOffline $false
    }
}
