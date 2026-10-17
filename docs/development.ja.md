# 実装と開発ガイド

[English](development.md) · [简体中文](development.zh-CN.md) · [日本語](development.ja.md)

ゲートウェイの設定は[ユーザーガイド](../README.ja.md)、ビルドは[ソースビルドガイド](build-and-flash.ja.md)を参照してください。本ページでは実装、ソース構成、ホスト回帰テスト、任意ツールを説明します。特記がなければコマンドはプロジェクトのルートで実行します。

## 実装を簡単に理解する

`main/` は ESP-IDF の Wi-Fi、`esp_netif`、socket、HTTP、NVS API を使ってゲートウェイの動作を定義します。**FreeRTOS は ESP-IDF 内蔵のスケジューラー**で、TCP/UDP や保守タスク、キュー、ロックを扱います。**lwIP は TCP/IP スタック**で、IPv4、TCP 接続交渉、再送などを担当します。アプリが転送方針を決め、SDK がドライバーとサービスを提供し、FreeRTOS が実行を調整し、lwIP が通信を処理します。別の OS をボードに導入する必要はありません。

ESP32 を、データを受け取り、相手に渡す中継役として考えると分かりやすくなります。

- **TCP** は、多くの Web 接続などで使う、確実な転送を行う仕組みです。ゲートウェイは端末が接続したい相手を記録し、SOCKS5 `CONNECT` で代理接続を依頼して、応答を端末へ返します。
- **UDP** は、通常の DNS 問い合わせや一部のリアルタイム通信で使います。`UDP ASSOCIATE` で中継先を取得し、宛先情報を付けて転送します。別々の通信はそれぞれの状態を保持します。
- **DNS** は、名前を IP アドレスに変換します。既定の DHCP は端末に `1.1.1.1` を通知します。ESP32 にローカル DNS サービスはなく、通常の DNS UDP 通信も SOCKS5 経由です。
- **NVS** は、Flash に設定を保存する領域です。そのため通常の再起動で設定が残り、アプリの再書き込みでも既存のネットワーク設定が自動では置き換わりません。

全体の容量は、同時に中継する TCP 接続が 64、UDP flow が 32 です。宛先と終了処理を記録する 512 件の対応表には、端末スロットごとに 256 件の保持記録、16 件の待ち受け側で未受け付けの記録という上限もあります。**TCP を 512 接続同時に転送できる意味ではありません**。再送など TCP の基本動作は lwIP が担当し、アプリの固定ループで複数の接続を処理します。

## Web ページへの TCP 接続を追う

**なぜ `ip4.c` を変更する？** 端末の Web 要求は外部ホスト宛てなので、通常のローカル socket ではその TCP 接続を受け取れません。パッチは IPv4 検証後、ローカルアドレス判定前に入力フックを入れ、宛先の記録と変更を可能にします。出力側はヘッダー準備後、ネットワークインターフェースへ渡す前に元のアドレスへ戻します。変更には独立したパケットコピーを使い、lwIP の再送用データを壊しません。SOCKS5 socket を開くだけではこの透過取り込み・アドレス復元はできず、アプリが TCP 全体を実装し直す必要もありません。[ESP-IDF の lwIP 説明](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/lwip.html)も参照してください。

スマートフォンが Web サイトの `203.0.113.10:443` に接続するとします。このアドレスは説明用の例です。

1. スマートフォンは、デフォルトゲートウェイである ESP32 にパケットを送ります。
2. プロジェクト内の lwIP パッチが IPv4 の処理経路からゲートウェイのコードを呼びます。パケットを検証し、端末と元の宛先を記録して、ESP32 のローカル待ち受けポートへ向け直します。受信側が検証できるようチェックサムも更新します。
3. lwIP がローカル TCP 接続を処理し、アプリは socket からデータを読みます。socket はプログラムがネットワークデータを送受信するためのインターフェースで、アプリ自身が TCP の接続交渉や再送を実装する必要はありません。
4. TCP の転送ループが上流と SOCKS5 NO AUTH を交渉し、`CONNECT` で元の宛先への接続を依頼します。その後、2 つの接続間でデータを中継します。
5. スマートフォンへ返す TCP パケットは、対応表から元の接続先アドレスへ戻し、チェックサムを修正します。端末からは Web サイトとの接続に見えます。

```mermaid
flowchart LR
    Client["端末の TCP パケット"] --> Hook["IPv4 フック<br/>宛先を記録してアドレスを変更"]
    Hook --> Stack["lwIP のローカル接続"]
    Stack --> Relay["TCP 転送ループ"]
    Relay -->|SOCKS5 CONNECT| Proxy["上流プロキシ"]
    Proxy --> Site["元の Web サイト"]
```

「透過」は端末からの使い方を示します。元の TCP パケットをそのまま SOCKS5 に入れるのではなく、lwIP が接続を処理してから、その中のデータを中継します。HTTPS の暗号化はブラウザーと Web サイトが担当し、ゲートウェイはページ内容を復号しません。

## UDP、並行処理、設定保存の仕組み

