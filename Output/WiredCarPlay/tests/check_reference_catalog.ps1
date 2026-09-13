<#
Developer-only reference check for the Wired CarPlay read-only catalog.

Parses the read-only CatPlay reference sources and compares them, entry by
entry, against Output/WiredCarPlay/include/wired_carplay/catalog.hpp:

  * every CSM packet message name and hexadecimal packet ID, plus its source
    family (source filename without .rs)
  * every implemented wire command name and direction
  * the commented wire command names declared in command_types.rs

The script is read-only and spawns no subprocesses. It is not required to build
the library. It exits 1 on any missing, extra or mismatched entry, and prints
numeric matched counts only after a full comparison.
#>
[CmdletBinding()]
param(
    [string] $ReferenceRoot
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ReferenceRoot)) {
    $ReferenceRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../Reference/CatPlaySource'))
}
else {
    $ReferenceRoot = [System.IO.Path]::GetFullPath($ReferenceRoot)
}

$headerPath = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../include/wired_carplay/catalog.hpp'))
$commandTypesPath = Join-Path $ReferenceRoot 'carplay/catplay_carplay/src/msg/commands/command_types.rs'
$csmMsgDir = Join-Path $ReferenceRoot 'core/catplay_csm/src/msg'

$problems = New-Object System.Collections.Generic.List[string]

function Add-Problem([string] $message) {
    $problems.Add($message)
}

# Reference names are case sensitive, and so are PowerShell string operators only
# when asked: use ordinal tables and the -c* operators throughout.
function New-OrdinalTable {
    return [System.Collections.Hashtable]::new([System.StringComparer]::Ordinal)
}

function Fail([string] $message) {
    Write-Output "FAILED: $message"
    exit 1
}

function Sort-Keys($table) {
    $keys = New-Object System.Collections.Generic.List[string]
    foreach ($key in $table.Keys) { $keys.Add([string] $key) }
    [string[]] $sorted = $keys.ToArray()
    [System.Array]::Sort($sorted, [System.StringComparer]::Ordinal)
    return $sorted
}

foreach ($required in @($headerPath, $commandTypesPath, $csmMsgDir)) {
    if (-not (Test-Path -LiteralPath $required)) {
        Fail "required path not found: $required"
    }
}

# ---------------------------------------------------------------------------
# Reference: CSM packet messages (name + hex packet ID + family)
# ---------------------------------------------------------------------------
$rustMessages = New-OrdinalTable
foreach ($file in (Get-ChildItem -LiteralPath $csmMsgDir -Filter '*.rs' -File | Sort-Object Name)) {
    $family = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
    $text = Get-Content -LiteralPath $file.FullName -Raw
    $matches = [regex]::Matches($text, 'packet_type!\s*\{\s*pub\s+struct\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*0x([0-9A-Fa-f]+)')
    foreach ($match in $matches) {
        $name = $match.Groups[1].Value
        $id = [Convert]::ToUInt32($match.Groups[2].Value, 16)
        if ($rustMessages.ContainsKey($name)) {
            Add-Problem "reference declares packet message '$name' more than once"
            continue
        }
        $rustMessages[$name] = [pscustomobject]@{ Name = $name; Id = $id; Family = $family }
    }
}

# ---------------------------------------------------------------------------
# Reference: wire commands (variant -> wire name, variant -> direction)
# ---------------------------------------------------------------------------
$commandTypesText = Get-Content -LiteralPath $commandTypesPath -Raw

$rustWireNames = New-OrdinalTable
foreach ($match in [regex]::Matches($commandTypesText, 'CommandType::([A-Za-z0-9_]+)\s*=>\s*"([^"]+)"')) {
    $variant = $match.Groups[1].Value
    if ($rustWireNames.ContainsKey($variant)) {
        Add-Problem "reference maps wire name for '$variant' more than once"
        continue
    }
    $rustWireNames[$variant] = $match.Groups[2].Value
}

