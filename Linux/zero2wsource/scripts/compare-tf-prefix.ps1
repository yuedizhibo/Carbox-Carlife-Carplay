param([string]$ImagePath,[int]$DiskNumber=2,[int]$Length=1048576)
$ErrorActionPreference='Stop'
$a=New-Object byte[] $Length
$b=New-Object byte[] $Length
$i=[IO.File]::OpenRead($ImagePath)
$d=New-Object IO.FileStream("\\.\PhysicalDrive$DiskNumber",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
try { $null=$i.Read($a,0,$Length); $null=$d.Read($b,0,$Length) } finally { $i.Dispose();$d.Dispose() }
$found=0
$total=0
for($x=0;$x -lt $Length;$x++) { if($a[$x]-ne $b[$x]) { $total++; if($found-lt 20){"offset=$x image=$($a[$x]) disk=$($b[$x])";$found++} } }
"prefix_mismatch_count=$total"
