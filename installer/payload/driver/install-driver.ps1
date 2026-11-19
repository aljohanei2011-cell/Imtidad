# Installs the Virtual Display Driver (second screen) if it is not installed yet. Runs elevated from the installer.
$ErrorActionPreference = 'Continue'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $dir 'install-driver.log'
"$(Get-Date) start" | Out-File $log
# Settings folder the driver reads (resolutions incl. tablet sizes); writable by signed-in users.
New-Item -ItemType Directory -Force 'C:\VirtualDisplayDriver' | Out-Null
if (-not (Test-Path 'C:\VirtualDisplayDriver\vdd_settings.xml')) {
  Copy-Item (Join-Path (Split-Path $dir) 'vdd_settings.xml') 'C:\VirtualDisplayDriver\vdd_settings.xml'
}
icacls 'C:\VirtualDisplayDriver' /grant '*S-1-5-11:(OI)(CI)M' | Out-Null
$existing = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\MttVDD' }
if ($existing) { "already installed: $($existing.Status)" | Out-File -Append $log; exit 0 }
# Trust the driver's publisher so Windows does not show a second prompt.
$cert = (Get-AuthenticodeSignature (Join-Path $dir 'mttvdd.cat')).SignerCertificate
if ($cert) {
  $store = New-Object System.Security.Cryptography.X509Certificates.X509Store('TrustedPublisher', 'LocalMachine')
  $store.Open('ReadWrite'); $store.Add($cert); $store.Close()
}
$p = Start-Process -FilePath (Join-Path $dir 'devcon.exe') -ArgumentList @('install', "`"$(Join-Path $dir 'MttVDD.inf')`"", 'Root\MttVDD') -Wait -PassThru -WindowStyle Hidden
"devcon exit $($p.ExitCode)" | Out-File -Append $log
exit $p.ExitCode
