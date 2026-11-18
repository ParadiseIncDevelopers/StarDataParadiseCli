# StarDataParadise CLI

StarDataParadise sunucusuna terminalden bağlanmak, SQL betikleri çalıştırmak ve yönetim işlerini otomatikleştirmek için Windows x64 istemcileri.

| İstemci | Protokol | Dağıtım |
| --- | --- | --- |
| `StarDataParadiseCLI.exe` | TLS üzerinden wire v1 | Bu deponun CMake / Visual Studio projesi |
| Markalı `mysqlsh.exe` | MySQL Classic TCP | MySQL Shell 26.10.0 kaynak önizlemesi ve Windows yapılandırması. [Derleme kılavuzu](compatibility/upstream/README.md) |

## Wire v1 ile ilk bağlantı

Bir [StarDataParadise Server](https://github.com/ParadiseIncDevelopers/StarDataParadiseServer) örneğinin adresini, kullanıcı adını ve CA PEM dosyasını kullanın. Örnekte sertifikanın adı `localhost`, dinleyici portu `7443`. Parola konsolda yankısız okunur. Sertifika adı ile ağ adresi farklıysa `--tls-name` seçeneğini ekleyin.

```powershell
.\StarDataParadiseCLI.exe --host localhost --port 7443 --user local-admin --ca C:\SDP\lab\ca.pem --status
.\StarDataParadiseCLI.exe --host localhost --port 7443 --user local-admin --ca C:\SDP\lab\ca.pem
```

Etkileşimli oturumda `\capabilities` sunucu özelliklerini, `\tables` görülebilen tabloları listeler. `\run FILE` UTF-8 SQL betiğini geçerli oturumda çalıştırır. `\quit` oturumu kapatır.

## Otomasyon

PowerShell 7.3+ örneği bir SQL dosyasını tek transaction’da çalıştırır ve satır başına JSON olayı üretir. `lab.json` dosyasında bağlantı ayarlarını, işlem ortamında `SDP_PASSWORD` değerini sağlayın.

```powershell
$env:SDP_PASSWORD | .\StarDataParadiseCLI.exe --profile .\lab.json --password-stdin --file .\verify.sql --transaction single --stop-on-error --format ndjson --output .\result.ndjson
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

Komut tek ifadeyi `--command` ile, SQL parametrelerini ayrı `--bind` seçenekleriyle alabilir. `\rpc`, ilgili sunucu yeteneği ve erişim izni varsa History, Decision, iş/takvim, model, yedek ve yönetim çağrılarını yapar. Yazma veya commit sonucu belirsizse hata kodu `7` döner. Token varsa aynı kullanıcıyla yeni oturumda `\outcome TOKEN` ile sonucu sorgulayın. [Profil şeması, çıktı biçimleri, komutlar ve çıkış kodları](CLI.md)

## Wire v1 istemcisini derleme

CMake 4.2.2, Visual Studio 2026 C++ (v145), Windows SDK 10.0.26100.0 ve sabit Native SDK **0.4.0** Windows x64 ZIP’i gerekir.

```powershell
cmake --preset windows-x64 -DSDP_NATIVE_ARCHIVE="C:/packages/StarDataParadiseDLLler-0.4.0-win-x64.zip"
cmake --build --preset windows-x64
ctest --preset windows-x64
.\out\build\windows-x64\Release\StarDataParadiseCLI.exe --help
```

Native DLL, EXE ile aynı dizinde çalışır. Canlı TLS entegrasyon testleri için `SDP_TEST_SERVER` ve `SDP_TEST_OPENSSL` yollarını verin. [Derleme, bağımlılık doğrulaması ve testler](BUILD.md)

## Classic TCP önizlemesi

Yamalı Shell `--mysql` ile Classic TCP kullanır ve JavaScript ile Python kiplerini sunar. Kendi Classic dinleyicinizin portunu ve CA dosyasını kullanın.

```powershell
.\mysqlsh.exe --mysql --sql --host=localhost --port=3306 --user=operator --ssl-mode=VERIFY_IDENTITY --ssl-ca=C:/SDP/lab/mysql-ca.pem
```

[Shell kaynak önizlemesi](compatibility/upstream/README.md) · [MySQL tabanlı Server uyumluluk kılavuzu](https://github.com/ParadiseIncDevelopers/StarDataParadiseServer/tree/master/compatibility) · [Sürüm notları](https://github.com/ParadiseIncDevelopers/StarDataParadiseCli/releases)
