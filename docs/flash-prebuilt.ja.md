# ESP-IDF を使わずにビルド済みファームウェアを書き込む

[English](flash-prebuilt.md) · [简体中文](flash-prebuilt.zh-CN.md) · [日本語](flash-prebuilt.ja.md)

Windows 11 または Linux から、Espressif の公式ツールで **ESP32-S3 N16R8** に書き込めます。Git、CMake、Ninja、ESP-IDF、クロスコンパイラー、プロジェクト独自の書き込みスクリプトは不要です。Windows には Python が不要な公式 GUI もあります。コンパイル時の既定値を変える場合は[ソースビルドガイド](build-and-flash.ja.md)を使います。

## 1. 適切なイメージを選ぶ

正式な[プロジェクト Release](https://github.com/noaic/esp32-socks5-gateway/releases)は `vYY.MM.DD` タグを使い、BIN を 2 つだけ提供します。名前は `esp32-socks5-gateway-vYY.MM.DD-esp32s3-n16r8-{factory,sysupgrade}.bin` です。Actions は開発検証パッケージを提供し、Release は自動公開しません。リリースがなければソースからビルドします。

| ファイル | 用途 |
| --- | --- |
| `*-factory.bin` | 初回導入 / 初期化用の完全イメージ。ブートローダー + パーティション表 + アプリを **`0x0`** に書き込む。**保存したネットワーク・管理設定を初期化** |
| `*-sysupgrade.bin` | アプリだけの更新。**`0x10000`** に書き込む。既存のブートローダー、配置、設定バージョンの互換性が必要 |

完全イメージはビルド結果から結合し、**設定済みデバイスの 16 MB Flash ダンプではありません**。アプリ末尾までを生成し、Flash 全体までは埋めません。ただしイメージ間は `0xFF` で埋まり、NVS の `0x9000–0xefff` も含みます。`erase-flash` を実行しなくても、`*-factory.bin` を書くと保存設定は上書きされます。設定を保持する互換更新には `*-sysupgrade.bin` を使います。空のボードはアプリだけでは起動できません。

**必要なハードウェア：**ESP32-S3、正確に 16 MB Flash と 8 MB Octal PSRAM、データ対応 USB ケーブル、安定した電源。Flash 設定は DIO / 80 MHz です。PSRAM は起動時に検査します。既定 LED は GPIO48 の WS2812 で、不適合なら管理画面で無効にできます。ファイル名やチップ検出だけですべての配線差を確認できるわけではありません。

以下は `v26.10.16` の例です。接頭辞を実際のバージョンに変えてください。SHA-256 は Release ページまたは GitHub のダイジェストと比較します。ライセンスと上流の告知は自動生成されるソースアーカイブに含まれます。[第三者の告知](../THIRD_PARTY_NOTICES.md)を参照してください。ここでの `sysupgrade` はアプリだけの**シリアル更新**で、OTA や OpenWrt の `sysupgrade` コマンドは実装していません。

## 2. Windows 11：PowerShell

### 公式 esptool の導入とイメージの確認

必要なら [python.org](https://www.python.org/downloads/windows/) から **Python ≥ 3.10** を導入します。通常の PowerShell を使い、ESP-IDF 環境は不要です。`py` を使う環境では最初の `python` を `py -3` に置き換えます。

ダウンロードした Release BIN を新しいフォルダーへコピーし、独立したツール環境を作ります：

```powershell
$Firmware = 'esp32-socks5-gateway-v26.10.16-esp32s3-n16r8'
New-Item -ItemType Directory -Force C:\esp\gateway-firmware | Out-Null
Copy-Item -Path "$env:USERPROFILE\Downloads\${Firmware}-*.bin" -Destination C:\esp\gateway-firmware
Set-Location C:\esp\gateway-firmware
Get-FileHash -Algorithm SHA256 "${Firmware}-factory.bin"
python --version
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install esptool==5.5.0
```

SHA-256 を Release ページと比較します。英字の大小は無視できます。更新時は `-sysupgrade.bin` を確認してください。仮想環境の Python を直接呼ぶので PowerShell の実行ポリシーは変更不要です。公式 esptool モジュールを使います。

### シリアルポートを選んで書き込む

ボードを接続し、「デバイス マネージャー → ポート（COM と LPT）」で追加されたポートを確認します。必要に応じて USB ブリッジのメーカー製ドライバーを導入します。`COM5` を実際のポートに置き換え、ほかのシリアルソフトを終了します。まず Flash 検出が **16 MB** であることを確認します。

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 flash-id
```

**初回導入または意図した設定初期化**：

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x0 "${Firmware}-factory.bin"
```

**既存のブートローダー / 配置 / 設定と互換性があるアプリ更新**では、代わりに次を使います。

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x10000 "${Firmware}-sysupgrade.bin"
```

Flash 全体の追加消去は不要です。書き込み後、任意で UART0 コンソールを開きます。

```powershell
.\.venv\Scripts\python.exe -m serial.tools.miniterm COM5 115200 --raw
```

`Ctrl+]` で終了します。ネイティブ USB の書き込み口と UART / USB-UART のログ出力口が異なる場合があります。

### 代替：Python が不要な公式 GUI

Espressif の [Flash Download Tool](https://docs.espressif.com/projects/esp-test-tools/en/latest/esp32s3/production_stage/tools/flash_download_tool.html) を入手します。**ESP32-S3**、ボードの接続方式、COM ポートを選び、`*-factory.bin` を **`0x0`** に設定して行をチェックします。**DoNotChgBin** を有効にして、イメージの DIO / 80 MHz / 16 MB 設定を保持し、115200 baud から開始します。この初回イメージは設定を初期化します。互換性のあるアプリ更新では `*-sysupgrade.bin` だけを `0x10000` に指定します。通常の更新に消去機能は不要です。

## 3. Linux：Bash

Ubuntu/Debian：

```bash
sudo apt-get update
sudo apt-get install python3 python3-venv
```

Fedora（Python に `venv` を含みます）：

```bash
sudo dnf install python3 python3-pip
```

Python ≥ 3.10 を使います。Release BIN をコピーしてハッシュを確認し、公式ツールを導入します：

```bash
firmware_prefix='esp32-socks5-gateway-v26.10.16-esp32s3-n16r8'
mkdir -p ~/esp/gateway-firmware
cp ~/Downloads/"${firmware_prefix}"-*.bin ~/esp/gateway-firmware/
cd ~/esp/gateway-firmware
sha256sum "${firmware_prefix}-factory.bin"
python3 --version
python3 -m venv .venv
.venv/bin/python -m pip install esptool==5.5.0
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

ハッシュを Release ページの同じファイル名の値、または GitHub のダイジェストと比較します。更新時は `${firmware_prefix}-sysupgrade.bin` を確認します。チェックサムは破損検出用なので信頼できる配布元を使ってください。

`/dev/ttyACM0` を実際のポートに置き換えます。`ls -l /dev/ttyACM0` で確認し、`dialout` グループなら `sudo usermod -aG dialout "$USER"` 後に再ログインします。別のグループはディストリビューションの規則に従い、常時 root での書き込みは避けます。まず **16 MB** を確認します。

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 flash-id
```

**初回導入 / 設定初期化**：

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 115200 write-flash 0x0 "${firmware_prefix}-factory.bin"
```

**互換性のあるアプリだけの更新**では代わりに次を使います。

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 115200 write-flash 0x10000 "${firmware_prefix}-sysupgrade.bin"
```

任意の UART0 コンソール：

```bash
.venv/bin/python -m serial.tools.miniterm /dev/ttyACM0 115200 --raw
```

`Ctrl+]` で終了します。**WSL2：**Windows COM ポートは自動で Linux のシリアルポートになりません。[WSL2 USB 接続手順](build-and-flash.ja.md#4-自分でビルドしたファームウェアを書き込む)に従い、`/dev/tty*` を使います。Windows から直接書き込めばこの手順は不要です。

## 4. 初回接続、更新、問題の切り分け

初回導入後の AP は `ESP32-Transparent-Proxy`、パスワードは `esp32proxy`、管理先は **http://192.168.4.1/** です。上流 Wi-Fi は未設定です。管理パスワードと Wi-Fi / SOCKS5 を設定し、通常使用前に共通の AP パスワードを変更します。[初回接続](../README.ja.md#書き込みと初回の接続)に従い、ネットワーク設定保存後は **180 秒以内に確定**します。

アプリだけの更新では通常 NVS を保持し、保存値がコンパイル時の既定値より優先されます。まずリリースの互換性説明を確認してください。配置変更、非互換設定、NVS 復旧処理などで設定の再入力 / 初期化が必要になる場合があります。現行配置の `*-factory.bin` は常に NVS を上書きします。初期化前に設定を控えてください。ファームウェアは設定バックアップではありません。[パーティションと再書き込み](build-and-flash.ja.md#パーティションイメージ再書き込み)も参照してください。

最初は 115200 baud を使い、安定した後に `--baud 460800` を選べます。接続できなければデータ線、ドライバー、ポート、USB 接続口、権限、ほかのソフトを確認します。自動でダウンロードモードに入らない場合はボードの BOOT/EN 手順に従います。チップ / Flash / 配置の不一致を無視して強制書き込みしないでください。

### 応用：個別イメージと任意の補助ツール

Actions とローカル開発パッケージには、検証用の `firmware.zip`、`manifest.json`、`SHA256SUMS`、告知、元の 3 イメージが残ります。その `factory.bin` は正式な `-factory.bin`、`app.bin` は `-sysupgrade.bin` に対応します。これらの補助ファイルは正式 Release には添付しません。配置が互換なら、公式 esptool で NVS の隙間を書かずに 3 イメージを更新できます：

```text
esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x0 images/bootloader.bin 0x8000 images/partition-table.bin 0x10000 images/esp32-socks5-gateway.bin
```

`esptool` が PATH にない場合は、前述の仮想環境の Python と `-m esptool` を使います。任意の補助ツールはソース内の `tools/flash_firmware.py` に残し、パッケージを検証して個別イメージを書き込みます。**ダウンロードには含めず、通常の書き込みにも不要**です。[ツールガイド](../tools/README.md)を参照してください。

## メンテナーによるパッケージ作成

SDK と lwIP を準備し、[独立した配布用の既定ビルド](build-and-flash.ja.md#public-release-build)を使います。AP パスワードを変更した個人用ビルドは公開用ツールが意図的に拒否するため、個人設定を初期化・上書きしないでください。有効化済みプロジェクト端末で実行します。

```text
python tools/package_firmware.py --build-dir build-release --output-dir dist/gateway-validation
```

これは開発/CI 検証用の完全パッケージを生成します。正式 Release は**変更のない `vYY.MM.DD` タグ**からビルドし、代わりに任意のエクスポート先を追加します。

```text
python tools/package_firmware.py --build-dir build-release --output-dir dist/gateway-validation --release-dir dist/gateway-release
```

両出力先は未作成で、異なるパスであり、互いに内包しない必要があります。2 番目のコマンドは両方を生成するので、最初の例で作った出力先を再利用しないでください。ツールはコンパイル済みアプリからバージョンを読み、変更のないソース/タグを検証し、規定名の `-factory.bin` と `-sysupgrade.bin` だけを書き出します。ファイル名やバージョンの手動上書きは不要です。完全パッケージは検証用に保管し、英語の Release 説明にハッシュと対応ソースの告知へのリンクを記載します。個人認証情報、NVS ダンプ、ログを配布しないでください。コマンドはリモートへ公開しません。[ツールガイド](../tools/README.md)も参照してください。

## 検証の範囲

作成、整合性 / 失敗処理、コマンド生成はホストテストと Linux ビルドで検証します。公式 esptool で、実機なしでも生成イメージを確認できます。Windows / WSL2 と GUI の手順は公式仕様に基づきます。このクラウド環境にはボードがなく、Windows 実機の書き込みや ESP32-S3 の起動は未検証です。無線性能は[性能と検証](performance.md)の過去の測定を参照してください。
