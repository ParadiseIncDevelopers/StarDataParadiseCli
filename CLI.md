# StarDataParadiseCLI

`--host` DNS adı veya IPv4/IPv6 adresi kabul eder. Varsayılan uç nokta `127.0.0.1:7443`. `--user` ve `--ca` gerekli. `--ca` sunucuyu doğrulayan PEM dosyasıdır. Ağ adresi sertifika adından farklıysa `--tls-name` kullan. `--timeout-ms` varsayılan olarak 5000 ms ve 100–120000 ms aralığındadır.

~~~powershell
.\StarDataParadiseCLI.exe --host localhost --user local-admin --ca .\ca.pem --status
~~~

`--profile FILE` UTF-8 JSON okur. Desteklenen alanlar `host`, `port`, `user`, `ca_file`, `tls_name`, `timeout_ms` ve `production`. Göreli CA yolu profil dizininden çözülür. Komut satırı seçenekleri profili geçersiz kılabilir. `--prod` veya profildeki `production: true`, terminal oturumunda `PROD` etiketi gösterir. Parola konsolda yankısız okunur. `--password-stdin` ile ilk UTF-8 satır parola olarak kullanılır. Profil ve seçenekler parola alanı kabul etmez.

## Etkileşim ve işlem

- SQL ifadesini noktalı virgülle bitir. Alıntı, yorum ve PL/SQL blokları çok satırlı giriş olarak işlenir. Terminal istemi `main.public` bağlamını, oturum kimliğini, transaction durumunu ve `DEV` veya `PROD` profil etiketini gösterir.
- Tab, yazılan öneke göre SQL anahtar kelimelerini ve terminal komutlarını tamamlar. Ctrl+R ve oklar oturum geçmişinde önek arar. `\history [PREFIX]`, bu oturumun kaydedilmiş geçmişini gösterir. Basit SELECT ve seçili salt okunur komutlar bellekte tutulur. Alıntılı SQL, parola girişi ve yönetim talepleri geçmişe alınmaz.
- `\databases` ve `\tables` erişilebilir katalog nesnelerini listeler. `\describe NAME` nesne ve sütun bilgilerini verir. `\format table` veya `\format vertical` görünümü değiştirir. `\timing on` her ifadenin tamamlanma süresini stderr’e yazar. `\run FILE` geçerli bağlantıyla UTF-8 dosya çalıştırır.
- `\begin`, `\savepoint NAME`, `\rollback [NAME]`, `\release NAME` ve `\commit` gerçek Server transaction’larına karşılık gelir. `\autocommit off` ilk SQL ifadesinde transaction açar. `\autocommit on` ve `\connect [PROFILE]` öncesinde açık transaction’ı bitir. Girdi bittiğinde açık transaction geri alınır ve oturum başarılı işlem durumu döndürmez.
- `\connect [PROFILE]` yeni bağlantı açar. `\status` ve `\capabilities` bağlantı ile sunucu özelliklerini gösterir. `\quit` oturumu bitirir.

## Otomasyon

PowerShell 7.3+ örneği. CI parolası ilk stdin satırından iletilir.

~~~powershell
$env:SDP_PASSWORD | .\StarDataParadiseCLI.exe --profile .\lab.json --password-stdin --file .\verify.sql --format json --transaction single --stop-on-error --output .\result.json
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Get-Content .\result.json -Raw | ConvertFrom-Json
~~~

Windows üzerinde POSIX shell örneği.

~~~sh
printf '%s\n' "$SDP_PASSWORD" | ./StarDataParadiseCLI.exe \
  --profile ./lab.json --password-stdin --format ndjson \
  --command 'SELECT id, amount FROM records WHERE id=:wanted' \
  --bind 'wanted={"t":"i64","v":"42"}'
~~~

`--file FILE`, `--command SQL` veya stdin kullanılabilir. `--password-stdin` ile stdin’de parola satırını SQL izler. Bir parametreyi `--bind NAME=JSON` ile gönder. Değer için wire v1 tipli nesne kullan. NULL girdisinin SQL tipini de belirt. Örnek `name={"type":{"type":"TEXT"},"value":{"t":"null"}}`. SQL metni ve parametreler ayrı iletilir.

`--transaction each` varsayılan davranıştır. Normal yazma ifadeleri kendi transaction’ında yapılır. `--transaction single` tüm girdi için tek transaction açar, hata durumunda geri alır ve açık transaction komutlarını reddeder. Server’ın bazı yönetim işlemleri yalnız transaction dışında çalışır. `--stop-on-error` ilk hatada durur. Devam eden betikler hata görmüşse sonunda sıfır dışı kod verir. `--output FILE` veri çıktısını UTF-8 olarak dosyaya yazar. Mesajlar, zamanlama ve SQL kaynak konumu stderr’e gider.

