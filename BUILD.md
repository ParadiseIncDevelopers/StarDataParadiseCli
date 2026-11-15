# StarDataParadiseCLI 0.2.0

Windows x64, Visual Studio 2026 (v145), Windows SDK 10.0.26100.0, C++20 ve CMake 4.2.2 kullanılır. CLI ve Native DLL aynı dizinde çalışır.

~~~powershell
cmake --preset windows-x64 -DSDP_NATIVE_ARCHIVE="C:/packages/StarDataParadiseDLLler-0.4.0-win-x64.zip"
cmake --build --preset windows-x64
ctest --preset windows-x64
~~~

CMake ve Visual Studio, Native SDK 0.4.0 x64 ZIP’i için SHA-256 değerini doğrular. Beklenen değer `4ee15b8fa50c94c5ee012245d8ed8e3c16259df64fc154ca15fb286cb6a8ad25`. CMake, paketin bildirdiği 0.4.0 sürümünü de doğrular. `SDP_JSON_ARCHIVE` çevrimdışı nlohmann/json 3.12.0 arşivini kabul eder. Yerel CI `ci.ps1 -NativeArchive <zip> -JsonArchive <archive>` ile çalışır.

Visual Studio çözümü için `SDP_NATIVE_ARCHIVE` ile çıkarılmış JSON arşiv kökü `SDP_JSON_ROOT` ortam değişkenlerini ayarla. x64 platformunu seç. CMake Release çıktıları `out/build/windows-x64/Release/` dizinindedir.

Gerçek TLS testlerini etkinleştirmek için `SDP_TEST_SERVER` ile Server 0.4.0 EXE ve `SDP_TEST_OPENSSL` ile OpenSSL EXE yollarını ver. Python 3.12 testleri paketten CLI ve DLL’yi ayrı istemci dizinine alır. Server test başına loopback adresi, yeni geçici sertifika, rastgele kimlik bilgileri ve ayrı veri dizini kullanır. Ürün çağrıları, test çıktıları ve hata senaryoları CTest sonucuna yansır.

CMake BSD-3-Clause, nlohmann/json MIT lisanslıdır. Native paketin lisansları ZIP’in `share` dizinindedir. [CLI kullanımı ve çıkış kodları](CLI.md)
