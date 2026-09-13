param([string]$ImagePath,[int]$DiskNumber=2,[long]$Offset=0,[int]$Length=268435456)
$ErrorActionPreference='Stop'
$image=(Get-Item -LiteralPath $ImagePath)
$actual=[int][Math]::Min([long]$Length,$image.Length-$Offset)
$bi=New-Object byte[] 4194304;$bd=New-Object byte[] 4194304
$hi=[Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
$hd=[Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
$i=[IO.File]::OpenRead($ImagePath)
$d=New-Object IO.FileStream("\\.\PhysicalDrive$DiskNumber",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite,4194304)
try {
 $null=$i.Seek($Offset,[IO.SeekOrigin]::Begin);$null=$d.Seek($Offset,[IO.SeekOrigin]::Begin)
 $remaining=[long]$actual
 while($remaining-gt 0){
   $want=[int][Math]::Min([long]$bi.Length,$remaining)
   $got=0;while($got-lt$want){$n=$i.Read($bi,$got,$want-$got);if($n-le 0){throw 'image EOF'};$got+=$n}
   $got=0;while($got-lt$want){$n=$d.Read($bd,$got,$want-$got);if($n-le 0){throw 'disk EOF'};$got+=$n}
   $hi.AppendData($bi,0,$want);$hd.AppendData($bd,0,$want);$remaining-=$want
 }
 $ih=([BitConverter]::ToString($hi.GetHashAndReset())).Replace('-','');$dh=([BitConverter]::ToString($hd.GetHashAndReset())).Replace('-','')
 "offset=$Offset length=$actual match=$($ih-eq$dh) image=$ih disk=$dh"
} finally {$i.Dispose();$d.Dispose();$hi.Dispose();$hd.Dispose()}
