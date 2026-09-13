param(
    [Parameter(Mandatory = $true)][string]$ImagePath,
    [int]$DiskNumber = 2,
    [string]$ExpectedSerial = '000000001536',
    [long]$ExpectedSize = 62534975488,
    [string]$LogPath = "$env:TEMP\zero2w-flash.log"
)
$ErrorActionPreference = 'Stop'
function Log([string]$Message) { Add-Content -LiteralPath $LogPath -Value "$(Get-Date -Format o) $Message" }
Set-Content -LiteralPath $LogPath -Value "$(Get-Date -Format o) START"
try {
    $principal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Administrator privileges required.'}
    $image=Get-Item -LiteralPath $ImagePath
    $disk=Get-Disk -Number $DiskNumber
    if($disk.BusType-ne'USB'){throw "Disk $DiskNumber is not USB."}
    if($disk.SerialNumber.Trim()-ne$ExpectedSerial){throw "Serial mismatch: $($disk.SerialNumber)."}
    if($disk.Size-ne$ExpectedSize){throw "Size mismatch: $($disk.Size)."}
    if($image.Length-le 0-or$image.Length-ge$disk.Size){throw 'Invalid image size.'}

    $bufferSize=4194304
    $imageBuffer=New-Object byte[] $bufferSize
    $diskBuffer=New-Object byte[] $bufferSize
    $input=[IO.File]::Open($image.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $devicePath="\\.\PhysicalDrive$DiskNumber"
    $output=New-Object IO.FileStream($devicePath,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite,$bufferSize,[IO.FileOptions]::WriteThrough)
    try {
        $null=$output.Seek(0,[IO.SeekOrigin]::Begin)
        $written=0L;$nextReport=67108864L;$started=Get-Date
        while(($count=$input.Read($imageBuffer,0,$imageBuffer.Length))-gt 0){
            $output.Write($imageBuffer,0,$count);$written+=$count
            if($written-ge$nextReport-or$written-eq$image.Length){
                $elapsed=[Math]::Max(((Get-Date)-$started).TotalSeconds,0.001)
                Log "WRITE $([Math]::Round(100*$written/$image.Length,1))% $written/$($image.Length) $([Math]::Round($written/1MB/$elapsed,1))MiB/s"
                $nextReport+=67108864L
            }
        }
        $output.Flush($true)

        # Verify while the same raw-device handle remains open, before Windows can mount or alter it.
        Log 'VERIFY started'
        $null=$input.Seek(0,[IO.SeekOrigin]::Begin);$null=$output.Seek(0,[IO.SeekOrigin]::Begin)
        $hi=[Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
        $hd=[Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
        try {
            $remaining=$image.Length;$verified=0L;$nextVerify=268435456L
            while($remaining-gt 0){
                $want=[int][Math]::Min([long]$imageBuffer.Length,$remaining)
                $got=0;while($got-lt$want){$n=$input.Read($imageBuffer,$got,$want-$got);if($n-le 0){throw 'image EOF'};$got+=$n}
                $got=0;while($got-lt$want){$n=$output.Read($diskBuffer,$got,$want-$got);if($n-le 0){throw 'disk EOF'};$got+=$n}
                $hi.AppendData($imageBuffer,0,$want);$hd.AppendData($diskBuffer,0,$want)
                $remaining-=$want;$verified+=$want
                if($verified-ge$nextVerify-or$verified-eq$image.Length){Log "VERIFY $([Math]::Round(100*$verified/$image.Length,1))%";$nextVerify+=268435456L}
            }
            $imageHash=([BitConverter]::ToString($hi.GetHashAndReset())).Replace('-','').ToLowerInvariant()
            $diskHash=([BitConverter]::ToString($hd.GetHashAndReset())).Replace('-','').ToLowerInvariant()
            Log "IMAGE_SHA256 $imageHash";Log "DISK_SHA256  $diskHash"
            if($imageHash-ne$diskHash){throw 'SHA-256 mismatch.'}
        } finally {$hi.Dispose();$hd.Dispose()}
    } finally {$input.Dispose();$output.Dispose()}
    Log 'FLASH_OK'
} catch { Log "FLASH_FAILED $($_.Exception.Message)"; throw }
