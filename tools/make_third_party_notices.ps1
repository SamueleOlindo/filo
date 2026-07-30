# Assembles THIRD-PARTY-NOTICES.txt for a BINARY release.
#
# This is an obligation, not a courtesy. Filo statically links code under BSD-3,
# BSD-2 and MIT, and every one of those licences requires the copyright notice
# and the licence text to travel with a redistribution IN BINARY FORM — not only
# with the source. A published Filo.exe without this file is a licence breach,
# quietly, whatever the readme says.
#
# Two of the components are not vendored in third_party/: PDFium and llama.cpp
# are built separately and linked in, so their licences are read from the same
# roots the build uses.
#
#   powershell -ExecutionPolicy Bypass -File tools\make_third_party_notices.ps1
#
# Attach the result to the GitHub release next to the executable.

$root   = Split-Path -Parent $PSScriptRoot
$output = Join-Path $root 'THIRD-PARTY-NOTICES.txt'

$pdfium = if ($env:PDFIUM_ROOT) { $env:PDFIUM_ROOT } else { '' }
$llama  = if ($env:LLAMA_ROOT)  { $env:LLAMA_ROOT }  else { '' }

$rule = '-' * 78

$parts = @()
$parts += @"
THIRD-PARTY NOTICES FOR FILO

Filo is MIT licensed; see LICENSE. The executable also contains code from the
projects below, each under its own licence, reproduced here in full as those
licences require.

$rule
"@

function Add-Notice($name, $summary, $path) {
    $script:parts += "`n$name`n$summary`n"
    if ($path -and (Test-Path $path)) {
        $script:parts += (Get-Content $path -Raw).TrimEnd()
    } else {
        $script:parts += "  [licence text not found at: $path]"
        Write-Warning "missing licence for $name ($path)"
    }
    $script:parts += "`n$rule"
}

# Vendored, in third_party/.
$parts += "`nSQLite`nPublic domain. The authors dedicate it to the public domain and ask only:`n"
$parts += @"
    May you do good and not evil.
    May you find forgiveness for yourself and forgive others.
    May you share freely, never taking more than you give.
"@
$parts += "`n$rule"

Add-Notice 'Snowball stemmers' 'BSD 3-Clause.' (Join-Path $root 'third_party\snowball\COPYING')
Add-Notice 'libdeflate' 'MIT. Decompression path only.' (Join-Path $root 'third_party\libdeflate\COPYING')
Add-Notice 'xxHash' 'BSD 2-Clause.' (Join-Path $root 'third_party\xxhash\LICENSE')

# Built separately and linked in, so read from where the build found them.
if ($llama) {
    Add-Notice 'llama.cpp and ggml' 'MIT. Present only in builds made with LLAMA_ROOT set.' `
               (Join-Path $llama 'llama.cpp\LICENSE')
}
if ($pdfium) {
    Add-Notice 'PDFium' 'BSD 3-Clause. Present only in builds made with PDFIUM_ROOT set.' `
               (Join-Path $pdfium 'LICENSE')
}

$parts += @"

Microsoft Edge WebView2
The WebView2 SDK headers and the static loader are redistributed under the
Microsoft WebView2 SDK licence terms, which permit distribution as part of an
application. The WebView2 RUNTIME is not distributed with Filo: it is a
component of Windows.

$rule

Models
No model is distributed with Filo. GGUF files are obtained by the user and
carry their own licences. See docs/models.md.
"@

# UTF-8 without a BOM: a notices file gets opened in Notepad, in a browser and
# by whatever a package auditor uses, and a BOM shows up as stray characters in
# at least one of them.
[System.IO.File]::WriteAllText($output, ($parts -join "`n"),
                               (New-Object System.Text.UTF8Encoding $false))

Write-Output "wrote $output ($((Get-Item $output).Length) bytes)"
if (-not $llama)  { Write-Warning 'LLAMA_ROOT not set: llama.cpp notice omitted' }
if (-not $pdfium) { Write-Warning 'PDFIUM_ROOT not set: PDFium notice omitted' }
