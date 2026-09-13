param([int]$DiskNumber=2,[string]$OutputPath)
$ErrorActionPreference='Stop'
$disk=Get-Disk -Number $DiskNumber
$wmi=Get-CimInstance Win32_DiskDrive -Filter "Index=$DiskNumber"
if($disk.BusType-ne'USB' -or $wmi.SerialNumber.Trim()-ne'000000001536' -or $disk.Size-ne62534975488){throw 'TF identity mismatch'}
$sourceOffset=272629760L;$length=2613051392L
$src=New-Object IO.FileStream("\\.\PhysicalDrive$DiskNumber",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
$dst=New-Object IO.FileStream($OutputPath,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None)
$dst.SetLength($length)
$buf=New-Object byte[] 8388608
try {
 $null=$src.Seek($sourceOffset,[IO.SeekOrigin]::Begin);$left=$length;$done=0L
 while($left-gt0){$want=[int][Math]::Min([long]$buf.Length,$left);$n=$src.Read($buf,0,$want);if($n-le0){throw 'short read'};$dst.Write($buf,0,$n);$left-=$n;$done+=$n;if(($done%268435456)-eq0){"READ $([Math]::Round(100*$done/$length,1))%"}}
 $dst.Flush($true);'ROOT_PARTITION_COPIED'
} finally {$src.Dispose();$dst.Dispose()}
