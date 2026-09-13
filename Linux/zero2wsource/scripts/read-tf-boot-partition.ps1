param([int]$DiskNumber=2,[string]$OutputPath)
$ErrorActionPreference='Stop'
$disk=Get-Disk -Number $DiskNumber
$wmi=Get-CimInstance Win32_DiskDrive -Filter "Index=$DiskNumber"
if($disk.BusType-ne'USB' -or $wmi.SerialNumber.Trim()-ne'000000001536' -or $disk.Size-ne62534975488){throw 'TF identity mismatch'}
$src=New-Object IO.FileStream("\\.\PhysicalDrive$DiskNumber",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
$dst=[IO.File]::Create($OutputPath)
$buf=New-Object byte[] 4194304
try {
 $null=$src.Seek(4194304,[IO.SeekOrigin]::Begin)
 $left=268435456L
 while($left-gt0){$want=[int][Math]::Min($buf.Length,$left);$n=$src.Read($buf,0,$want);if($n-le0){throw 'short read'};$dst.Write($buf,0,$n);$left-=$n}
 $dst.Flush($true)
 'BOOT_PARTITION_COPIED'
} finally {$src.Dispose();$dst.Dispose()}