UDP 自体には TCP のような接続手順がありませんが、SOCKS5 の UDP 転送には TCP の制御接続が必要です。各 UDP flow は端末と宛先を記録し、`UDP ASSOCIATE` を交渉して、SOCKS5 の宛先ヘッダー付き UDP データを中継先へ送ります。返信は flow と端末の所属を確認してから配信します。DNS 用に一部の容量と交渉機会を確保し、ほかの UDP 通信が資源を使い切って名前解決ができなくなる状況を減らしています。

TCP と UDP はそれぞれ固定の転送ループを使います。今送受信できる接続を確認し、一部のデータを処理して次の接続に移ります。接続ごとにタスクは作りません。キュー容量と待ち時間には上限があり、大きな TCP データバッファーは PSRAM、頻繁に参照する TCP 状態は内部 RAM に置きます。メモリ使用量を制御する仕組みで、無線帯域や接続総数の上限がなくなるわけではありません。

設定管理は現在の値と以前の値を保存します。新しいネットワークは試用後に確定し、時間切れなら元へ戻します。管理 / 保守タスクは転送ループと分かれており、状態ページを開くたびにプロキシを再起動しません。各端末の接続識別情報と通信量カウンターを使い、返信の所属や、切断した端末の接続を判別します。

端末は ESP32 の DHCP から IPv4 アドレスを取得する必要があります。手動の固定 IP は転送用の端末一覧に登録されません。接続識別子は再接続後の古い返信を区別しますが、IP 偽装を防ぐ認証ではありません。通信量は中継した TCP/UDP の **ペイロードのバイト数**を数え、プロトコルヘッダーを含みません。無線全体のバイト数や、アプリが受信済みという証明ではありません。

管理サーバーはリクエストごとのネットワーク受信/送信を **5 秒**の期限で制限し、本文は 1024 バイトまでです。拒否したリクエストや未読の本文がある場合は接続を閉じ、任意の長さのデータを読み捨てません。同じ HTTP タスクで処理する設定のロールバックに対するネットワーク待機の影響を抑えます。状態の更新で未保存の Internet チェック設定を上書きせず、イベント名はページの言語に合わせて表示します。

ソースを読む場合は、調べたい部分に対応するファイルから始められます。

| 知りたい部分 | 主なソース |
| --- | --- |
| 起動、AP/STA、ネットワークイベント | [app_main.c](../main/app_main.c) |
| TCP 入力、アドレス変更、宛先記録 | [tcp_poc.c](../main/tcp_poc.c)、[packet_rewrite.c](../main/packet_rewrite.c)、[flow_table.c](../main/flow_table.c) |
| TCP の SOCKS5 交渉と双方向転送 | [socks_relay.c](../main/socks_relay.c) |
| UDP の受け取り、関連付け、データ形式 | [udp_gateway.c](../main/udp_gateway.c)、[udp_relay.c](../main/udp_relay.c)、[socks_udp.c](../main/socks_udp.c) |
| 端末の所属と通信量 | [ap_clients.c](../main/ap_clients.c) |
| 設定保存、確定、復旧 | [gateway_settings.c](../main/gateway_settings.c)、[web_management.c](../main/web_management.c) |
| HTTP の通信期限と接続の終了処理 | [management_http.c](../main/management_http.c) |
| 保守タスク、接続検査 | [gateway_control.c](../main/gateway_control.c)、[gateway_probe.c](../main/gateway_probe.c) |

全体の経路と担当は [アーキテクチャ](architecture.md)を参照してください。初回は設定手順を理解し、必要になった部分から実装を読めば十分です。

詳細な CPU/実行時間統計、RX バッファー参照追跡、Wi-Fi TX 完了の観測は `CONFIG_PROXY_VERBOSE_DIAGNOSTICS` でのみ有効にし、基本 Web/エラーカウンターは残します。既定ソース設定では細かな計測を無効にしますが、既存 `sdkconfig` は以前の選択を保持します。比較前に確認し、新たな実機測定なしに速度向上を主張しないでください。

## 既存の回帰テストを実行する

「ホストテスト」は開発用パソコンで C コードの一部をコンパイルし、実行するテストです。ESP32 ボードは不要です。先にファームウェアを 1 回正常にビルドし、指定した cJSON が `managed_components/` に取得されていることを確認してください。

Linux で ESP-IDF を有効にしたプロジェクトのターミナルを使い、システム GCC/binutils と Node.js を用意して実行します。

```bash
# PATH を変える前に、使用中の Python の場所を保存します。
HOST_PYTHON="$(command -v python)"
PATH=/usr/bin:/bin:$PATH "$HOST_PYTHON" -m unittest discover -s tests -v
node tests/management_feedback_harness.js
node tests/management_transport_harness.js
```

ESP-IDF の Python 環境には、UART テストで使う pyserial/esptool が含まれます。全スイートを実行し、実際のテスト数、失敗、スキップを確認してください。ファームウェア/ツールのホスト回帰、lwIP 準備スクリプト、2 組のフロントエンドの操作結果/互換性テストを含みます。準備スクリプトのテストは一時 Git リポジトリで LF/CRLF、繰り返し操作、読み取り専用検証、想定外の編集の拒否を確認します。HTTP の期限は実際のホスト socket、シリアルのエコー/プロンプトは疑似端末と模擬 UART ドライバーで検証します。実行数、失敗、スキップを確認してください。成功しても、実際の Wi-Fi、電源断、長時間のボード動作を検証したことにはなりません。

