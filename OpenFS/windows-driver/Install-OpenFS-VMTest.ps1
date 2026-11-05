#Requires -RunAsAdministrator
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$certificatePath = Join-Path $packageRoot 'OpenFS-TestSigning.cer'
$infPath = Join-Path $packageRoot 'OpenFS.inf'

foreach ($path in @($certificatePath, $infPath, (Join-Path $packageRoot 'OpenFS.sys'), (Join-Path $packageRoot 'OpenFS.cat'))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Package incomplete: missing $path"
    }
}

# A test-signed kernel driver requires TESTSIGNING and Secure Boot disabled
# inside this disposable VM. Never run this installer on the host PC.
$bootConfig = (& bcdedit.exe /enum '{current}' 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "Unable to inspect boot configuration: $bootConfig" }
if ($bootConfig -notmatch '(?im)^\s*testsigning\s+Yes\s*$') {
    & bcdedit.exe /set testsigning on
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not enable TESTSIGNING. Disable Secure Boot in the VM firmware, then rerun this script.'
    }
    Write-Warning 'TESTSIGNING enabled. Reboot this VM, then run this script again.'
    exit 3010
}

Import-Certificate -FilePath $certificatePath -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $certificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null

# Install the filesystem service from its DefaultInstall section.
$arguments = 'setupapi.dll,InstallHinfSection DefaultInstall 132 "{0}"' -f $infPath
$process = Start-Process -FilePath "$env:SystemRoot\System32\rundll32.exe" -ArgumentList $arguments -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "INF installation failed with exit code $($process.ExitCode)." }

& sc.exe start OpenFS
if ($LASTEXITCODE -ne 0) {
    $state = (& sc.exe query OpenFS 2>&1 | Out-String)
    throw "OpenFS service did not start. sc.exe returned $LASTEXITCODE. Details: $state"
}
& sc.exe query OpenFS
Write-Warning 'OpenFS is an experimental overwrite-only driver: existing regular files only, no EOF extension, allocation, create, rename, delete, or metadata writes. It is not crash-atomic.'
Write-Warning 'Use only with disposable OpenFS disk images in an isolated VMware test VM. Never use a volume containing valuable data.'
Write-Host 'OpenFS driver service started.'
