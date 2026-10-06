# esp32-socks5-gateway

[English](README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md)

**ESP32-S3 を使った IPv4 透過 SOCKS5 ゲートウェイ**です。端末を ESP32 の Wi-Fi AP に接続するだけで、端末側にプロキシを設定せずに TCP/UDP 通信を上流の SOCKS5 サーバーへ転送します。

個人の実験プロジェクトで、現在の基準は検証済みの **web51** です。Android 2台での接続、転送、Web管理、設定のロールバック、シリアル復旧を確認しています。公開版のアドレスは例示であり、個人のWi-Fi認証情報、NVS、ログ、ビルド成果物は含みません。

## 機能と対象範囲

- IPv4 の Wi-Fi AP+STA、AP端末は最大 **2台**、DHCPを使用。
- TCPはSOCKS5 CONNECT、UDPはUDP ASSOCIATE。SOCKS5 v5の **NO AUTH** のみ。
- 接続ごとにタスクを作らず、固定イベントループで処理。TCPのハンドシェイク、ACK、再送、ウィンドウ、切断はlwIPが担当。
- 独立したフロー情報：TCPリレー64、終了処理を含むTCPマッピング512、UDPフロー32。
- AP内のWeb管理：初回パスワード設定、状態、端末別カウンター、設定、イベント、診断、再接続と再起動。
- 設定変更後に再起動し、180秒以内の確認が必要。未確認なら以前の設定へ戻ります。
- GPIO48のWS2812状態LED。消灯設定を保存可能。
- 英語のUARTコマンドと、1回限りの確認コードによるパスワード／ネットワーク復旧。

IPv6、HTTP上流プロキシ、SOCKSユーザー名／パスワード認証、BLE、OTA、VLESS/REALITY、ローカルGeoIP/GeoSiteルーティング、NTPは実装していません。SOCKS UDPの`FRAG != 0`は破棄します。ブロードキャスト、マルチキャスト、特殊なP2P動作や帯域の厳密な均等配分は対象外です。

## ハードウェアと依存関係

| 項目 | 検証済み構成 |
| --- | --- |
| モジュール | ESP32-S3 N16R8、デュアルコア240 MHz |
| Flash | 16 MB、DIO、80 MHz |
| PSRAM | 8 MB、Octal、80 MHz、起動時メモリテスト |
| LED | GPIO48のWS2812/NeoPixel 1個 |
| コンソール | UART、115200 baud |
| ESP-IDF | v6.1、下記のコミットを固定 |
| ツールチェーン | xtensa-esp32s3-elf-gcc 15.2.0 |
| コンポーネント | cJSON 1.7.19、led_strip 3.0.3、dependencies.lockで固定 |

lwIPパッチは次のリビジョンに対応しています。

- ESP-IDF：`fff9895c82d744c7237be8847347bdd1b07c6643`
- esp-lwIP：`c6f2f878e7b0f86033214b85547d579be43351e3`

準備スクリプトがコミットとハッシュを確認します。バージョン表示がv6.1でも、別コミットへの適用は保証しません。パッチは生成されるプロジェクト内の`components/lwip`に適用し、グローバルESP-IDFを変更しません。

## ビルドと初回設定

[公式インストール手順](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html)に従って指定リビジョンとサブモジュールを用意し、`source "$IDF_PATH/export.sh"`またはEIMの有効化スクリプトを実行します。

```bash
python3 tools/prepare_lwip.py apply
idf.py menuconfig
idf.py build
idf.py size
```

`Transparent gateway Wi-Fi`で上流SSID／パスワードとAPパスワードを設定してください。STA SSIDが空ならAPのみ起動し、STA接続は試みません。`sdkconfig.defaults`は既存の`sdkconfig`を上書きしません。

実際のシリアルポートを指定して書き込みます。

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

