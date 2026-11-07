<#
  Lembas for Windows - follow the sun (the default), or keep one look all the time.
  Also switches Lembas Desktop (the moving scene and clock behind your icons) and its clock on or off.
#>
param([ValidateSet('', 'adaptive', 'shire', 'redbook', 'starlit', 'elven', 'lorien', 'havens')][string]$Look = '')

$Dest = Split-Path -Parent $PSScriptRoot
$configFile = Join-Path $Dest 'lembas.json'
$config = Get-Content $configFile -Raw | ConvertFrom-Json

if (-not $Look) {
    Write-Host ""
    Write-Host "  LEMBAS - choose a look" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "   1  Follow the sun (Shire Morning, Red Book or Lothlorien, Starlit Night, Elven Silver, the Grey Havens)$(if ($config.look -eq 'adaptive') { '   <- now' })"
    $keys = 'shire', 'redbook', 'starlit', 'elven', 'lorien', 'havens'
    $names = 'Shire Morning (light, green)', 'Red Book (light, red)', 'Starlit Night (dark, gold)', 'Elven Silver (dark, silver)', 'Lothlorien (dark, gold)', 'The Grey Havens (dark, grey-blue)'
    for ($i = 0; $i -lt 6; $i++) { Write-Host ("   {0}  Always {1}{2}" -f ($i + 2), $names[$i], $(if ($config.look -eq $keys[$i]) { '   <- now' })) }
    $living = -not ($config.PSObject.Properties['livingDesktop'] -and -not $config.livingDesktop)
    $clock = -not ($config.PSObject.Properties['desktopClock'] -and -not $config.desktopClock)
    Write-Host ""
    Write-Host ("   8  Moving desktop scene: {0}  (choose to turn it {1})" -f $(if ($living) { 'on' } else { 'off' }), $(if ($living) { 'off' } else { 'on' }))
    Write-Host ("   9  Clock on the desktop: {0}  (choose to turn it {1})" -f $(if ($clock) { 'on' } else { 'off' }), $(if ($clock) { 'off' } else { 'on' }))
    Write-Host ""
    $ans = Read-Host "  Number"
    if ($ans -eq '8' -or $ans -eq '9') {
        if ($ans -eq '8') { $config | Add-Member -NotePropertyName livingDesktop -NotePropertyValue (-not $living) -Force }
        else { $config | Add-Member -NotePropertyName desktopClock -NotePropertyValue (-not $clock) -Force }
        $config | ConvertTo-Json | Set-Content $configFile -Encoding UTF8
        $exe = Join-Path $Dest 'Living\LembasDesktop.exe'
        $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
        if (Test-Path $exe) {
            Start-Process $exe -ArgumentList '--quit' -Wait
            if ($config.livingDesktop) {
                Set-ItemProperty -Path $runKey -Name 'Lembas Desktop' -Value "`"$exe`""
                Start-Process $exe
            } else {
                Remove-ItemProperty -Path $runKey -Name 'Lembas Desktop' -ErrorAction SilentlyContinue
            }
        }
        Write-Host "  Done." -ForegroundColor Gray
        Start-Sleep -Seconds 1
        exit 0
    }
    $Look = switch ($ans) { '1' { 'adaptive' } '2' { 'shire' } '3' { 'redbook' } '4' { 'starlit' } '5' { 'elven' } '6' { 'lorien' } '7' { 'havens' } default { '' } }
    if (-not $Look) { exit 0 }
}
$config.look = $Look
$config | Add-Member -NotePropertyName keepUntil -NotePropertyValue 0 -Force   # chosen here: kept until changed
$config | ConvertTo-Json | Set-Content $configFile -Encoding UTF8
# tell your other devices (if you've signed in to sync)
if (Test-Path (Join-Path $env:APPDATA 'Lembas\google-token.dat')) { try { & (Join-Path $PSScriptRoot 'Lembas-Sync.ps1') -Quiet } catch { } }
if (Get-ScheduledTask -TaskPath '\Lembas\' -TaskName 'Lembas Sun' -ErrorAction SilentlyContinue) {
    Start-ScheduledTask -TaskPath '\Lembas\' -TaskName 'Lembas Sun'
} else {
    & (Join-Path $PSScriptRoot 'LembasSun.ps1') -Force -NoWindhawk
}
Write-Host "  Done." -ForegroundColor Gray
Start-Sleep -Seconds 1
