$out = "verify_result.txt"
$p = Get-Process rawimport-pipeline -ErrorAction SilentlyContinue
"proc_alive=$($null -ne $p)" | Set-Content $out -Encoding ascii
Get-ChildItem testdata\output -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { "OUT: $($_.Name) $($_.Length)" } | Add-Content $out -Encoding ascii
Get-ChildItem testdata\watch -File -ErrorAction SilentlyContinue | ForEach-Object { "WATCH: $($_.Name)" } | Add-Content $out -Encoding ascii
Get-Content native_out.log -ErrorAction SilentlyContinue | Select-String 'WIN2|converted|failed|duplicate|reconcile' | ForEach-Object { $_.Line } | Add-Content $out -Encoding ascii