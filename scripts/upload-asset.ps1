$ErrorActionPreference = 'Continue'
$log = 'E:\dev\asset_upload.log'
"=== run $(Get-Date) ===" | Set-Content $log -Encoding ascii

$inFile  = 'E:\dev\cred_in.txt'
$outFile = 'E:\dev\cred_out.txt'
Set-Content -Path $inFile -Value "protocol=https`r`nhost=github.com`r`n" -Encoding ascii -NoNewline
cmd /c "git credential fill < ""$inFile"" > ""$outFile"" 2>nul"
$cred = Get-Content $outFile -ErrorAction SilentlyContinue
"user lines: $($cred.Count)" | Add-Content $log -Encoding ascii
if (-not $cred) { "NO CRED" | Add-Content $log -Encoding ascii; exit 1 }
$user = ($cred | Select-String '^username=').Line.Substring(9)
$tok  = ($cred | Select-String '^password=').Line.Substring(9)
$auth = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$user`:$tok"))
$h = @{ Authorization = $auth; Accept = 'application/vnd.github+json' }

try {
  $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/rawrdoge/autodngconverter/releases/375332303' -Headers $h -TimeoutSec 30
  "release id=$($rel.id) assets=$($rel.assets.Count)" | Add-Content $log -Encoding ascii
  foreach ($a in $rel.assets) {
    if ($a.name -eq 'rawimport-pipeline-v2.0.5-win-x64.exe') {
      Invoke-RestMethod -Uri "https://api.github.com/repos/rawrdoge/autodngconverter/releases/assets/$($a.id)" -Method Delete -Headers $h -TimeoutSec 30
      "deleted old asset $($a.id)" | Add-Content $log -Encoding ascii
    }
  }
  curl.exe -sSL -X POST -H "Authorization: $auth" -H "Content-Type: application/octet-stream" --data-binary "@E:\Jonathan\Documents\git\autodngconverter\build-win\Release\rawimport-pipeline.exe" "https://uploads.github.com/repos/rawrdoge/autodngconverter/releases/375332303/assets?name=rawimport-pipeline-v2.0.5-win-x64.exe" | Add-Content $log -Encoding ascii
  "DONE" | Add-Content $log -Encoding ascii
} catch {
  "ERROR: $($_.Exception.Message)" | Add-Content $log -Encoding ascii
}