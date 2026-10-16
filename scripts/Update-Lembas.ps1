<#
  Lembas for Windows - update now. (Lembas also checks once a day and updates itself quietly;
  this is for when you don't want to wait.) Your backups, location and chosen look are kept.
#>
$Dest = Split-Path -Parent $PSScriptRoot
$config = Get-Content (Join-Path $Dest 'lembas.json') -Raw | ConvertFrom-Json
$url = $config.updateUrl
$work = Join-Path $env:TEMP ("lembas-update-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work -Force | Out-Null

Write-Host ""
Write-Host "  LEMBAS - update" -ForegroundColor Yellow
Write-Host "  Downloading $url" -ForegroundColor DarkGray
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $zip = Join-Path $work 'lembas.zip'
    Invoke-WebRequest $url -OutFile $zip -UseBasicParsing
    Expand-Archive $zip -DestinationPath $work -Force
    $installer = Get-ChildItem $work -Recurse -Filter 'Install-Lembas.ps1' | Select-Object -First 1
    if (-not $installer) { throw "the download doesn't look like Lembas" }
    $newVersion = (Get-Content (Join-Path (Split-Path (Split-Path $installer.FullName)) 'version.txt') -ErrorAction SilentlyContinue) -join ''
    Write-Host "  Installed: $($config.version)   Newest: $newVersion" -ForegroundColor Gray
    & $installer.FullName -NoPause
} catch {
    Write-Host ""
    Write-Host "  Couldn't download the update: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "  Check your internet connection and try again." -ForegroundColor Gray
} finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host ""
Read-Host "  Press Enter to close" | Out-Null
