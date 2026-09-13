param(
 [string]$ImagePath,
 [int]$DiskNumber=2,
 [string]$ExpectedSerial='000000001536',
 [long]$ExpectedSize=62534975488,
 [string]$LogPath="$env:TEMP\zero2w-flash-mbr-last.log"
)
$ErrorActionPreference='Stop'
function Log([string]$m){$line="$(Get-Date -Format o) $m";Add-Content -LiteralPath $LogPath -Value $line;Write-Output $line}
function CheckTarget {
 $disk=Get-Disk -Number $DiskNumber
 $wmi=Get-CimInstance Win32_DiskDrive -Filter "Index=$DiskNumber"
 if($disk.BusType-ne'USB' -or $wmi.SerialNumber.Trim()-ne$ExpectedSerial -or $disk.Size-ne$ExpectedSize){throw 'TF card identity changed'}
}
function OpenRaw([IO.FileAccess]$access){
 New-Object IO.FileStream("\\.\PhysicalDrive$DiskNumber",[IO.FileMode]::Open,$access,[IO.FileShare]::ReadWrite,4194304,[IO.FileOptions]::WriteThrough)
}
Set-Content -LiteralPath $LogPath -Value ''
CheckTarget
$image=Get-Item -LiteralPath $ImagePath
$start=512L;$chunkSize=8388608
$buffer=New-Object byte[] $chunkSize
$input=[IO.File]::OpenRead($image.FullName);$raw=$null
try {
 $null=$input.Seek($start,[IO.SeekOrigin]::Begin);$offset=$start;$raw=OpenRaw ([IO.FileAccess]::ReadWrite);$null=$raw.Seek($offset,[IO.SeekOrigin]::Begin)
 Log "WRITE_START bytes=$($image.Length)"
 while($offset-lt$image.Length){
  $want=[int][Math]::Min([long]$chunkSize,$image.Length-$offset)
  $got=0;while($got-lt$want){$n=$input.Read($buffer,$got,$want-$got);if($n-le0){throw 'image EOF'};$got+=$n}
  $done=$false
  for($attempt=1;$attempt-le20-and-not$done;$attempt++){
   try{$raw.Write($buffer,0,$want);$done=$true}
   catch{Log "WRITE_REOPEN offset=$offset attempt=$attempt";try{$raw.Dispose()}catch{};$raw=$null;Start-Sleep -Seconds 2;CheckTarget;$raw=OpenRaw ([IO.FileAccess]::ReadWrite);$null=$raw.Seek($offset,[IO.SeekOrigin]::Begin)}
  }
  if(-not$done){throw "write failed at $offset"}
  $offset+=$want
  if((($offset-$start)%(67108864))-lt$chunkSize-or$offset-eq$image.Length){Log "WRITE $([Math]::Round(100*$offset/$image.Length,1))%"}
 }
 $raw.Flush($true);Log 'WRITE_PAYLOAD_OK'
}finally{if($null-ne$raw){$raw.Dispose()};$input.Dispose()}

$imageBuffer=New-Object byte[] $chunkSize;$diskBuffer=New-Object byte[] $chunkSize
$input=[IO.File]::OpenRead($image.FullName);$raw=$null
try {
 $null=$input.Seek($start,[IO.SeekOrigin]::Begin);$offset=$start;$raw=OpenRaw ([IO.FileAccess]::Read);$null=$raw.Seek($offset,[IO.SeekOrigin]::Begin)
 Log 'VERIFY_START'
 while($offset-lt$image.Length){
  $want=[int][Math]::Min([long]$chunkSize,$image.Length-$offset)
  $got=0;while($got-lt$want){$n=$input.Read($imageBuffer,$got,$want-$got);if($n-le0){throw 'image EOF'};$got+=$n}
  $done=$false
  for($attempt=1;$attempt-le20-and-not$done;$attempt++){
   try{$got=0;while($got-lt$want){$n=$raw.Read($diskBuffer,$got,$want-$got);if($n-le0){throw 'disk EOF'};$got+=$n};$done=$true}
   catch{Log "READ_REOPEN offset=$offset attempt=$attempt";try{$raw.Dispose()}catch{};$raw=$null;Start-Sleep -Seconds 2;CheckTarget;$raw=OpenRaw ([IO.FileAccess]::Read);$null=$raw.Seek($offset,[IO.SeekOrigin]::Begin)}
  }
  if(-not$done){throw "read failed at $offset"}
  $sha=[Security.Cryptography.SHA256]::Create();try{$ih=[BitConverter]::ToString($sha.ComputeHash($imageBuffer,0,$want));$dh=[BitConverter]::ToString($sha.ComputeHash($diskBuffer,0,$want))}finally{$sha.Dispose()}
  if($ih-ne$dh){throw "verify mismatch at $offset"}
  $offset+=$want
  if((($offset-$start)%(268435456))-lt$chunkSize-or$offset-eq$image.Length){Log "VERIFY $([Math]::Round(100*$offset/$image.Length,1))%"}
 }
 Log 'VERIFY_PAYLOAD_OK'
}finally{if($null-ne$raw){$raw.Dispose()};$input.Dispose()}

# Commit the MBR only after all remaining bytes have been written and verified.
$header=New-Object byte[] 512;$check=New-Object byte[] 512
$input=[IO.File]::OpenRead($image.FullName);try{$null=$input.Read($header,0,512)}finally{$input.Dispose()}
$raw=OpenRaw ([IO.FileAccess]::ReadWrite)
try{$raw.Write($header,0,512);$raw.Flush($true);$null=$raw.Seek(0,[IO.SeekOrigin]::Begin);$got=$raw.Read($check,0,512);if($got-ne512){throw 'MBR short read'};for($i=0;$i-lt512;$i++){if($header[$i]-ne$check[$i]){throw "MBR mismatch at $i"}}}finally{$raw.Dispose()}
Log 'FLASH_AND_VERIFY_OK'
