param([string]$NativeArchive = $env:SDP_NATIVE_ARCHIVE, [string]$JsonArchive, [string]$TestServer, [string]$TestOpenSsl)

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $configure = @('--preset', 'windows-x64')
    if ($NativeArchive) { $configure += "-DSDP_NATIVE_ARCHIVE=$NativeArchive" }
    if ($JsonArchive) { $configure += "-DSDP_JSON_ARCHIVE=$JsonArchive" }
    if ($TestServer) { $configure += "-DSDP_TEST_SERVER=$TestServer" }
    if ($TestOpenSsl) { $configure += "-DSDP_TEST_OPENSSL=$TestOpenSsl" }
    & cmake @configure
    if ($LASTEXITCODE) { exit $LASTEXITCODE }

    & cmake --build --preset windows-x64
    if ($LASTEXITCODE) { exit $LASTEXITCODE }

    & ctest --preset windows-x64
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
