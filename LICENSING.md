# CLI licensing / Lisans kapsamı

| Component | Applicable terms |
| --- | --- |
| Independent wire v1 client in `StarDataParadiseCli/` and original support files outside the exclusions below | [StarDataParadise CLI Free-Use License 1.0](licenses/StarDataParadise-CLI-Free-Use.txt) |
| `compatibility/upstream/`, including `shell-26.10.0.patch` and original MySQL Shell integration contributions | GPL version 2, applicable upstream additional permissions and [upstream notices](compatibility/upstream/LICENSE.mysql-shell.txt) |
| Native SDK and other dependencies | Their separate licenses and notices |

The original wire client and MySQL Shell are distinct programs. The Free-Use
License permits no-cost personal and commercial use and redistribution of
the unmodified official wire-client binary with its notices. It does not
grant general source modification or redistribution rights, and is not an
open-source license.

Those proprietary terms never restrict MySQL Shell, the GPL-covered patch
or its covered derivative works. Renaming `mysqlsh.exe` does not change
its license. The full [GPL version 2 text](licenses/GPL-2.0-only.txt) is included;
original StarDataParadise integration contributions in the designated GPL
scope use that version. Separately identified third-party terms remain intact.

For copyright-controlled StarDataParadise contributions in the identified GPL
scope, the [additional linking permission](licenses/StarDataParadise-GPL-Linking-Exception.txt)
expressly preserves linking with the separately licensed software covered by
the corresponding upstream release's permission. It grants rights only in
those original contributions and does not change third-party terms.

## Upstream source and distribution

The patch targets MySQL Shell 26.10.0 at
[1161d07bad16a57cc728a9bbdb8575f3577483b4](https://github.com/mysql/mysql-shell/tree/1161d07bad16a57cc728a9bbdb8575f3577483b4).
The exact upstream [LICENSE](https://github.com/mysql/mysql-shell/blob/1161d07bad16a57cc728a9bbdb8575f3577483b4/LICENSE)
has been copied verbatim to this repository, including its additional
permissions and third-party notices. Those permissions apply only within
their stated scope. The source patch is unchanged.

When distributing GPL-covered binaries, satisfy the applicable source
requirements for the exact distributed build, including modifications
and required build material. Preserve all upstream and dependency notices;
an upstream link alone is not a substitute for those obligations.

Neither this scope notice nor the Free-Use License grants trademark,
endorsement, Cloud or separate Enterprise access rights. Previously
granted licenses are not reduced. This document is not a complete binary
distribution compliance audit.

## Türkçe özet

Özgün wire v1 CLI, değiştirilmeyen resmî ikililer için ücretsiz kişisel ve
ticari kullanıma izin veren özel lisans altındadır. Kaynak koduna genel
değiştirme veya yeniden dağıtım izni verilmez. MySQL Shell yaması ve ondan
türeyen Classic CLI ise GPLv2 ve mevcut upstream izinleri kapsamındadır.
