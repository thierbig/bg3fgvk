Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000} -MaxEvents 20 |
  Where-Object { $_.Message -match 'bg3' } |
  Select-Object -First 5 |
  ForEach-Object {
    $l = $_.Message -split "`n"
    "TIME: $($_.TimeCreated)"
    $l | Select-String -Pattern 'Faulting (application|module) (name|path)|Exception code' | ForEach-Object { $_.Line.Trim() }
    "----"
  }