## 任意の実ブラウザーテスト

前述の Node テストはブラウザーも npm 依存も不要です。Linux 上で実際の HTML を Chromium で確認する場合は、Playwright をホストの一時フォルダーへ導入できます。模擬 API で英語/中国語のデスクトップ/携帯幅を検証し、ボードにはアクセスしません。ブラウザーのダウンロードと Chromium のシステムライブラリーは任意の開発依存です。

```bash
BROWSER_TEST_TOOLS="$(mktemp -d)"
npm install --prefix "$BROWSER_TEST_TOOLS" --no-package-lock playwright@1.62.1
PLAYWRIGHT_BROWSERS_PATH="$BROWSER_TEST_TOOLS/browsers" "$BROWSER_TEST_TOOLS/node_modules/.bin/playwright" install chromium
NODE_PATH="$BROWSER_TEST_TOOLS/node_modules" PLAYWRIGHT_BROWSERS_PATH="$BROWSER_TEST_TOOLS/browsers" node tests/management_browser_harness.js
```

## tools の簡単な使い分け

プロジェクトのルートで実行します。ビルドに必須なのは lwIP の準備だけで、ほかは任意の調査/開発ツールです。完全なコマンド、プラットフォームの制限、テスト条件は [ツールの使用ガイド（英語）](../tools/README.md)を参照してください。

| ツール | 用途 |
| --- | --- |
| `package_firmware.py`、`flash_firmware.py` | 完全/アプリイメージとスクリプトなし ZIP を作成。リポジトリ内の任意ツールで確認/個別イメージを書き込み |
| `prepare_lwip.py` | `apply` でコピー準備、`check` で検証、`restore` でローカルパッチを復元 |
| `uart_capture.py` | シリアルログ保存。Linux/WSL2 は `--interactive` でコマンド送信も可能 |
| `probe_upstream_udp.py`、`probe_upstream_quic.py` | パソコンから SOCKS UDP/DNS または QUIC を直接検査。ESP32 は通りません |
| `test_stream_server.py`、`throughput_bench.html` | LAN のテストサービス/ページを起動し、AP 端末から速度測定 |
| `test_transparent_echo.py`、`test-transparent-echo.ps1` | 専用の echo 宛先で透過 TCP を検査。通常の HTTPS サイトは echo サービスではありません |
| `collect_link_bench.py`、`snapshot_manifest.py` | 対応する診断 UART レポートの抽出、ローカルファイル一覧の作成/検証 |

## ディレクトリと参考資料

| パス | 内容 |
| --- | --- |
| `main/` | ファームウェアと組み込み管理ページ |
| `patches/` | lwIP パッチとリビジョン / ハッシュ情報 |
| `tools/` | パッチ準備、UART 記録、ネットワーク調査、速度測定 |
| `tests/` | C/Python ホストテストと JavaScript フロントエンドテスト |
| `experiments/` | ホストテストで使う、保管したバッファー/返信実験モジュール。ファームウェアにはリンクしません |
| `examples/` | Xray の完全なクライアント設定例、配置ガイド、部分設定 |
| `sdkconfig.defaults` | ハードウェア / 機能の初期設定 |
| `dependencies.lock` | ファームウェア部品のバージョン記録 |
| `docs/` | 設計、保守、性能、開発記録 |

- [ドキュメント索引](README.md)
- [保守と復旧](maintenance.md)
- [性能と検証](performance.md)：測定記録、メモリのサンプル値、検証の範囲。
- [開発履歴](history/README.md)：後で取り消した実験も含まれます。日常の動作は[ユーザーガイド](../README.ja.md)と、このディレクトリの現行ガイドを優先してください。

`sdkconfig`、`build/`、`managed_components/`、プロジェクト内の lwIP コピー、テスト出力はツールが生成します。コミット前に `git status` を確認し、Wi-Fi パスワード、個人設定、ログ、ローカルのファームウェア成果物を公開しないでください。

## シリアル記録

Linux/WSL2 でログの保存と行単位のコマンド送信を 1 つの接続で行うには、プロジェクトのルートで実行します。

```bash
python tools/uart_capture.py --port /dev/ttyACM0 --no-reset --interactive --timestamps --seconds 0
```

`--interactive` の標準入力処理は POSIX 専用で、Enter 後に行全体を送信します。Windows のコマンド入力には `idf.py -p COM5 monitor` またはシリアルターミナルを使ってください。ログ保存だけなら `--port COM5` を指定し、`--interactive` を付けずに使えます。タイムスタンプ付きでも、改行前のプロンプトとエコーをすぐに表示します。先にほかのシリアルツールを終了してください。`--seconds 0` は中断まで読み続けます。`--no-reset` はツールによる能動的なリセットを避けますが、アダプターやドライバーによってリセット信号が影響を受ける場合はあります。
