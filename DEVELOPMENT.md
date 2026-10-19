# 開発・検証

C++20、Windows API、Visual Studio 2026 / MSVC v145。外部ライブラリは使用しない。

プロジェクトのTargetNameはdelCppl。Visual Studio標準の出力ディレクトリを使用する。MSBuildはx64ホストツールを指定する。

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' AE_CPPl_Remove.slnx /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64
python tests/test_cli.py
python tests/test_large_cancel.py
```

DebugもConfiguration=Debugでビルドする。通常テストはPython標準ライブラリで生成したAEP構造を使い、実物のAdobeプロジェクトによる互換性検証とは区別する。

`tests/test_large_cancel.py`はWindowsのコンソールに接続した状態で実行する。NTFSのスパースファイルを用いて3GiBのCPPlを試験し、コピー中にCTRL_BREAK_EVENTを送って終了コード130、一時ファイルの削除、原本と名前の保持を検査する。Ctrl+Cも同じ中断ハンドラで処理する。テスト用一時ディレクトリはtests直下に作成し、削除対象の親を検証してから片付ける。

解析は32ビットbig-endianのサイズ値と64ビット位置情報を用い、LIST境界を再帰的に検証する。階層の上限は128。LIST型がCPPlの場合だけ削除候補にし、文字列cpidの出現位置には依存しない。未知の通常チャンクのデータは保持する。

入力は共有書込・共有削除を許さずに開く。固定長バッファで一意な一時ファイルへ出力し、サイズ修正後にflush、再解析、対象外領域の全バイト比較を実施する。通常出力の確定と原本退避は既存ファイルを上書きしないWindows APIで行う。上書き時は入力ハンドルを閉じ、原本退避と出力確定を行い、失敗時に原本名へ復元する。このリネーム区間では他プロセスによる名前変更・強制終了などを完全には防止できないため、復元失敗時は原本の退避先を必ず報告する。

設計調査は[CPPl_RESEARCH.md](CPPl_RESEARCH.md)を参照。
