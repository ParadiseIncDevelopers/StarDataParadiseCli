# StarDataParadiseCLI derleme ve test

Windows x64, Visual Studio 2026 (v145), Windows SDK 10.0.26100.0, C++20 ve CMake 4.2.2 kullanılır. CLI ve native DLL uygulamanın aynı dizininde çalışır.

```powershell
cmake --preset windows-x64 -DSDP_NATIVE_ARCHIVE="C:/packages/StarDataParadiseDLLler-0.1.0-win-x64.zip"
cmake --build --preset windows-x64
ctest --preset windows-x64
```

`SDP_NATIVE_ARCHIVE` Etap 05 Release ZIP’idir. Derleme SHA-256 `9a9e7409c3670e38c1a54bca1bcdc2b9cf9e3f68c87286500c745bdbea1cfd4c` değerini ve CMake paket sürümü 0.1.0’ı doğrular. `SDP_JSON_ARCHIVE` çevrimdışı nlohmann/json 3.12.0 arşivini kabul eder. Yerel CI `ci.ps1 -NativeArchive <zip> -JsonArchive <archive>` ile çalışır.

Visual Studio çözümü için `SDP_NATIVE_ARCHIVE` ve çıkarılmış JSON arşiv kökü `SDP_JSON_ROOT` ortam değişkenlerini ayarla. x64 platformunu kullan. CMake çıktıları `out/build/windows-x64/Release/` dizinindedir.

Yerel TLS entegrasyon testini etkinleştirmek için yapılandırmaya `SDP_TEST_SERVER` ile Server EXE ve `SDP_TEST_OPENSSL` ile OpenSSL EXE yollarını ver. Python 3.12+ testi istemci dizinine yalnız CLI ve DLL’yi kopyalar, ayrı loopback Server başlatır ve geçici sertifika üretir.

CMake BSD-3-Clause, nlohmann/json MIT lisanslıdır. Native paketin lisansları ZIP’in `share` dizinindedir. Kullanım ve çıkış kodları [CLI.md](CLI.md) dosyasında.
