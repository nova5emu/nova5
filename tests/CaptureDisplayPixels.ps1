param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\_Build\nova5\sos-display.png'),
    [switch]$MetadataOnly
)

# Capture the PRIMARY display in physical pixels. A DPI-unaware Screen.Bounds
# passed to CopyFromScreen can capture only the upper-left part of the display.
# This changes only this script thread's DPI context, restored in finally.
$ErrorActionPreference = 'Stop'
if (-not ('Nova5.DisplayPixels' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Nova5 {
    public static class DisplayPixels {
        [DllImport("user32.dll")]
        public static extern int GetSystemMetrics(int index);
        [DllImport("user32.dll", SetLastError = true)]
        public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    }
}
'@
}

$logicalWidth = [Nova5.DisplayPixels]::GetSystemMetrics(0)
$logicalHeight = [Nova5.DisplayPixels]::GetSystemMetrics(1)
$previous = [Nova5.DisplayPixels]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
if ($previous -eq [IntPtr]::Zero) {
    throw "SetThreadDpiAwarenessContext failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}
$bitmap = $null
$graphics = $null
try {
    $pixelWidth = [Nova5.DisplayPixels]::GetSystemMetrics(0)
    $pixelHeight = [Nova5.DisplayPixels]::GetSystemMetrics(1)
    if ($pixelWidth -le 0 -or $pixelHeight -le 0) { throw 'Invalid primary display extent' }
    $result = [ordered]@{
        display = 'primary'
        before_dpi_context = @($logicalWidth, $logicalHeight)
        physical_pixels = @($pixelWidth, $pixelHeight)
        ratio = @(($pixelWidth / [double]$logicalWidth), ($pixelHeight / [double]$logicalHeight))
        captured = $false
    }
    if (-not $MetadataOnly) {
        Add-Type -AssemblyName System.Drawing
        $bitmap = New-Object System.Drawing.Bitmap $pixelWidth, $pixelHeight
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        # The primary display starts at (0,0). Both the source extent and the
        # destination bitmap are physical pixels; no resizing or crop follows.
        $graphics.CopyFromScreen(0, 0, 0, 0, $bitmap.Size)
        $resolvedOutput = [IO.Path]::GetFullPath($OutputPath)
        $bitmap.Save($resolvedOutput, [System.Drawing.Imaging.ImageFormat]::Png)
        $result.captured = $true
        $result.output = $resolvedOutput
    }
    [pscustomobject]$result | ConvertTo-Json -Compress
} finally {
    if ($graphics) { $graphics.Dispose() }
    if ($bitmap) { $bitmap.Dispose() }
    [void][Nova5.DisplayPixels]::SetThreadDpiAwarenessContext($previous)
}