$directionBlock = $commandTypesText
$directionStart = $commandTypesText.IndexOf('pub fn direction')
if ($directionStart -ge 0) {
    $directionBlock = $commandTypesText.Substring($directionStart)
}
else {
    Fail 'reference has no pub fn direction in command_types.rs'
}

$directionMap = New-OrdinalTable
$directionMap['FromDevice'] = 'DeviceToAccessory'
$directionMap['FromAccessory'] = 'AccessoryToDevice'
$directionMap['Bidirectional'] = 'Bidirectional'

$rustDirections = New-OrdinalTable
foreach ($match in [regex]::Matches($directionBlock, '(CommandType::[A-Za-z0-9_]+(?:\s*\|\s*CommandType::[A-Za-z0-9_]+)*)\s*=>\s*CommandDirection::([A-Za-z0-9_]+)')) {
    $variants = [regex]::Matches($match.Groups[1].Value, 'CommandType::([A-Za-z0-9_]+)') | ForEach-Object { $_.Groups[1].Value }
    $referenceDirection = $match.Groups[2].Value
    if (-not $directionMap.ContainsKey($referenceDirection)) {
        Fail "unknown reference direction '$referenceDirection' in command_types.rs"
    }
    foreach ($variant in $variants) {
        if ($rustDirections.ContainsKey($variant)) {
            Add-Problem "reference assigns a direction to '$variant' more than once"
            continue
        }
        $rustDirections[$variant] = $directionMap[$referenceDirection]
    }
}

# Commented wire command names declared inside the CommandType enum body.
$declaredExtensions = New-Object System.Collections.Generic.List[string]
$enumMatch = [regex]::Match($commandTypesText, '(?s)pub\s+enum\s+CommandType\s*\{(?<body>.*?)\r?\n\}')
if (-not $enumMatch.Success) {
    Fail 'could not locate the CommandType enum body in command_types.rs'
}
foreach ($match in [regex]::Matches($enumMatch.Groups['body'].Value, '(?m)^[ \t]*//[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t\r]*$')) {
    $declaredExtensions.Add($match.Groups[1].Value)
}

# ---------------------------------------------------------------------------
# Header catalog
# ---------------------------------------------------------------------------
$headerText = Get-Content -LiteralPath $headerPath -Raw

$headerEnumIds = New-OrdinalTable
$headerEnumOrder = New-Object System.Collections.Generic.List[string]
$csmEnumMatch = [regex]::Match($headerText, '(?s)enum\s+class\s+CsmType\s*:\s*std::uint16_t\s*\{(?<body>.*?)\r?\n\};')
if (-not $csmEnumMatch.Success) {
    Fail 'could not locate the CsmType enum body in catalog.hpp'
}
foreach ($match in [regex]::Matches($csmEnumMatch.Groups['body'].Value, '(?m)^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*0x([0-9A-Fa-f]+)')) {
    $name = $match.Groups[1].Value
    $id = [Convert]::ToUInt32($match.Groups[2].Value, 16)
    if ($headerEnumIds.ContainsKey($name)) {
        Add-Problem "header declares CsmType::$name more than once"
        continue
    }
    $headerEnumIds[$name] = $id
    $headerEnumOrder.Add($name)
}

$headerMessages = New-OrdinalTable
foreach ($match in [regex]::Matches($headerText, 'CsmDescriptor\{CsmType::([A-Za-z0-9_]+),\s*"([^"]+)",\s*"([^"]+)"\}')) {
    $enumerator = $match.Groups[1].Value
    $name = $match.Groups[2].Value
    $family = $match.Groups[3].Value
    if ($headerMessages.ContainsKey($enumerator)) {
        Add-Problem "header csm_catalog lists CsmType::$enumerator more than once"
        continue
    }
    $headerMessages[$enumerator] = [pscustomobject]@{ Name = $name; Family = $family }
}

