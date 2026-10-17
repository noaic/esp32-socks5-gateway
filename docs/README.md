# Documentation / 文档 / ドキュメント

Start with the user guide: [English](../README.md) · [简体中文](../README.zh-CN.md) · [日本語](../README.ja.md). Hardware requirements, historical performance, upstream SOCKS5 requirements, first-use settings, and important limits stay in those landing guides.

先从用户指南开始；需要刷写、搭建环境或开发时，再选择下方对应语言的详细指南。ユーザーガイドから始め、書き込み・環境構築・開発が必要になったら以下の詳細ガイドを使ってください。

| Task / 任务 / 目的 | English | 简体中文 | 日本語 |
| --- | --- | --- | --- |
| Flash a package without ESP-IDF / 无需 SDK 刷写 / SDK なしで書き込み | [Package flashing](flash-prebuilt.md) | [固件包刷写](flash-prebuilt.zh-CN.md) | [パッケージ書き込み](flash-prebuilt.ja.md) |
| Install the exact SDK, prepare lwIP, build and flash / 环境、补丁、编译 / 環境、パッチ、ビルド | [Build and flash](build-and-flash.md) | [编译与刷写](build-and-flash.zh-CN.md) | [ビルドと書き込み](build-and-flash.ja.md) |
| Understand modules and run tests / 实现与测试 / 実装とテスト | [Development](development.md) | [开发指南](development.zh-CN.md) | [開発ガイド](development.ja.md) |

Current reference material:

- [Architecture](architecture.md): modules, packet path, ownership, and resource limits.
- [Maintenance and recovery](maintenance.md): configuration trials, UART, reset behavior, and recovery boundaries.
- [Performance and validation](performance.md): historical radio measurements, connection limits, released verification baseline, and remaining hardware checks.
- [Tool usage](../tools/README.md): preparation, serial capture, probes, benchmarks, and package creation.
- [License](../LICENSE) and [third-party notices](../THIRD_PARTY_NOTICES.md): original-project and upstream licensing; [Xray example notices](../examples/LICENSE.md) describe the separately licensed adapted template.
- [Development history](history/README.md): sanitized original notes, mainly in Chinese, including [dated audit/build records](history/audit-validation-2026-10.md) and reverted experiments. Historical statements about uncommitted builds or pending releases are not current project status.

The current guides live with the source so a checkout/tag keeps the matching build instructions. Wiki or community notes can supplement them; they do not replace the exact SDK, patch, and recovery instructions in this repository.
