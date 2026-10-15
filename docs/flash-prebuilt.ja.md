# ESP-IDF を使わずにビルド済みファームウェアを書き込む

[English](flash-prebuilt.md) · [简体中文](flash-prebuilt.zh-CN.md) · [日本語](flash-prebuilt.ja.md)

Windows 11 または Linux から、Espressif の公式ツールで **ESP32-S3 N16R8** に書き込めます。Git、CMake、Ninja、ESP-IDF、クロスコンパイラー、プロジェクト独自の書き込みスクリプトは不要です。Windows には Python が不要な公式 GUI もあります。コンパイル時の既定値を変える場合は[ソースビルドガイド](build-and-flash.ja.md)を使います。

## 1. 適切なイメージを選ぶ

信頼できる[プロジェクトのリリース](https://github.com/noaic/esp32-socks5-gateway/releases)、または Actions で成功した **Verify and package firmware** の成果物を使います。リポジトリへのアクセス権が必要です。公開された成果物がなければソースからビルドしてください。ワークフローは成果物をアップロードし、Release は自動公開しません。

| ファイル | 用途 |
| --- | --- |
| `factory.bin` | 初回導入 / 初期化用の完全イメージ。ブートローダー + パーティション表 + アプリを **`0x0`** に書き込む。**保存したネットワーク・管理設定を初期化** |
| `app.bin` | アプリだけの更新。**`0x10000`** に書き込む。既存のブートローダー、配置、設定バージョンの互換性が必要 |
| `firmware.zip` | 両イメージ、元の個別イメージ、情報、説明、ライセンスをまとめた ZIP。**ホスト用スクリプトは含まない** |
| `manifest.json`、`SHA256SUMS` | ハードウェア / ビルドの出典と SHA-256 チェックサム |
| `firmware.zip.sha256` | ZIP 自体のチェックサム。ZIP と別に配布 |
| `FLASHING.md`、`LICENSE`、`THIRD_PARTY_NOTICES.md`、`LICENSES/` | 書き込み説明と適用ライセンス。ファームウェア再配布時にも保持 |

完全イメージはビルド結果から結合し、**設定済みデバイスの 16 MB Flash ダンプではありません**。アプリ末尾までを生成し、Flash 全体までは埋めません。ただしイメージ間は `0xFF` で埋まり、NVS の `0x9000–0xefff` も含みます。`erase-flash` を実行しなくても、`factory.bin` を書くと保存設定は上書きされます。設定を保持する互換更新には `app.bin` を使います。空のボードはアプリだけでは起動できません。

**必要なハードウェア：**ESP32-S3、正確に 16 MB Flash と 8 MB Octal PSRAM、データ対応 USB ケーブル、安定した電源。Flash 設定は DIO / 80 MHz です。PSRAM は起動時に検査します。既定 LED は GPIO48 の WS2812 で、不適合なら管理画面で無効にできます。ファイル名やチップ検出だけですべての配線差を確認できるわけではありません。

`factory.bin` または `app.bin` と、`SHA256SUMS`、著作権表示をダウンロードするか、`firmware.zip` を新しい空のフォルダーへ展開します。Actions の成果物がさらに ZIP を包んでいる場合は両方展開します。以下では `gateway-firmware` フォルダーを使います。チェックサムは破損検出用で、配布者の証明ではありません。信頼できる入手先を使ってください。

## 2. Windows 11：PowerShell

### 公式 esptool の導入とイメージの確認

必要なら [python.org](https://www.python.org/downloads/windows/) から **Python ≥ 3.10** を導入します。通常の PowerShell を使い、ESP-IDF 環境は不要です。`py` を使う環境では最初の `python` を `py -3` に置き換えます。

例として、ダウンロードした `firmware.zip` を展開し、独立したツール環境を作ります。

```powershell
New-Item -ItemType Directory -Force C:\esp | Out-Null
Expand-Archive -LiteralPath "$env:USERPROFILE\Downloads\firmware.zip" -DestinationPath C:\esp\gateway-firmware
Set-Location C:\esp\gateway-firmware
Get-FileHash -Algorithm SHA256 .\factory.bin
Get-Content .\SHA256SUMS | Select-String '  factory\.bin$'
python --version
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install esptool==5.5.0
```

2 つの SHA-256 を大文字小文字を区別せず比較します。更新時は `app.bin` を確認してください。展開前の ZIP は `Get-FileHash` の結果と `firmware.zip.sha256` を比較します。仮想環境の Python を直接使うため、PowerShell の実行ポリシー変更は不要です。実行するのは導入した公式 esptool モジュールで、同梱の独自スクリプトではありません。

### シリアルポートを選んで書き込む

ボードを接続し、「デバイス マネージャー → ポート（COM と LPT）」で追加されたポートを確認します。必要に応じて USB ブリッジのメーカー製ドライバーを導入します。`COM5` を実際のポートに置き換え、ほかのシリアルソフトを終了します。まず Flash 検出が **16 MB** であることを確認します。

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 flash-id
```

**初回導入または意図した設定初期化**：

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x0 factory.bin
```

**既存のブートローダー / 配置 / 設定と互換性があるアプリ更新**では、代わりに次を使います。

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x10000 app.bin
```

Flash 全体の追加消去は不要です。書き込み後、任意で UART0 コンソールを開きます。

```powershell
.\.venv\Scripts\python.exe -m serial.tools.miniterm COM5 115200 --raw
```

`Ctrl+]` で終了します。ネイティブ USB の書き込み口と UART / USB-UART のログ出力口が異なる場合があります。

### 代替：Python が不要な公式 GUI

Espressif の [Flash Download Tool](https://docs.espressif.com/projects/esp-test-tools/en/latest/esp32s3/production_stage/tools/flash_download_tool.html) を入手します。**ESP32-S3**、ボードの接続方式、COM ポートを選び、`factory.bin` を **`0x0`** に設定して行をチェックします。**DoNotChgBin** を有効にして、イメージの DIO / 80 MHz / 16 MB 設定を保持し、115200 baud から開始します。この初回イメージは設定を初期化します。互換性のあるアプリ更新では `app.bin` だけを `0x10000` に指定します。通常の更新に消去機能は不要です。

## 3. Linux：Bash

Ubuntu/Debian：

```bash
sudo apt-get update
sudo apt-get install python3 python3-venv unzip
```

Fedora（Python に `venv` を含みます）：

```bash
sudo dnf install python3 python3-pip unzip
```

Python ≥ 3.10 を使います。展開とチェック後、公式ツールを導入します。

```bash
mkdir -p ~/esp/gateway-firmware
unzip ~/Downloads/firmware.zip -d ~/esp/gateway-firmware
cd ~/esp/gateway-firmware
sha256sum -c SHA256SUMS
python3 --version
python3 -m venv .venv
.venv/bin/python -m pip install esptool==5.5.0
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

`sha256sum -c` では全項目が `OK` になることを確認します。単独の BIN を入手した場合は `sha256sum factory.bin`（または `app.bin`）を `SHA256SUMS` の対応行と比較します。ZIP は同じフォルダーで `sha256sum -c firmware.zip.sha256` を実行します。

`/dev/ttyACM0` を実際のポートに置き換えます。`ls -l /dev/ttyACM0` で確認し、`dialout` グループなら `sudo usermod -aG dialout "$USER"` 後に再ログインします。別のグループはディストリビューションの規則に従い、常時 root での書き込みは避けます。まず **16 MB** を確認します。

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 flash-id
```

**初回導入 / 設定初期化**：

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 115200 write-flash 0x0 factory.bin
```

**互換性のあるアプリだけの更新**では代わりに次を使います。

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 115200 write-flash 0x10000 app.bin
```

任意の UART0 コンソール：

```bash
.venv/bin/python -m serial.tools.miniterm /dev/ttyACM0 115200 --raw
```

`Ctrl+]` で終了します。**WSL2：**Windows COM ポートは自動で Linux のシリアルポートになりません。[WSL2 USB 接続手順](build-and-flash.ja.md#4-自分でビルドしたファームウェアを書き込む)に従い、`/dev/tty*` を使います。Windows から直接書き込めばこの手順は不要です。

## 4. 初回接続、更新、問題の切り分け

初回導入後の AP は `ESP32-Transparent-Proxy`、パスワードは `esp32proxy`、管理先は **http://192.168.4.1/** です。上流 Wi-Fi は未設定です。管理パスワードと Wi-Fi / SOCKS5 を設定し、通常使用前に共通の AP パスワードを変更します。[初回接続](../README.ja.md#書き込みと初回の接続)に従い、ネットワーク設定保存後は **180 秒以内に確定**します。

アプリだけの更新では通常 NVS を保持し、保存値がコンパイル時の既定値より優先されます。まずリリースの互換性説明を確認してください。配置変更、非互換設定、NVS 復旧処理などで設定の再入力 / 初期化が必要になる場合があります。現行配置の `factory.bin` は常に NVS を上書きします。初期化前に設定を控えてください。ZIP は設定バックアップではありません。[パーティションと再書き込み](build-and-flash.ja.md#パーティションイメージ再書き込み)も参照してください。

最初は 115200 baud を使い、安定した後に `--baud 460800` を選べます。接続できなければデータ線、ドライバー、ポート、USB 接続口、権限、ほかのソフトを確認します。自動でダウンロードモードに入らない場合はボードの BOOT/EN 手順に従います。チップ / Flash / 配置の不一致を無視して強制書き込みしないでください。

### 応用：個別イメージと任意の補助ツール

ZIP には元の 3 イメージもあります。既存配置に互換性があれば、公式 esptool で NVS の隙間を避けて 3 つとも更新できます。導入済み esptool を使い、展開フォルダーで実行します。`COM5` は実際の Windows または Linux ポートに置き換えます。

```text
esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x0 images/bootloader.bin 0x8000 images/partition-table.bin 0x10000 images/esp32-socks5-gateway.bin
```

`esptool` が PATH にない場合は、前述の仮想環境の Python と `-m esptool` を使います。任意の補助ツールはソース内の `tools/flash_firmware.py` に残し、パッケージを検証して個別イメージを書き込みます。**ダウンロードには含めず、通常の書き込みにも不要**です。[ツールガイド](../tools/README.md)を参照してください。

## メンテナーによるパッケージ作成

ESP-IDF を有効にした端末のプロジェクトルートで、[ソースビルド](build-and-flash.ja.md)後に実行します。

```text
python tools/package_firmware.py --build-dir build --output-dir dist/esp32-socks5-gateway-n16r8-local
```

新しい出力フォルダーに、イメージ、チェックサム、著作権表示、スクリプトを含まない `firmware.zip` を生成します。配布用にはクリーンな公開既定設定を使い、個人のコンパイル済み認証情報、デバイスの NVS ダンプ、ログを含めないでください。共有前にマニフェストとライセンスを確認します。パッケージ作成と成果物生成は Release を公開しません。[ツールガイド](../tools/README.md)も参照してください。

## 検証の範囲

作成、整合性 / 失敗処理、コマンド生成はホストテストと Linux ビルドで検証します。公式 esptool で、実機なしでも生成イメージを確認できます。Windows / WSL2 と GUI の手順は公式仕様に基づきます。このクラウド環境にはボードがなく、Windows 実機の書き込みや ESP32-S3 の起動は未検証です。無線性能は[性能と検証](performance.md)の過去の測定を参照してください。