$headerCommands = New-OrdinalTable
foreach ($match in [regex]::Matches($headerText, 'CommandDescriptor\{CommandType::([A-Za-z0-9_]+),\s*"([^"]+)",\s*Direction::([A-Za-z0-9_]+)\}')) {
    $variant = $match.Groups[1].Value
    if ($headerCommands.ContainsKey($variant)) {
        Add-Problem "header command_catalog lists CommandType::$variant more than once"
        continue
    }
    $headerCommands[$variant] = [pscustomobject]@{ WireName = $match.Groups[2].Value; Direction = $match.Groups[3].Value }
}

$headerExtensions = New-Object System.Collections.Generic.List[string]
$extensionMatch = [regex]::Match($headerText, '(?s)declared_command_extensions\s*\{([^}]*)\}')
if (-not $extensionMatch.Success) {
    Fail 'could not locate declared_command_extensions in catalog.hpp'
}
foreach ($match in [regex]::Matches($extensionMatch.Groups[1].Value, '"([^"]+)"')) {
    $headerExtensions.Add($match.Groups[1].Value)
}

# ---------------------------------------------------------------------------
# Compare: CSM messages, every ID individually
# ---------------------------------------------------------------------------
$messagesMatched = 0
foreach ($name in (Sort-Keys $rustMessages)) {
    if (-not $headerMessages.ContainsKey($name)) {
        Add-Problem "MISSING message: $($rustMessages[$name].Family)/$name 0x$('{0:X4}' -f $rustMessages[$name].Id)"
        continue
    }
    if (-not $headerEnumIds.ContainsKey($name)) {
        Add-Problem "MISSING message id: CsmType::$name is not declared in the header enum"
        continue
    }
    $expectedId = $rustMessages[$name].Id
    $actualId = $headerEnumIds[$name]
    if ($actualId -ne $expectedId) {
        Add-Problem ("MISMATCH message id: {0} reference 0x{1:X4} header 0x{2:X4}" -f $name, $expectedId, $actualId)
        continue
    }
    $headerName = $headerMessages[$name].Name
    if (-not ($headerName -ceq $name)) {
        Add-Problem "MISMATCH message name: CsmType::$name is catalogued as '$headerName'"
        continue
    }
    $headerFamily = $headerMessages[$name].Family
    if (-not ($headerFamily -ceq $rustMessages[$name].Family)) {
        Add-Problem "MISMATCH message family: $name reference '$($rustMessages[$name].Family)' header '$headerFamily'"
        continue
    }
    ++$messagesMatched
}

foreach ($name in (Sort-Keys $headerMessages)) {
    if (-not $rustMessages.ContainsKey($name)) {
        Add-Problem "EXTRA message: CsmType::$name ($($headerMessages[$name].Family)) has no reference packet_type struct"
    }
}

foreach ($name in $headerEnumOrder) {
    if (-not $rustMessages.ContainsKey($name)) {
        Add-Problem "EXTRA enum value: CsmType::$name has no reference packet_type struct"
    }
}

$referenceIdOwners = New-OrdinalTable
foreach ($name in $rustMessages.Keys) {
    $id = $rustMessages[$name].Id
    if ($referenceIdOwners.ContainsKey($id)) {
        Add-Problem ("DUPLICATE reference id: 0x{0:X4} used by both {1} and {2}" -f $id, $referenceIdOwners[$id], $name)
        continue
    }
    $referenceIdOwners[$id] = $name
}

$headerIdOwners = New-OrdinalTable
foreach ($name in $headerEnumIds.Keys) {
    $id = $headerEnumIds[$name]
    if ($headerIdOwners.ContainsKey($id)) {
        Add-Problem ("DUPLICATE header id: 0x{0:X4} used by both CsmType::{1} and CsmType::{2}" -f $id, $headerIdOwners[$id], $name)
        continue
    }
    $headerIdOwners[$id] = $name
}