| Biçim | Veri gösterimi |
| --- | --- |
| JSON | Tek nesnede `events` dizisi ve `exit_code`. Her olay `type` ve `data` alanlarını içerir. |
| NDJSON | Satır başına bir olay. Son `status` olayında `exit_code` vardır. |
| CSV | Sonuç kümesi başına kolon başlığı ve satırlar. NULL tırnaksız `\N`, metin tırnaklı, DECIMAL tam ondalık metin, ikili değer `0x` ile başlar. |
| Table | Sekmeyle ayrılan sütunlar. |
| Vertical | Her satırda alan adları ayrı satırlarda gösterilir. |

JSON/NDJSON, sunucu wire değerlerini korur. `i64` ve DECIMAL katsayısı dize olarak taşınır. DECIMAL tipi `unscaled` ve `scale`, binary tipi `hex`, timestamp tipi saniye, nanosaniye ve offset alanları içerir. NULL ayrı tip etiketiyle temsil edilir. Sonuçlar `batch` olayları halinde gelir. `--json`, SQL girdi kaynağı olmadan önceki tek bağlantı durum nesnesini üretir.

## Yönetim

`\rpc GROUP ACTION [JSON]`, paketlenmiş Native SDK’nın ilgili işlemini çağırır. Server, her işlem için oturum yetkisini denetler.

| Grup | İşlemler |
| --- | --- |
| `sessions` | `list`, `reset`, `cancel`, `terminate` |
| `history` | `log`, `read`, `diff`, `verify`, `status`, `retain`, `purge`, `pin`, `unpin`, `restore`, `revert` |
| `decision` | `create`, `validate`, `activate`, `test`, `explain`, `whynot`, `whatif`, `recheck` |
| `job` | `submit`, `status`, `watch`, `cancel`, `schedule-create`, `schedule-status`, `schedule-cancel`, `capabilities` |
| `dataset` | `create`, `get`, `delete`, `plsql-submit` |
| `model` | `create`, `status`, `train`, `evaluate`, `predict`, `activate`, `compare`, `checkpoint`, `resume`, `deactivate`, `rollback`, `delete`, `devices` |
| `backup` | `submit`, `status`, `cancel`, `catalog`, `verify`, `restore`, `delete` |
| `admin` | `status`, `health`, `health-check`, `config-preview`, `config-apply`, `support-preview`, `support-create`, `maintenance-submit` |

~~~text
\rpc history log {"limit":5}
\rpc job submit {"kind":"compute","count":10000}
\rpc job watch {"job_id":"<submit cevabındaki kimlik>"}
\rpc backup catalog {}
\rpc admin health {}
~~~

`\rpc history restore` ve `revert` önizleme planı, onu üreten oturumda uygulanır. NDJSON oturumunda `plan_id` okunduktan sonra ikinci komutu aynı stdin akışına gönder. Yazma isteği gerektirdiğinde CLI işlem tokenı üretir. Kullanıcı ve rol tanımları SQL ile yönetilir. `\password [USER]` bağlı konsolda parola değiştirir veya yetkili aktör için başka kullanıcının parolasını sıfırlar.

`\service status|start|stop NAME`, yerel Windows Service Control Manager üzerinden adı `StarDataParadise` ile başlayan servisi sorgular veya durumunu değiştirir. Windows servis izinleri geçerlidir. Ağdan servis kontrolü yapılmaz.

Ctrl+C devam eden isteği veya job izlemeyi keser. Sunucu işi, izleyen CLI kapandıktan sonra çalışmaya devam eder. `job cancel` iptal isteği gönderir. `job watch` son durumu doğrular. Yazma ya da COMMIT yanıtı kaybolduğunda CLI yazmayı tekrarlamaz. Hata bir işlem tokenı içeriyorsa yeni bağlantıda `\outcome TOKEN` ile kayıt sorgulanabilir.

| Kod | Anlamı |
| ---: | --- |
| 0 | Başarılı |
| 2 | Geçersiz seçenek, profil veya girdi |
| 3 | Kimlik doğrulama veya yetki hatası |
| 4 | Bağlantı, TLS, timeout veya protokol hatası |
| 5 | SQL veya işlem hatası |
| 6 | İptal |
| 7 | İşlem sonucu belirsiz |
