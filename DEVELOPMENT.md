# 開発・検証

C++20、Windows API、Visual Studio 2026 / MSVC v145。外部ライブラリは使用しない。

アプリアイコンはassets/icons/delCppl.icoとdelCpplUI.ico。両プロジェクトのAppIcon.rcからリソースID101として埋め込む。GUIはタイトルバーとタスク切替用にも同じリソースを使用し、DPI変更時に適切なサイズを選び直す。ICOには16・20・24・32・40・48・64・128・256pxの32bit透過PNGを格納する。再生成は`powershell -ExecutionPolicy Bypass -File assets/generate-icons.ps1`。Windows標準のSystem.Drawingで描画するため追加パッケージは不要。512pxのPNGも同時に生成する。

ソリューションにはCLI版delCpplとGUI版delCpplUIの2プロジェクトがある。解析・キャッシュ・固定長コピー・出力検証はCpplCore.hで共有する。GUIはWin32のコントロールとIFileOpenDialog / IFileSaveDialogを使用し、解析・保存はワーカースレッドで実行する。D&DはWM_DROPFILESのインデックス0のみ参照する。処理中の終了は中断を要求してワーカー完了後に閉じる。

GUIの動作テストは`python tests/test_ui.py`。ウィンドウを非表示で起動し、情報表示、ファイル選択、保存ダイアログ、キャンセル、保存バイト一致、空・破損ファイルを検証する。Windowsのダイアログ初期化完了を待ってから操作する。D&Dの実メッセージテストはtests/test_drop.vcxprojをRelease/x64でビルドし、`tests/x64/Release/test_drop.exe sample/AAA.aep`を実行する。複数ファイルの先頭だけを処理し、先頭がフォルダなら後続ファイルへ進まないことを検証する。

GUIのカードとボタンはGDI+で角丸描画し、操作・フォーカス・読み取り専用テキストはWin32コントロールを使う。DWMで暗いタイトルバーと角丸を要求し、非対応OSでは通常フレームへフォールバックする。レジストリのAppsUseLightThemeを参照して自動テーマを決め、WM_SETTINGCHANGEなどで再適用する。明示テーマよりハイコントラスト設定を優先する。手動テーマの永続化やOS設定の変更は行わない。

`tests/preview_ui.vcxproj`をRelease/x64でビルドし、`tests/x64/Release/preview_ui.exe sample/AAA.aep`を実行すると、アプリと同一の描画処理でtests/ui_dark.pngとtests/ui_light.pngを書き出す。ウィンドウを表示せず描画を確認できる。これはクライアント領域の描画であり、DWMのタイトルバーは含まない。

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

チャンク解析には64KiBの読み取りキャッシュを使用し、小さなヘッダごとのSetFilePointerEx / ReadFile呼び出しを削減している。解析範囲・階層・サイズ・保存後の全バイト比較は従来通り検査する。キャッシュ境界をまたぐヘッダ・LIST型とキャッシュより大きなペイロードをテストする。

AAA.aep（約5MiB）を同一PC、ファイルキャッシュが温まった状態で測定したところ、7回の中央値で情報表示278.02ms→12.08ms、パッチ556.94ms→25.52ms。起動時間、保存後の再解析・バイト比較・flushを含む。大容量・低速ストレージなどでは効果が異なる。再測定は`python tests/benchmark.py`。比較用の旧exeをtests/delCppl_before.exeに置けば旧版も測定する。

実ファイルAAA.aepで判明した形式差に対応した。RIFXサイズはファイル全体ではなくRIFX本体の終端を示し、その後ろにXMPパケットが存在する場合がある。解析は本体境界で止め、後続のxpacket開始・終了マーカーを確認して、後続領域全体を変更せず保持する。これはXML全体の妥当性検証ではない。未知の後続データは拒否する。またLIST(btdk)は通常の子チャンク列として再帰解析せず、ヘッダのサイズ・親境界のみ検査する。AAA.aepのCPPl内容1,229,940バイトを除去し、後続XMPを含む対象外領域の一致を確認した。AEでの読込・描画検証は別途必要。
