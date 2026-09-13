param([string]$ImagePath,[int]$DiskNumber=2,[string]$LogPath="$env:TEMP\zero2w-flash-resilient.log")
$ErrorActionPreference='Stop'
function Log([string]$m){Add-Content -LiteralPath $LogPath -Value "$(Get-Date -Format o) $m"}
function OpenDisk([IO.FileAccess]$access){
  $path="\\.\PhysicalDrive$DiskNumber"
  return New-Object IO.FileStream($path,[IO.FileMode]::Open,$access,[IO.FileShare]::ReadWrite,4194304,[IO.FileOptions]::WriteThrough)
}
Set-Content -LiteralPath $LogPath -Value "$(Get-Date -Format o) START"
$image=Get-Item -LiteralPath $ImagePath
$disk=Get-Disk -Number $DiskNumber
if($disk.BusType-ne'USB'-or$disk.SerialNumber.Trim()-ne'000000001536'-or$disk.Size-ne62534975488){throw 'TF card identity check failed.'}
$chunkSize=16777216
$buffer=New-Object byte[] $chunkSize
$input=[IO.File]::OpenRead($image.FullName)
try {
 # Keep the partition table blank while writing the payload. This prevents
 # Windows from auto-discovering partitions and invalidating the raw handle.
 $offset=4194304L;$null=$input.Seek($offset,[IO.SeekOrigin]::Begin)
 while($offset-lt$image.Length){
  $want=[int][Math]::Min([long]$chunkSize,$image.Length-$offset)
  $got=0;while($got-lt$want){$n=$input.Read($buffer,$got,$want-$got);if($n-le 0){throw 'image EOF'};$got+=$n}
  $done=$false
  for($attempt=1;$attempt-le 20-and-not$done;$attempt++){
   $raw=$null
   try{
    $raw=OpenDisk ([IO.FileAccess]::ReadWrite);$null=$raw.Seek($offset,[IO.SeekOrigin]::Begin);$raw.Write($buffer,0,$want);$raw.Flush($true);$done=$true
   }catch{Log "WRITE_RETRY offset=$offset attempt=$attempt $($_.Exception.Message)";Start-Sleep -Seconds 2}
   finally{if($null-ne$raw){try{$raw.Dispose()}catch{}}}
  }
  if(-not$done){throw "write failed at $offset"}
  $offset+=$want
  if(($offset%(67108864))-eq0-or$offset-eq$image.Length){Log "WRITE $([Math]::Round(100*$offset/$image.Length,1))% $offset/$($image.Length)"}
 }
}finally{$input.Dispose()}

Log 'VERIFY started'
$imageBuffer=New-Object byte[] $chunkSize;$diskBuffer=New-Object byte[] $chunkSize
$input=[IO.File]::OpenRead($image.FullName)
try{
 $offset=4194304L;$null=$input.Seek($offset,[IO.SeekOrigin]::Begin)
 while($offset-lt$image.Length){
  $want=[int][Math]::Min([long]$chunkSize,$image.Length-$offset)
  $got=0;while($got-lt$want){$n=$input.Read($imageBuffer,$got,$want-$got);if($n-le 0){throw 'image EOF'};$got+=$n}
  $done=$false
  for($attempt=1;$attempt-le20-and-not$done;$attempt++){
   $raw=$null
   try{
    $raw=OpenDisk ([IO.FileAccess]::Read);$null=$raw.Seek($offset,[IO.SeekOrigin]::Begin);$got=0
    while($got-lt$want){$n=$raw.Read($diskBuffer,$got,$want-$got);if($n-le0){throw 'disk EOF'};$got+=$n}
    $done=$true
   }catch{Log "READ_RETRY offset=$offset attempt=$attempt $($_.Exception.Message)";Start-Sleep -Seconds 2}
   finally{if($null-ne$raw){try{$raw.Dispose()}catch{}}}
  }
  if(-not$done){throw "read failed at $offset"}
  $sha=[Security.Cryptography.SHA256]::Create()
  try{$ih=([BitConverter]::ToString($sha.ComputeHash($imageBuffer,0,$want))).Replace('-','');$dh=([BitConverter]::ToString($sha.ComputeHash($diskBuffer,0,$want))).Replace('-','')}finally{$sha.Dispose()}
  if($ih-ne$dh){throw "verify mismatch at offset $offset"}
  $offset+=$want
  if(($offset%(268435456))-eq0-or$offset-eq$image.Length){Log "VERIFY $([Math]::Round(100*$offset/$image.Length,1))%"}
 }
}finally{$input.Dispose()}

# Write and immediately verify the first 4 MiB (MBR/bootloader) last.
$header=New-Object byte[] 4194304;$check=New-Object byte[] 4194304
$input=[IO.File]::OpenRead($image.FullName)
try{$got=0;while($got-lt$header.Length){$n=$input.Read($header,$got,$header.Length-$got);if($n-le0){throw 'header EOF'};$got+=$n}}finally{$input.Dispose()}
$done=$false
for($attempt=1;$attempt-le20-and-not$done;$attempt++){
 $raw=$null
 try{
  $raw=OpenDisk ([IO.FileAccess]::ReadWrite);$null=$raw.Seek(0,[IO.SeekOrigin]::Begin);$raw.Write($header,0,$header.Length);$raw.Flush($true)
  $null=$raw.Seek(0,[IO.SeekOrigin]::Begin);$got=0;while($got-lt$check.Length){$n=$raw.Read($check,$got,$check.Length-$got);if($n-le0){throw 'header read EOF'};$got+=$n}
  $sha=[Security.Cryptography.SHA256]::Create();try{$ih=([BitConverter]::ToString($sha.ComputeHash($header))).Replace('-','');$dh=([BitConverter]::ToString($sha.ComputeHash($check))).Replace('-','')}finally{$sha.Dispose()}
  if($ih-ne$dh){throw 'header verify mismatch'};$done=$true
 }catch{Log "HEADER_RETRY attempt=$attempt $($_.Exception.Message)";Start-Sleep -Seconds 2}
 finally{if($null-ne$raw){try{$raw.Dispose()}catch{}}}
}
if(-not$done){throw 'header write failed'}
Log 'HEADER_OK'
Log 'FLASH_OK'
