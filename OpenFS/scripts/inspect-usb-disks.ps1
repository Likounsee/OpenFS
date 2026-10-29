[CmdletBinding()]
param(
    [Parameter()]
    [UInt32] $DiskNumber = [UInt32]::MaxValue
)

$ErrorActionPreference = 'Stop'

# This script is deliberately inventory-only. It never initializes, partitions,
# formats, clears, or writes to a disk.
if (-not (Get-Command Get-Disk -ErrorAction SilentlyContinue)) {
    throw "Get-Disk is unavailable. Run this script in Windows PowerShell 5.1 or PowerShell 7 on Windows."
}

$disks = @(Get-Disk | Sort-Object Number)
if ($disks.Count -eq 0) {
    Write-Output "No disks reported by Windows."
    exit 0
}

$report = foreach ($disk in $disks) {
    $partitions = @(Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue)
    $volumes = foreach ($partition in $partitions) {
        Get-Volume -Partition $partition -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FileSystemLabel -ErrorAction SilentlyContinue
    }
    [pscustomobject]@{
        DiskNumber       = $disk.Number
        FriendlyName     = $disk.FriendlyName
        SerialNumber     = ($disk.SerialNumber -as [string]).Trim()
        BusType          = [string]$disk.BusType
        SizeBytes        = [UInt64]$disk.Size
        PartitionStyle   = [string]$disk.PartitionStyle
        IsBoot           = [bool]$disk.IsBoot
        IsSystem         = [bool]$disk.IsSystem
        IsOffline        = [bool]$disk.IsOffline
        IsReadOnly       = [bool]$disk.IsReadOnly
        OperationalStatus = ($disk.OperationalStatus -join ',')
        VolumeLabels     = (($volumes | Where-Object { $_ }) -join ',')
        CandidateUSB     = ([string]$disk.BusType -eq 'USB')
    }
}

if ($DiskNumber -ne [UInt32]::MaxValue) {
    $selected = @($report | Where-Object { $_.DiskNumber -eq $DiskNumber })
    if ($selected.Count -ne 1) {
        throw "Disk number $DiskNumber was not found. No disk was changed."
    }
    $selected | Format-List
    Write-Host ""
    Write-Host "READ-ONLY INVENTORY ONLY: no disk was opened for writing or modified."
    if ($selected[0].IsBoot -or $selected[0].IsSystem) {
        Write-Warning "This disk is marked as boot/system. Do not use it for OpenFS tests."
    }
    if (-not $selected[0].CandidateUSB) {
        Write-Warning "Windows does not report this disk as USB. Verify its identity physically."
    }
} else {
    $report | Format-Table DiskNumber, FriendlyName, SerialNumber, BusType, SizeBytes, PartitionStyle, IsBoot, IsSystem, IsOffline, IsReadOnly, CandidateUSB, VolumeLabels -AutoSize
    Write-Host ""
    Write-Host "READ-ONLY INVENTORY ONLY: no disk was opened for writing or modified."
    Write-Host "Re-run with -DiskNumber N to inspect one candidate in detail."
}
