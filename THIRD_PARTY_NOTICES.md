# Licenses and third-party notices

Original code, tools and documentation in **esp32-socks5-gateway** are licensed
under the [MIT License](LICENSE), except where a file or the notices below state
otherwise. The project license grants rights only in material its contributors
can license. It does not replace an upstream license, remove an upstream
copyright, or license a third-party program obtained separately.

## Firmware dependencies

The table describes the pinned SDK and default ESP32-S3 N16R8 build. ESP-IDF
contains independently licensed components; “ESP-IDF is Apache-2.0” does not mean
every file in the SDK is Apache-2.0. Original upstream source headers remain
authoritative.

| Material | Version/source used | License and retained notices |
| --- | --- | --- |
| ESP-IDF original components and Espressif integration code | [fff9895c82d744c7237be8847347bdd1b07c6643](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643) | [Apache-2.0](LICENSES/Apache-2.0.txt); applicable per-file notices in [SDK-SOURCE-NOTICES.txt](LICENSES/SDK-SOURCE-NOTICES.txt) |
| esp-lwIP, including the local `ip4.c` override; BSD-licensed lwIP integration | [c6f2f878e7b0f86033214b85547d579be43351e3](https://github.com/espressif/esp-lwip/tree/c6f2f878e7b0f86033214b85547d579be43351e3) and the pinned SDK | [BSD-3-Clause](LICENSES/lwIP-BSD-3-Clause.txt), with additional per-file copyrights in SDK-SOURCE-NOTICES.txt |
| FreeRTOS kernel and ESP-IDF ports | Kernel 10.5.1, ESP-IDF SMP-modified sources in the pinned SDK | [MIT](LICENSES/FreeRTOS-MIT.txt); Amazon and other per-file copyrights retained in SDK-SOURCE-NOTICES.txt |
| cJSON | [espressif/cjson 1.7.19](https://components.espressif.com/components/espressif/cjson/versions/1.7.19), Dave Gamble and cJSON contributors | [MIT](LICENSES/cJSON-MIT.txt) |
| LED strip driver | [espressif/led_strip 3.0.3](https://components.espressif.com/components/espressif/led_strip/versions/3.0.3), Espressif Systems | [Apache-2.0](LICENSES/led_strip-Apache-2.0.txt) |
| Mbed TLS and TF-PSA-Crypto | [Espressif fork a2b32072ea898afc1ed5b6caf6931e36028c91d6](https://github.com/espressif/mbedtls/tree/a2b32072ea898afc1ed5b6caf6931e36028c91d6), including its TF-PSA-Crypto tree | Apache-2.0 **selected** from the upstream Apache-2.0 OR GPL-2.0-or-later offer; unabridged [Mbed TLS](LICENSES/MbedTLS-LICENSE.txt) and [TF-PSA-Crypto](LICENSES/TF-PSA-Crypto-LICENSE.txt) license files retained |
| Wi-Fi, PHY and coexistence binary libraries | `esp_wifi/lib` 7cd4d7b2b4acc38aead43b029ac9409c3eba9c54; `esp_phy/lib` 5695f4f38108658bc4a33e4712c1ebcb34911434; `esp_coex/lib` ddc56fa524a38df3098d2f36475fbe3eefe9b4f4 | Their supplied Apache-2.0 texts: [Wi-Fi](LICENSES/esp_wifi-LICENSE.txt), [PHY](LICENSES/esp_phy-LICENSE.txt), [coexistence](LICENSES/esp_coex-LICENSE.txt) |
| wpa_supplicant / hostapd integration | Pinned SDK; Jouni Malinen and contributors | [BSD-3-Clause terms and copyright](LICENSES/hostapd-BSD-3-Clause.txt), [upstream COPYING explanation](LICENSES/hostapd-COPYING.txt), plus per-file notices in SDK-SOURCE-NOTICES.txt |
| HTTP parser | Pinned SDK; Igor Sysoev, Joyent and Node contributors | [MIT-style license and copyright](LICENSES/http_parser-MIT.txt) |
| TLSF allocator | Pinned SDK; Matthew Conte | [BSD-3-Clause](LICENSES/TLSF-BSD-3-Clause.txt), with original SPDX notices in SDK-SOURCE-NOTICES.txt |
| Xtensa support and HAL | Pinned SDK; Cadence Design Systems and contributors | MIT and/or Apache-2.0 as identified in the original notices reproduced in SDK-SOURCE-NOTICES.txt |
| C library | Default `CONFIG_LIBC_PICOLIBC=y`; ESP-IDF toolchain `esp-15.2.0_20251204` | [Complete picolibc copyright/license collection](LICENSES/picolibc-COPYING.txt), including its Newlib-derived material; individual parts retain their own terms |
| GCC runtime and C++ runtime | Xtensa GCC 15.2.0, from the same toolchain | [GPL-3.0](LICENSES/GCC-GPL-3.0.txt) with the [GCC Runtime Library Exception 3.1](LICENSES/GCC-Runtime-Exception-3.1.txt) for covered runtime code |
| Mozilla CA certificate data | ESP-IDF `esp_crt_bundle/cacrt_all.pem`, dated 2026-02-11 in the pinned SDK | [MPL-2.0](LICENSES/MPL-2.0.txt); [original bundle header and source availability](LICENSES/CA-CERTIFICATES-NOTICE.txt) |

The source copyright collection reproduces initial comment notices from compiled
SDK sources in components represented by the default application link map, and
networking, allocator, FreeRTOS and Xtensa headers. It includes some optional
files from those components. It is a retained notice collection, not a complete
software bill of materials or a claim that every listed file is linked.
Separately copied upstream license texts are unmodified; their source paths and
SHA-256 hashes are recorded in [LICENSES/SOURCES.json](LICENSES/SOURCES.json).
Some optional lwIP PPP files carry additional historical per-file terms; PPP is
disabled in the default firmware. Their notices are retained in the collection
because it also covers optional source/header files. Review those terms before
enabling or redistributing additional components.

The GPL text in a dual-license offer does not change the Apache-2.0 choice above.
The compiler's own GPL license is also distinct from the runtime exception.
Covered runtime code compiled in an eligible compilation process can be combined
with independently licensed application code under the exception's terms.
The certificate-data license applies to that material, rather than independent
application files merely using it.

## Project lwIP changes

`patches/lwip-v6.1-transparent-tcp.patch` is a project modification (2026, noaic
and contributors). It adds IPv4 input/output hook declarations and call sites to
`lwip/src/core/ipv4/ip4.c` for transparent TCP rewriting. The copied and patched
upstream file retains its original header, copyright and BSD license. Project
additions do not remove or replace those terms. The source, pin and patch are
available in the project repository; `patches/lwip-v6.1.json` identifies the
original and patched file hashes. Other unmodified files copied by
`tools/prepare_lwip.py` keep their original notices.

## Xray configuration example and external programs

The adapted **`examples/xray-client-reality.json`** is distributed under
**CC BY-SA 4.0**, with upstream attribution and project changes documented in
[examples/LICENSE.md in the source repository](https://github.com/noaic/esp32-socks5-gateway/blob/main/examples/LICENSE.md). Its source is the official
XTLS/Xray-examples VLESS/TCP/XTLS Vision/REALITY client example. The
[complete CC BY-SA 4.0 terms](LICENSES/CC-BY-SA-4.0.txt) are included. Other
original fragments and explanatory text remain MIT unless separately identified.
This example is not compiled into the ESP32 firmware.

Xray-core is a separately installed upstream SOCKS5 server/client program; the
firmware communicates with it using SOCKS5 and does not bundle it. Xray-core has
its own [MPL-2.0 license](https://github.com/XTLS/Xray-core/blob/v26.3.27/LICENSE).
Its executable and `geoip.dat` / `geosite.dat` assets are not supplied here.
If redistributing them, retain and review their respective upstream notices.

Python, Git, CMake, Ninja, ESP-IDF host tools and esptool also have their own
licenses. They are installed separately and are not included in the firmware
ZIP. The optional project flashing helper stays in the source repository's
`tools/` directory and is not included in firmware downloads. It calls the user's
installed esptool and does not incorporate the esptool implementation. Redistributing a toolchain
or host-tool installer requires its own license review.

## Redistribution and maintenance

When distributing firmware, including standalone BIN downloads, accompany it
with `LICENSE`, this file and the `LICENSES/` directory (also included in the
script-free firmware ZIP). When distributing source, also retain copyright,
license and modification notices in the original files and preserve the
example's attribution. The packaging tool includes the notice bundle. Official binary-only Releases
also provide this notice bundle in GitHub's generated source archives; their
release notes link to the notices at the matching tag.

This inventory was checked against the pinned local SDK, managed-component
versions and default firmware link map. It is not a historical, line-by-line
provenance audit of every contribution. Changes to the SDK, linked components,
toolchain, certificate bundle or copied examples require reviewing and updating
these notices; optional components can introduce additional obligations. Merely
adding this project MIT license does not relicense third-party material.
