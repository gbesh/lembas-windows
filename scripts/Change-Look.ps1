<#
  Lembas for Windows - follow the sun (the default), or keep one look all the time.
#>
param([ValidateSet('', 'adaptive', 'shire', 'redbook', 'starlit', 'elven')][string]$Look = '')

$Dest = Split-Path -Parent $PSScriptRoot
$configFile = Join-Path $Dest 'lembas.json'
$config = Get-Content $configFile -Raw | ConvertFrom-Json

if (-not $Look) {
    Write-Host ""
    Write-Host "  LEMBAS - choose a look" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "   1  Follow the sun (Shire Morning, Red Book, Starlit Night, Elven Silver)$(if ($config.look -eq 'adaptive') { '   <- now' })"
    $keys = 'shire', 'redbook', 'starlit', 'elven'
    $names = 'Shire Morning (light, green)', 'Red Book (light, red)', 'Starlit Night (dark, gold)', 'Elven Silver (dark, silver)'
    for ($i = 0; $i -lt 4; $i++) { Write-Host ("   {0}  Always {1}{2}" -f ($i + 2), $names[$i], $(if ($config.look -eq $keys[$i]) { '   <- now' })) }
    Write-Host ""
    $ans = Read-Host "  Number"
    $Look = switch ($ans) { '1' { 'adaptive' } '2' { 'shire' } '3' { 'redbook' } '4' { 'starlit' } '5' { 'elven' } default { '' } }
    if (-not $Look) { exit 0 }
}
$config.look = $Look
$config | ConvertTo-Json | Set-Content $configFile -Encoding UTF8
if (Get-ScheduledTask -TaskPath '\Lembas\' -TaskName 'Lembas Sun' -ErrorAction SilentlyContinue) {
    Start-ScheduledTask -TaskPath '\Lembas\' -TaskName 'Lembas Sun'
} else {
    & (Join-Path $PSScriptRoot 'LembasSun.ps1') -Force -NoWindhawk
}
Write-Host "  Done." -ForegroundColor Gray
Start-Sleep -Seconds 1
