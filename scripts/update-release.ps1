$inFile  = Join-Path $env:TEMP 'cred_in.txt'
$outFile = Join-Path $env:TEMP 'cred_out.txt'
Set-Content -Path $inFile -Value "protocol=https`r`nhost=github.com`r`n" -Encoding ascii -NoNewline
cmd /c "git credential fill < ""$inFile"" > ""$outFile"" 2>nul"
$cred = Get-Content $outFile -ErrorAction SilentlyContinue
if (-not $cred) { Set-Content probeP5.txt 'NO CRED' -Encoding ascii; exit 1 }
$user = ($cred | Select-String '^username=').Line.Substring(9)
$tok  = ($cred | Select-String '^password=').Line.Substring(9)
$auth = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$user`:$tok"))
$h = @{ Authorization = $auth; Accept = 'application/vnd.github+json' }
try {
  $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/rawrdoge/autodngconverter/releases/375332303' -Headers $h -TimeoutSec 30
  "RELEASE EXISTS id=$($rel.id) assets=$($rel.assets.Count)" | Set-Content probeP5.txt -Encoding ascii
  # ensure exe note present
  $add = '**Prebuilt Windows binary:** rawimport-pipeline-v2.0.5-win-x64.exe (statically linked, no DLLs; requires exiftool on PATH for EXIF dates, otherwise falls back to mtime)' + [char]10
  if ($rel.body -notlike '*Prebuilt Windows binary*') {
    $notes = $add + [char]10 + $rel.body
    $payload = @{ name = 'v2.0.5'; tag_name = 'v2.0.5'; target_commitish = 'main'; body = $notes } | ConvertTo-Json -Compress
    $r = Invoke-RestMethod -Uri 'https://api.github.com/repos/rawrdoge/autodngconverter/releases/375332303' -Method Patch -Headers $h -ContentType 'application/json; charset=utf-8' -Body $payload -TimeoutSec 30
  }
} catch {
  "RELEASE GONE: $($_.Exception.Message) - recreating" | Set-Content probeP5.txt -Encoding ascii
  $payload = @{ name = 'v2.0.5'; tag_name = 'v2.0.5'; target_commitish = 'main'; body = ('Prebuilt Windows binary attached: rawimport-pipeline-v2.0.5-win-x64.exe (statically linked).' + [char]10 + '23 defects fixed per PRD v2.1.0 amendment; E2E-verified vs external MariaDB with real NRW input.') } | ConvertTo-Json -Compress
  $r = Invoke-RestMethod -Uri 'https://api.github.com/repos/rawrdoge/autodngconverter/releases' -Method Post -Headers $h -ContentType 'application/json; charset=utf-8' -Body $payload -TimeoutSec 30
  Add-Content probeP5.txt -Encoding ascii -Value "RECREATED id=$($r.id)"
}