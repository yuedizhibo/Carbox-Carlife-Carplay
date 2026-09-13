param([string]$ImagePath,[int]$DiskNumber=2)
$ErrorActionPreference='Stop'
$offset=4194304L;$length=1048576
$buffer=New-Object byte[] $length
$i=[IO.File]::OpenRead($ImagePath)
$d=New-Object IO.FileStream("\\.\PhysicalDrive$DiskNumber",[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite,4194304,[IO.FileOptions]::WriteThrough)
try {
 $null=$i.Seek($offset,[IO.SeekOrigin]::Begin);$got=0;while($got-lt$length){$n=$i.Read($buffer,$got,$length-$got);if($n-le 0){throw 'image EOF'};$got+=$n}
 $null=$d.Seek($offset,[IO.SeekOrigin]::Begin);$d.Write($buffer,0,$length);$d.Flush($true)
 $check=New-Object byte[] $length;$null=$d.Seek($offset,[IO.SeekOrigin]::Begin);$got=0;while($got-lt$length){$n=$d.Read($check,$got,$length-$got);if($n-le 0){throw 'disk EOF'};$got+=$n}
 for($x=0;$x-lt$length;$x++){if($buffer[$x]-ne$check[$x]){throw "verify mismatch at $($offset+$x)"}}
 'BOOT_PREFIX_REPAIR_OK'
} finally {$i.Dispose();$d.Dispose()}
