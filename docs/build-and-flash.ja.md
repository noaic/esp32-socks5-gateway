# ソースからのビルドと書き込み

[English](build-and-flash.md) · [简体中文](build-and-flash.zh-CN.md) · [日本語](build-and-flash.ja.md)

指定 SDK、lwIP の準備、設定、ビルド、書き込みの全手順をまとめています。ESP-IDF を導入せずに使う場合は、[ビルド済みファームウェアのガイド](flash-prebuilt.ja.md)を選んでください。書き込み後は[初回の接続設定](../README.ja.md#書き込みと初回の接続)へ戻ります。

## 開発用の依存関係

### ソフトウェア

| 種類 | 依存関係と用途 | 準備方法 |
| --- | --- | --- |
| 必須：開発ツール | Git、Python ≥ 3.10（3.12 推奨）、CMake ≥ 3.22、Ninja | Windows は公式 EIM、Linux は後述のシステムパッケージ。指定 IDF の推奨は CMake 4.0.3、Ninja 1.12.1 |
| 必須：SDK | 指定コミットの ESP-IDF v6.1 と esp-lwIP | 後述の表を参照。FreeRTOS と lwIP は ESP-IDF に含まれ、別途インストールしません |
| 自動管理：ツールチェーン | ESP32-S3 用 Xtensa GCC 15.2.0、esptool、Python ビルドツール、Component Manager | EIM または IDF スクリプトが、このコミットの一覧に従って取得 |
| 自動管理：部品 | cJSON 1.7.19、led_strip 3.0.3 | `main/idf_component.yml` に宣言し、`dependencies.lock` で版とハッシュを固定。初回ビルドで `managed_components/` に取得 |
| 必須：利用環境 | ブラウザー、既存の 2.4 GHz Wi-Fi、IPv4 / NO AUTH / UDP ASSOCIATE の SOCKS5 サービス | 利用者が準備。ビルドとホストテストだけなら実際のプロキシは不要 |
| 任意：開発テスト | Linux のシステム GCC/binutils、Node.js。ccache は再ビルドを高速化 | GCC はホスト C テスト、Node.js はフロントエンドテスト用で、ファームウェアの実行依存ではありません |

管理画面と初回設定画面は既定で英語です。ブラウザーの最優先言語が中国語（`zh` または `zh-*`）なら簡体字中国語、それ以外や判定できない場合は英語を表示します。英語が優先で中国語が第 2 候補の場合も英語です。シリアルコマンドは英語です。システム GCC と ESP32 用コンパイラーは別の用途です。ファームウェア用の代わりに前者を使ったり、ビルド成功のためだけにロックファイルを更新したりしないでください。

Windows は **PowerShell**、Linux / WSL2 は **Bash** を使います。自分の環境に対応するコマンドだけ実行してください。SDK とプロジェクトのパスは空白や非 ASCII 文字を避け、Windows なら `C:\esp` などを使います。ほかのプロジェクトで使う ESP-IDF を上書きせず、別のディレクトリに準備します。

## 1. 対応する ESP-IDF をインストールする

透過転送には、小さな lwIP パッチを使っています。lwIP は ESP-IDF 内部で TCP/IP 通信を処理するライブラリです。パッチは具体的なソースコードに依存するため、「v6.1」という表示だけでは不十分です。次のコミットを合わせてください。

| 依存関係 | 正確なコミット |
| --- | --- |
| ESP-IDF v6.1 | `fff9895c82d744c7237be8847347bdd1b07c6643` |
| esp-lwIP | `c6f2f878e7b0f86033214b85547d579be43351e3` |

### Windows 11：公式 EIM と PowerShell

1. [Espressif の公式ページ](https://dl.espressif.com/dl/eim/)から EIM の GUI インストーラーを取得します。[Windows 公式ガイド](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/windows-setup.html)の前提条件チェックを行い、Git が使えなければ [Git for Windows](https://git-scm.com/install/windows)を入れて新しい PowerShell を開いてください。`winget` は必須ではありません。
2. 通常の PowerShell で、**まだ存在しない** `C:\esp\esp-idf-gateway` に指定コミットを取得します。準備スクリプトは LF と CRLF の両方に対応するため、Git の改行設定を変更する必要はありません。

```powershell
git --version
New-Item -ItemType Directory -Force C:\esp | Out-Null
git clone --no-checkout --depth 1 https://github.com/espressif/esp-idf.git C:\esp\esp-idf-gateway
Set-Location C:\esp\esp-idf-gateway
git fetch --depth 1 origin fff9895c82d744c7237be8847347bdd1b07c6643
git checkout --detach fff9895c82d744c7237be8847347bdd1b07c6643
git submodule update --init --recursive --depth 1
```

3. EIM の **Custom Installation** で、この既存 Git ディレクトリを指定します。公式の [Expert Installation](https://docs.espressif.com/projects/idf-im-ui/en/latest/expert_installation.html)は既存リポジトリを変更せずに利用できます。このチェックアウトに対応するコンパイラー、CMake、Ninja、Python 環境を準備してください。別の最新 SDK をそのまま使う方法ではありません。
4. EIM の **Manage Installations → Open IDF Terminal**、または生成された PowerShell ショートカットから、該当する環境を開きます。**以降の Windows コマンドは、この有効化済みターミナルで実行します。** 普通の PowerShell を新しく開いても自動では有効になりません。

```powershell
python --version
idf.py --version
git -C "$env:IDF_PATH" rev-parse HEAD
git -C "$env:IDF_PATH\components\lwip\lwip" rev-parse HEAD
git -C "$env:IDF_PATH\components\lwip\lwip" ls-files --eol src/core/ipv4/ip4.c
cmake --version
ninja --version
```

両コミットが表と一致することを確認してください。改行コマンドは診断用で、`i/lf w/lf` と `i/lf w/crlf` の両方に対応します。後者は Git が Windows の改行で取得したことを示し、ソースの編集とは限りません。この差だけで SDK を再インストールする必要はありません。EIM は生成された入口で有効化し、従来方式なら SDK 内の PowerShell で `.\install.ps1 esp32s3` / `.\export.ps1`、CMD で `install.bat esp32s3` / `export.bat` を使います。Windows で Bash の `source` は使いません。

### Linux：Ubuntu/Debian または Fedora、Bash

Ubuntu/Debian では [公式の依存パッケージ一覧](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/linux-macos-setup-legacy.html)を使います。Ubuntu 22.04 以降など、Python ≥ 3.10 を提供する版を選んでください。

```bash
sudo apt-get update
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

Fedora はパッケージ名が異なり、`dnf` を使います。Python パッケージに `venv` が含まれます。

```bash
sudo dnf install git wget flex bison gperf python3 python3-pip cmake ninja-build ccache libffi-devel openssl-devel dfu-util libusb1
```

SDK を別ディレクトリに準備します。`esp-idf-gateway` はまだ存在しないものとします。

```bash
mkdir -p ~/esp
cd ~/esp

git clone --no-checkout --depth 1 https://github.com/espressif/esp-idf.git esp-idf-gateway
cd esp-idf-gateway
git fetch --depth 1 origin fff9895c82d744c7237be8847347bdd1b07c6643
git checkout --detach fff9895c82d744c7237be8847347bdd1b07c6643
git submodule update --init --recursive --depth 1

./install.sh esp32s3
python3 tools/idf_tools.py install cmake ninja
source ./export.sh
```

インストーラーが対応するコンパイラーと Python 依存関係を取得します。CMake/Ninja を明示的に取得すると、古いシステム版を避けられます。**新しい Bash を開くたびに有効化し直し**、実際の版を確認します。

```bash
source ~/esp/esp-idf-gateway/export.sh
idf.py --version
git -C "$IDF_PATH" rev-parse HEAD
git -C "$IDF_PATH/components/lwip/lwip" rev-parse HEAD
cmake --version
ninja --version
```

### 既存の環境を再利用できる？

両方のコミットが一致し、サブモジュールが揃い、全体の lwIP ソースが未変更の場合だけ再利用できます。その環境自身の入口で有効化して上記を確認してください。「v6.1」だけでは判断できません。一致しなければ別の SDK を作り、共有の SDK に直接 `git pull` したり版を切り替えたりしないでください。EIM は EIM のターミナル、スクリプト方式は対応する `export` を使います。

浅い clone では `idf.py --version` が `Git version unavailable` と表示してからソースの版を読む場合があります。タグ表示だけでなく、両コミットとパッチ検証で確認してください。

WSL2 は Linux ディストリビューション内で Linux の手順を実行し、Windows のコンパイラーをそのまま再利用しません。USB 接続は後述します。macOS は公式ガイドを参照してください。Bash のプロジェクトコマンドは同じです。

## 2. プロジェクトを取得し、lwIP を準備する

Windows の有効化済み PowerShell：

```powershell
git clone https://github.com/noaic/esp32-socks5-gateway.git C:\esp\esp32-socks5-gateway
Set-Location C:\esp\esp32-socks5-gateway
python tools/prepare_lwip.py apply
python tools/prepare_lwip.py check
```

Linux / WSL2 の有効化済み Bash：

```bash
git clone https://github.com/noaic/esp32-socks5-gateway.git ~/esp32-socks5-gateway
cd ~/esp32-socks5-gateway
python3 tools/prepare_lwip.py apply
python3 tools/prepare_lwip.py check
```

すでに取得済みなら、そのディレクトリに移動してください。以降のプロジェクトコマンドは、**`CMakeLists.txt` があるプロジェクトのルート**で実行します。

`patches/lwip-v6.1-transparent-tcp.patch` は `ip4.c` の変更、`patches/lwip-v6.1.json` はコミットと変更前後の SHA-256 を記録します。スクリプトは `$IDF_PATH/components/lwip` と lwIP サブモジュールの Git 管理下ファイルを、プロジェクトの `components/lwip` にコピーし、そのコピーだけを変更します。ESP-IDF の [同名コンポーネントの優先順位](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/build-system.html#multiple-components-with-the-same-name)により、プロジェクト側が使われます。全体の SDK やほかのプロジェクトには影響せず、生成ディレクトリは Git の対象外です。

`ip4.c` のハッシュ検証はメモリー上で CRLF を LF として扱い、既存のハッシュを維持します。`apply` と `restore` は検証済みプロジェクトコピーだけを LF で保存します。パッチが CRLF でも、そのファイルを書き換えずに利用できます。`check` は読み取り専用です。`apply`/`check` は **SDK の lwIP コンポーネント/サブモジュールの Git 管理下ファイルをコミット内容と比較**し、HEAD が一致していても内容を調べます。SDK 内の未管理テスト/編集キャッシュは無視し、コピーしません。プロジェクト側には管理下ファイルだけが必要で、差分は所定の `ip4.c` パッチに限ります。すべてのテキスト比較で LF/CRLF の差を許可し、バイナリーはバイト単位で一致させます。コード/空白の編集、BOM、単独の CR、管理下ファイルの欠落、ローカルコピーの予期しない追加ファイル、シンボリックリンクは拒否します。

次のコマンドは、どちらの有効化済みターミナルでも使えます。Linux では `python3` も使えます。

| コマンド | 用途 |
| --- | --- |
| `python tools/prepare_lwip.py apply` | 版と内容を確認し、コピーを作成または再利用。`ip4.c` を LF にしてパッチを適用。繰り返し実行可能 |
| `python tools/prepare_lwip.py check` | 読み取り専用で、コミット、LF として計算した元/適用後のハッシュ、全ファイルの一覧と内容を検証 |
| `python tools/prepare_lwip.py restore` | 検証済みのローカル `ip4.c` を逆適用で LF に戻す。SDK の有効化は不要。ディレクトリを残し、透過機能は自動で無効にしません |

`restore` はローカル `ip4.c` のパッチ状態だけを確認するオフライン復元です。SDK/コンポーネント全体の整合性確認には `apply` と `check` を使ってください。

既定の透過機能では、`restore` 後に再び `apply` と `check` が必要です。`check` は未適用のコピーを拒否します。以前のビルドが全体の lwIP を参照していた場合、コピー準備後に `idf.py reconfigure` を実行してからビルドします。版のエラーには期待値と実際のコミットが表示され、ハッシュのエラーは LF/CRLF 以外の差を示します。SDK やコピーの編集をバックアップして確認するか、別の正常な SDK を準備し、強制上書きは避けてください。`apply` と `check` はほかのファイルの変更も拒否し、書き込み時はローカルのシンボリックリンクも拒否します。ESP-IDF/esp-lwIP の更新で `ip4.c` のパス、フック位置、インターフェースが変わる可能性があり、パッチの再検討と適合が必要です。コミットやハッシュだけ変えて検証を回避しないでください。

## 3. 設定してビルドする

次の設定・ビルドコマンドは、両方の有効化済みターミナルで同じです。

```text
idf.py menuconfig
```

`Transparent gateway Wi-Fi` を開きます。

- 初回は上流の `SSID` を空にして、管理ページから後で設定できます。この場合 AP は起動しますが、上流 Wi-Fi への接続は試みません。
- `SoftAP WPA2 password` を自分の AP パスワードに変更します。ASCII 文字で 8～63 文字にしてください。
- AP 名は `ESP32-Transparent-Proxy` のままで構いません。
- ほかのハードウェア / ネットワーク設定は、まず既定値を使います。通常の利用で `Temporary gateway diagnostics` を有効にする必要はありません。

保存してメニューを閉じ、次を実行します。

```text
idf.py build
idf.py size
```

`build` はファームウェアを生成し、`size` は容量を表示します。この段階ではボードに書き込みません。初回は関連部品をダウンロードするため、次回以降より時間がかかります。

`sdkconfig.defaults` は ESP32-S3、Flash、PSRAM などの初期設定を提供します。既存の `sdkconfig` を上書きしないため、古い設定を使う場合もボードの仕様を確認してください。

Linux x86_64 のパッチ準備、ビルド、容量確認は検証済みです。Windows 11/EIM、各ディストリビューションのパッケージ導入、WSL2 USB は公式説明と照合していますが、各環境と実機での確認が必要です。

<a id="public-release-build"></a>

### 配布用の既定ファームウェアを別にビルドする

自分のボード用の `sdkconfig` と `build/` は保持できます。公開用パッケージツールは独自の AP パスワードや上流の認証情報を意図的に拒否します。配布用は、有効化済みのプロジェクト端末で**別の、まだ使っていない** `build-release/` と設定ファイルを使います。

Windows PowerShell：

```powershell
$ReleaseConfig = Join-Path (Get-Location) 'build-release\sdkconfig'
idf.py -B build-release -D "SDKCONFIG=$ReleaseConfig" build
idf.py -B build-release size
```

Linux/WSL2 Bash：

```bash
idf.py -B build-release -D SDKCONFIG="$PWD/build-release/sdkconfig" build
idf.py -B build-release size
```

新しい設定は `sdkconfig.defaults` の公開用既定値を使います。上流 Wi-Fi は空で、初期 AP の共通パスワードは `esp32proxy` です。書き込み後の初回設定で AP パスワードを変更してください。プロジェクトルートの個人用 `sdkconfig` は上書きしません。`build-release/sdkconfig` が既存なら、その値が優先されます。内容を確認するか別の未使用パスを選び、個人設定をコピーしないでください。次は[パッケージ作成](flash-prebuilt.ja.md#メンテナーによるパッケージ作成)へ進みます。

## 4. 自分でビルドしたファームウェアを書き込む

Windows 11 では「デバイス マネージャー → ポート（COM と LPT）」で接続前後を比較し、追加された COM ポートを探します。USB ブリッジに応じたメーカーのドライバーも必要になる場合があります。Linux は接続前後の一覧と権限を確認します。

```bash
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
ls -l /dev/ttyACM0
```

`/dev/ttyACM0` と `COM5` は例なので、実際のポートに置き換えてください。macOS は通常 `/dev/cu.*` です。Linux のデバイスが `dialout` グループなら `sudo usermod -aG dialout "$USER"` 後にログアウト / ログインします。ほかのグループは実際の所有権に合わせ、常時 root で書き込む方法は避けます。

Linux の有効化済みプロジェクトターミナル：

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Windows の有効化済み PowerShell プロジェクトターミナル：

```powershell
idf.py -p COM5 flash monitor
```

`flash` は書き込み、`monitor` は **115200 baud** のシリアルログ表示です。ESP-IDF monitor は `Ctrl+]` で終了できます。書き込み、monitor、ほかのシリアルツールで同じポートを同時に使わないでください。

現在は **UART0 コンソール**設定です。ボードの UART / USB-UART 接続口を優先してください。ネイティブ USB で書き込めても、その接続口に現在の UART ログが出るとは限りません。自動でダウンロードモードに入れない場合は、ボードの BOOT/EN 操作説明に従います。

**WSL2 USB：**Windows に [Microsoft の説明に従って usbipd-win](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)を入れ、WSL2 ディストリビューションを起動します。次は Windows PowerShell のコマンドです。`4-4` は例で、`usbipd list` の実際の BUSID に置き換えてください。`bind` は管理者権限が必要です。

```powershell
usbipd list
usbipd bind --busid 4-4
usbipd attach --wsl --busid 4-4
```

その後 WSL Bash で `/dev/ttyACM*` / `/dev/ttyUSB*` を探し、Linux のコマンドで書き込みます。リセット後に再認識され、割り当てを再確認する場合があります。同じ USB を Windows と WSL のシリアルツールから同時には使えません。

### パーティション、イメージ、再書き込み

`partitions.csv` は次の配置を定義します。16 MB Flash の残りは未割り当てで、OTA やファイルシステム用パーティションはありません。

| 領域 | 開始アドレス | サイズ | 用途 |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | `0x6000`（24 KiB） | 永続設定などの NVS データ |
| `phy_init` | `0xf000` | `0x1000`（4 KiB） | PHY 初期化データの予約領域。現在は独立した PHY イメージなし |
| `factory` | `0x10000` | 4 MiB | アプリケーション |

ビルドは `build/bootloader/bootloader.bin`（アプリ起動）、`build/partition_table/partition-table.bin`（配置の説明）、`build/esp32-socks5-gateway.bin`（アプリ）を生成します。現在の `build/flash_args` は順に `0x0`、`0x8000`、`0x10000` を指定し、`idf.py flash` が使用します。手作業でアドレスを推測する必要はありません。

上記の `idf.py flash` は 3 つの個別イメージの範囲を消去・書き込みし、`erase-flash` とは異なります。同じ配置なら NVS 設定は通常残ります。全消去はネットワークと管理パスワードを消します。配布用の完全 `factory.bin` も埋め合わせ領域が NVS を覆うため、全消去なしでも設定を消去します。互換性のある `app.bin` 更新はアプリだけを書き込みます。[ビルド済みファームウェアのガイド](flash-prebuilt.ja.md)を参照してください。通常の更新では全消去せず、配置や互換性のない設定形式を変える前に設定を保存してください。既存 NVS は新しいコンパイル時の既定値より優先します。

起動時に `ESP_ERR_NVS_NO_FREE_PAGES` または `ESP_ERR_NVS_NEW_VERSION_FOUND` が出た場合は例外です。現在のコードは NVS を消去して再試行するため、設定を失う場合があります。ハードウェア/NVS の確認前に起動が失敗すると、シリアル復旧コマンドはまだ使えません。まず起動ログを確認してください。[復旧の範囲](maintenance.md#tested-boundary)も参照してください。

## ビルド時の問題

| 症状 | 最初に確認する点 |
| --- | --- |
| `idf.py` が見つからない | 今のターミナルで ESP-IDF を有効にしてから、プロジェクトへ移動 |
| Python / CMake / Ninja の版やパスが違う | Python ≥ 3.10、CMake ≥ 3.22、対応 SDK のターミナル。Windows と WSL のツールを混ぜない |
| lwIP の revision / hash が一致しない | 両固定コミットとサブモジュールを確認。LF/CRLF の差だけなら対応済みです。SDK の lwIP リポジトリで `git diff` を確認するか、プロジェクト側をバックアップしてから再生成。パッチは強制しない |
| ビルドが別 SDK や全体の lwIP を参照 | 正しい環境で `idf.py reconfigure`。古いキャッシュが残る場合はプロジェクト内で `idf.py fullclean` 後にビルド |
| ビルド中に部品をダウンロードできない | 最初のダウンロードエラーと、GitHub・Espressif のダウンロード先・部品レジストリへの接続。TLS / ハッシュ検証は無効にしない |
| シリアルポートがない / 書き込めない | データ対応ケーブル、ボードの USB 接続先、ドライバー、ポート権限、WSL の USB 接続、ほかのツールによる使用 |
