<#
  Lembas for Windows - see all four looks now, about 25 seconds each, then back to the right one.
  Everything switches, as it will through the day: wallpaper, light/dark mode, accent colour,
  taskbar, Start, File Explorer and the rest.
#>
param([int]$Seconds = 25)

$Dest = Split-Path -Parent $PSScriptRoot
$preview = Join-Path $Dest 'preview.json'
$looks = Get-Content (Join-Path $Dest 'looks.json') -Raw | ConvertFrom-Json
$task = Get-ScheduledTask -TaskPath '\Lembas\' -TaskName 'Lembas Sun' -ErrorAction SilentlyContinue

function Show-Look([string]$look) {
    if ($look) { @{ look = $look; until = (Get-Date).AddMinutes(10).ToString('s') } | ConvertTo-Json | Set-Content $preview -Encoding UTF8 }
    else { Remove-Item $preview -Force -ErrorAction SilentlyContinue }
    if ($task) {
        # The task has the rights to change the Windhawk styles too
        Start-ScheduledTask -TaskPath '\Lembas\' -TaskName 'Lembas Sun'
        for ($i = 0; $i -lt 40 -and (Get-ScheduledTask -TaskPath '\Lembas\' -TaskName 'Lembas Sun').State -eq 'Running'; $i++) { Start-Sleep -Milliseconds 500 }
    } else {
        & (Join-Path $PSScriptRoot 'LembasSun.ps1') -Force -NoWindhawk
    }
}

Write-Host ""
Write-Host "  LEMBAS - a day in four looks" -ForegroundColor Yellow
Write-Host "  Open Start, File Explorer or the notification centre while it runs." -ForegroundColor DarkGray
Write-Host ""
try {
    foreach ($k in 'shire', 'redbook', 'starlit', 'elven') {
        Write-Host ("  {0}" -f $looks.$k.name) -ForegroundColor $(if ($looks.$k.light) { 'DarkYellow' } else { 'Cyan' })
        Show-Look $k
        Start-Sleep -Seconds $Seconds
    }
} finally {
    Write-Host ""
    Write-Host "  Back to the look for right now." -ForegroundColor Gray
    Show-Look ''
}
Start-Sleep -Seconds 2
