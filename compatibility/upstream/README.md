# StarDataParadise CLI 0.3.0 önizlemesi

`shell-26.10.0.patch`, markalı giriş ve durum ekranını, uyumluluk komutlarını ve Windows derleme değişikliklerini içerir. Kaynak tabanı `mysql/mysql-shell` deposunun `26.10.0` etiketi ve `1161d07bad16a57cc728a9bbdb8575f3577483b4` commit’idir.

```powershell
git apply --check C:/src/StarDataParadiseCli/compatibility/upstream/shell-26.10.0.patch
git apply C:/src/StarDataParadiseCli/compatibility/upstream/shell-26.10.0.patch
```

Yamayı ayrı bir upstream çalışma ağacına uygulayın. Windows yapılandırmasında eşleşen MySQL 26.10.0 kaynak ve derleme dizinlerini `MYSQL_SOURCE_DIR` ve `MYSQL_BUILD_DIR` ile verin. Yerleşik Python için 3.12 geliştirme dosyalarını, JavaScript için 26.10.0 ile ABI uyumlu JIT Executor başlıklarını, import kitaplığını ve çalışma zamanı DLL’ini kullanın. Yerel Windows kabulünde Oracle imzalı 26.10.0 JIT DLL’i kullanıldı.

Canlı kabul yeni Server üzerinde Classic TLS, CRUD, `ANALYZE`, savepoint/rollback, `\capabilities`, `\databases`, `\tables`, JavaScript ve Python sorgularını kapsadı. Bu etiket kaynak önizlemesine aittir.
