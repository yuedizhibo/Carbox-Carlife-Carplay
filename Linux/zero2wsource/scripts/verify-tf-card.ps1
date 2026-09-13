param(
    [Parameter(Mandatory = $true)][string]$ImagePath,
    [int]$DiskNumber = 2,
    [string]$LogPath = "$env:TEMP\zero2w-verify.log"
)
$ErrorActionPreference = 'Stop'
function Log([string]$Message) { Add-Content -LiteralPath $LogPath -Value "$(Get-Date -Format o) $Message" }
Set-Content -LiteralPath $LogPath -Value "$(Get-Date -Format o) VERIFY_START"
$image = Get-Item -LiteralPath $ImagePath
$imageSha = [System.Security.Cryptography.SHA256]::Create()
$imageStream = [System.IO.File]::OpenRead($image.FullName)
try {
    $imageHash = ([BitConverter]::ToString($imageSha.ComputeHash($imageStream))).Replace('-', '').ToLowerInvariant()
}
finally {
    $imageStream.Dispose(); $imageSha.Dispose()
}
Log "IMAGE_SHA256 $imageHash"
$devicePath = "\\.\PhysicalDrive$DiskNumber"
$stream = New-Object System.IO.FileStream($devicePath,[System.IO.FileMode]::Open,[System.IO.FileAccess]::Read,[System.IO.FileShare]::ReadWrite,4194304)
$hash = [System.Security.Cryptography.IncrementalHash]::CreateHash([System.Security.Cryptography.HashAlgorithmName]::SHA256)
$buffer = New-Object byte[] 4194304
$readTotal = 0L
$nextReport = 268435456L
try {
    while ($readTotal -lt $image.Length) {
        $wanted = [int][Math]::Min([long]$buffer.Length, $image.Length - $readTotal)
        $count = $stream.Read($buffer,0,$wanted)
        if ($count -le 0) { throw 'Unexpected end of TF card.' }
        $hash.AppendData($buffer,0,$count)
        $readTotal += $count
        if ($readTotal -ge $nextReport -or $readTotal -eq $image.Length) {
            Log "READ $([Math]::Round(100*$readTotal/$image.Length,1))% $readTotal/$($image.Length)"
            $nextReport += 268435456L
        }
    }
    $diskHash = ([BitConverter]::ToString($hash.GetHashAndReset())).Replace('-', '').ToLowerInvariant()
    Log "DISK_SHA256  $diskHash"
    if ($diskHash -ne $imageHash) { throw 'SHA-256 mismatch.' }
    Log 'VERIFY_OK'
}
catch {
    Log "VERIFY_FAILED $($_.Exception.Message)"
    throw
}
finally {
    $stream.Dispose(); $hash.Dispose()
}