# ---------------------------------------------------------------------------
# Compare: wire commands
# ---------------------------------------------------------------------------
$commandsMatched = 0
foreach ($variant in (Sort-Keys $rustWireNames)) {
    if (-not $headerCommands.ContainsKey($variant)) {
        Add-Problem "MISSING command: CommandType::$variant ('$($rustWireNames[$variant])')"
        continue
    }
    if (-not ($headerCommands[$variant].WireName -ceq $rustWireNames[$variant])) {
        Add-Problem "MISMATCH command wire name: $variant reference '$($rustWireNames[$variant])' header '$($headerCommands[$variant].WireName)'"
        continue
    }
    if (-not $rustDirections.ContainsKey($variant)) {
        Add-Problem "MISSING command direction: CommandType::$variant has no direction in command_types.rs"
        continue
    }
    if (-not ($headerCommands[$variant].Direction -ceq $rustDirections[$variant])) {
        Add-Problem "MISMATCH command direction: $variant reference '$($rustDirections[$variant])' header '$($headerCommands[$variant].Direction)'"
        continue
    }
    ++$commandsMatched
}

foreach ($variant in (Sort-Keys $headerCommands)) {
    if (-not $rustWireNames.ContainsKey($variant)) {
        Add-Problem "EXTRA command: CommandType::$variant has no wire name in command_types.rs"
    }
}

foreach ($variant in (Sort-Keys $rustDirections)) {
    if (-not $headerCommands.ContainsKey($variant)) {
        Add-Problem "MISSING command: CommandType::$variant has a direction but no catalog entry"
    }
}

$protocolDefinedCount = [regex]::Matches($headerText, 'CommandDescriptor\{CommandType::[A-Za-z0-9_]+,\s*"[^"]+",\s*Direction::ProtocolDefined\}').Count

# ---------------------------------------------------------------------------
# Compare: declared extensions
# ---------------------------------------------------------------------------
$extensionsMatched = 0
foreach ($extension in $declaredExtensions) {
    if ($headerExtensions -cnotcontains $extension) {
        Add-Problem "MISSING declared extension: $extension"
        continue
    }
    ++$extensionsMatched
}

foreach ($extension in $headerExtensions) {
    if ($declaredExtensions -cnotcontains $extension) {
        Add-Problem "EXTRA declared extension: $extension"
    }
    foreach ($variant in (Sort-Keys $headerCommands)) {
        if ($headerCommands[$variant].WireName -ceq $extension) {
            Add-Problem "declared extension '$extension' is also catalogued as an implemented CommandType::$variant"
        }
    }
}

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
Write-Output "reference root: $ReferenceRoot"
Write-Output "header:         $headerPath"
Write-Output "reference messages: $($rustMessages.Count)  header messages: $($headerMessages.Count)"
Write-Output "reference commands: $($rustWireNames.Count)  header commands: $($headerCommands.Count)"
Write-Output "reference declared extensions: $($declaredExtensions.Count)  header declared extensions: $($headerExtensions.Count)"
Write-Output "messages matched:   $messagesMatched/$($rustMessages.Count)"
Write-Output "commands matched:   $commandsMatched/$($rustWireNames.Count)"
Write-Output "extensions matched: $extensionsMatched/$($declaredExtensions.Count)"
Write-Output "Direction::ProtocolDefined entries: $protocolDefinedCount"

if ($problems.Count -gt 0) {
    Write-Output "problems: $($problems.Count)"
    foreach ($problem in $problems) {
        Write-Output "  $problem"
    }
    Write-Output 'catalog check FAILED'
    exit 1
}

if ($messagesMatched -ne $rustMessages.Count -or $commandsMatched -ne $rustWireNames.Count -or $extensionsMatched -ne $declaredExtensions.Count) {
    Write-Output 'catalog check FAILED: matched counts do not cover the reference'
    exit 1
}

Write-Output 'catalog check OK'
exit 0
