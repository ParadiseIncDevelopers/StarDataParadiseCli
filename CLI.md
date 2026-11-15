# StarDataParadiseCLI bağlantısı

`--host` DNS adı veya düz IPv4/IPv6 adresidir ve varsayılanı `127.0.0.1`’dir. Port varsayılanı 7443. `--user` ve `--ca` gerekli. CA, sunucu sertifikasını doğrulayan yerel PEM dosyasıdır. `--tls-name` yalnız sertifikada doğrulanması gereken ad bağlantı adresinden farklıysa kullanılır. `--timeout-ms` varsayılan 5000 ms ve aralık 100–120000 ms.

```powershell
.\StarDataParadiseCLI.exe --host localhost --port 7443 --user local-admin --ca .\ca.pem --status
```

`--profile FILE` UTF-8 JSON okur. Desteklenen alanlar `host`, `port`, `user`, `ca_file`, `tls_name` ve `timeout_ms`. Göreli CA yolu profilin bulunduğu dizinden çözülür. Komut satırı seçenekleri profili geçersiz kılabilir. Parola konsolda yankısız okunur. Otomasyon `--password-stdin` ile ilk UTF-8 giriş satırını güvenli şekilde yönlendirir. Parola bir komut satırı seçeneği veya profil alanı olarak kabul edilmez.

Etkileşimli komutlar `\connect [PROFILE]`, `\status`, `\capabilities`, `\help` ve `\quit`. `--json` tek durum nesnesi üretir ve işlemi bitirir. Log ve hata metinleri stderr’e gider. `--color auto` yalnız konsolda renk verir ve `NO_COLOR` değerini dikkate alır. `always` ve `never` renk seçimini açıkça belirler. JSON çıktısında ANSI bulunmaz. Terminalde uzak metnin kontrol baytları kaçış biçiminde gösterilir.

Ctrl+C terminal oturumunu kapatır. Devam eden health çağrısı iptal edilir. İlk native bağlantı açma işlemi yapılandırılmış süre sınırına kadar sürebilir. EOF oturumu kapatır ve son komutun sonucu çıkış koduna yansır.

| Kod | Anlamı |
| ---: | --- |
| 0 | Başarılı |
| 2 | Geçersiz seçenek, profil veya girdi |
| 3 | Kimlik doğrulama veya yetki hatası |
| 4 | Bağlantı, sertifika, timeout veya protokol hatası |
| 5 | SQL hatası veya SQL özelliği unavailable |
| 6 | İptal |
| 7 | İşlem sonucu belirsiz |

SQL yürütme sonraki etaplarda sunulur. Bu sürüm Server’ın capability bayraklarını gösterir.
