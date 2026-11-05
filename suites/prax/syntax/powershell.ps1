# Removes log files older than a number of days, and reports what it freed.
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path $_ -PathType Container })]
    [string] $Path,

    [ValidateRange(1, 365)]
    [int] $Days = 30,

    [switch] $Recurse
)

$ErrorActionPreference = 'Stop'
$cutoff = (Get-Date).AddDays(-$Days)

function Format-Size {
    param([long] $Bytes)
    switch ($Bytes) {
        { $_ -ge 1GB } { return '{0:N1} GB' -f ($Bytes / 1GB) }
        { $_ -ge 1MB } { return '{0:N1} MB' -f ($Bytes / 1MB) }
        default { return "$Bytes bytes" }
    }
}

$old = Get-ChildItem -Path $Path -Filter '*.log' -File -Recurse:$Recurse |
    Where-Object { $_.LastWriteTime -lt $cutoff }

$freed = 0L
foreach ($file in $old) {
    if ($PSCmdlet.ShouldProcess($file.FullName, 'Remove')) {
        try {
            $freed += $file.Length
            Remove-Item -LiteralPath $file.FullName
        }
        catch {
            Write-Warning "could not remove $($file.Name): $_"
        }
    }
}

[pscustomobject]@{
    Removed = $old.Count
    Freed   = Format-Size $freed
    Before  = $cutoff.ToString('yyyy-MM-dd')
} | Format-Table -AutoSize