1. `ESP32-Transparent-Proxy`へ接続。開発用APパスワード`esp32proxy`は通常使用前に変更してください。
2. **http://192.168.4.1/**で管理パスワードを初回設定します。APのWi-Fiパスワードとは別です。
3. 上流SOCKS5の実際のIPv4とポートを入力。`192.168.1.2:7897`は例です。LANからの接続、NO AUTH、UDP ASSOCIATEを許可するサーバーが必要です。
4. 保存後にAPへ再接続し、180秒以内に設定を確認。DNS、Web閲覧、接続診断を確認します。

上流Wi-FiもWeb画面から設定できます。NVSに保存した設定はコンパイル時の既定値より優先され、通常のアプリ再書き込みでは消えません。

APは`192.168.X.1/24`の第3オクテットを変更できます。上流とサブネットが重複する場合は変更してください。再起動後は新アドレスへアクセスし、試用期間内に確認します。DHCPの既定DNSは`1.1.1.1`で、ESP32自体はDNSサーバーではありません。DNS UDPもSOCKS5経由で転送します。

## 状態表示と復旧

LEDは赤＝STA未準備、青＝APのみ、紫＝AP+STA準備済みだがInternet結果不明／期限切れ、緑＝直近のSOCKS経由DNS/HTTP検査成功、黄＝サブネット重複または検査の連続失敗です。緑は検査先への到達を示し、すべてのサイトへの接続を保証しません。通常の自動検査は5分間隔で、無効化可能です。失敗による自動再起動は行いません。

```text
help
status
events
log info
log warn
log off
reset-admin
reset-network
confirm CODE
cancel
```

`reset-admin`は現在のネットワークを保持して管理パスワードを解除します。`reset-network`は管理パスワードを保持してコンパイル時のネットワーク既定値へ戻し、入口を`192.168.4.1`にします。どちらも設定の試用を取り消し、成功後に再起動します。

装置が表示した30秒有効の確認コードを使ってください。上記の`CODE`をそのまま入力しないでください。RST/ENは再起動のみ、BOOTは復旧メニューに使用しません。UARTの書き込み、monitor、ログ取得を同時に実行しないでください。詳しくは[保守と復旧](docs/maintenance.md)を参照してください。

## 検証と性能

Linux上で75項目のC/Python回帰テストと、フロントエンドのJavaScriptテストがあります。先にIDFビルドを実行してcJSONを取得し、ホストGCC/binutilsとpyserial/esptoolを用意します。

```bash
PATH=/usr/bin:/bin:$PATH python -m unittest discover -s tests
node tests/management_feedback_harness.js
node tests/management_transport_harness.js
```

Pythonが別の環境に切り替わる場合は、必要な環境のPythonを絶対パスで指定してください。Nodeはフロントエンドテスト専用です。ホストテストだけでWi-Fi、電源断、長時間安定性を検証したとは扱いません。

過去のローカル測定は単一ストリーム約16/16 Mbps、4ストリーム合計約24/21 Mbps。混合負荷ではAの1080P再生に停止がなく、Bは14.41/11.83 Mbps、A待機時は14.21/11.97 Mbpsでした。端末、電波、動画キャッシュなどに左右される短時間の結果で、保証値ではありません。[性能と検証](docs/performance.md)に条件を記載しています。

## 構成と制限

`main/`はファームウェア、`patches/`はlwIPパッチ、`tools/`はUART・測定・準備ツール、`tests/`は回帰テスト、`examples/`はXray設定断片、`docs/`は設計と匿名化した開発履歴です。履歴には撤回済み実験も含まれ、現在の動作はREADMEとdocs直下の説明を優先してください。

管理はAP限定ですがHTTPです。NVS内のパスワードや設定は暗号化していません。UART復旧は物理アクセスを前提とし、認証しません。安全な端末で初回設定し、管理画面をインターネットへ公開しないでください。

2台の端末はAP+STAの単一無線とフロー容量を共有します。PSRAMがあっても内部RAMの制約は残ります。Flash書き込み中の電源断、共有Xrayの途中停止、長時間安定性は引き続き検証対象です。
