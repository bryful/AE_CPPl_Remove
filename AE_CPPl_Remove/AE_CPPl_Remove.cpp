#include "../CpplCore.h"

namespace
{
bool wildcard(const std::wstring& pattern, const std::wstring& name)
{
    const auto p = lower(pattern), n = lower(name);
    size_t i = 0, j = 0, star = std::wstring::npos, retry = 0;
    while (j < n.size())
    {
        if (i < p.size() && (p[i] == L'?' || p[i] == n[j])) { ++i; ++j; }
        else if (i < p.size() && p[i] == L'*') { star = i++; retry = j; }
        else if (star != std::wstring::npos) { i = star + 1; j = ++retry; }
        else return false;
    }
    while (i < p.size() && p[i] == L'*') ++i;
    return i == p.size();
}
bool generated(const fs::path& path)
{
    const auto name = lower(path.stem().wstring());
    const auto patched = name.rfind(L"_patched");
    if (patched != std::wstring::npos)
    {
        const auto tail = name.substr(patched + 8);
        if (tail.empty() || (tail[0] == L'_' && tail.size() > 1 &&
            std::all_of(tail.begin() + 1, tail.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))) return true;
    }
    const auto original = name.rfind(L"_original_");
    if (original == std::wstring::npos) return false;
    const auto tail = name.substr(original + 10);
    return tail.size() >= 17 && tail[8] == L'_' && tail[15] == L'_' &&
        std::all_of(tail.begin(), tail.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'_'; });
}
void collectDirectory(const fs::path& directory, const std::wstring& pattern, bool recursive, std::vector<fs::path>& files)
{
    checkCancel();
    for (const auto& entry : fs::directory_iterator(fs::path(nativePath(directory))))
    {
        checkCancel();
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) winError("属性取得失敗");
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (recursive) collectDirectory(entry.path(), pattern, true, files);
        }
        else if (lower(entry.path().extension().wstring()) == L".aep" && !generated(entry.path()) && wildcard(pattern, entry.path().filename().wstring()))
            files.push_back(entry.path());
    }
}
void collect(const fs::path& input, bool recursive, std::vector<fs::path>& files)
{
    const auto path = fs::absolute(input).lexically_normal();
    auto parentText = path.parent_path().wstring();
    if (parentText.starts_with(L"\\\\?\\")) parentText.erase(0, 4);
    if (parentText.find_first_of(L"*?") != std::wstring::npos)
        throw std::runtime_error("ワイルドカードはファイル名部分だけに指定してください");
    if (path.filename().wstring().find_first_of(L"*?") != std::wstring::npos)
    {
        collectDirectory(path.parent_path(), path.filename().wstring(), recursive, files);
    }
    else
    {
        const auto attributes = GetFileAttributesW(nativePath(path).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) winError("対象が見つかりません");
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("リンク・ジャンクションは対象外です");
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) collectDirectory(path, L"*.aep", recursive, files);
        else
        {
            if (lower(path.extension().wstring()) != L".aep") throw std::runtime_error("対象拡張子は.aepです");
            files.push_back(path);
        }
    }
}
void usage()
{
    std::cout << "delCppl <aep / フォルダ / ワイルドカード> [...] [オプション]\n"
        "  -info             CPPlのサイズを表示（既定）\n"
        "  -patch, -p, -f    CPPlの内容を削除\n"
        "  -sub              サブディレクトリも検索（リンクは追跡しない）\n"
        "  -overwrite        原本を退避して上書き（-patchが必要）\n"
        "  -wait             終了時にキー入力を待つ\n"
        "  -help, --help     この説明を表示\n"
        "通常出力: 名前_patched.aep（既存なら連番）\n"
        "ワイルドカードはファイル名の * と ? に対応。引用符で囲んでください。\n"
        "検索時は生成済みの_patched / _original_日時_連番を除外します。\n"
        "Ctrl+Cで中断。終了コード: 0=正常 1=失敗あり 2=引数不正 130=中断\n";
}
}
int run(int argc, wchar_t* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    if (!SetConsoleCtrlHandler(controlHandler, TRUE))
    {
        std::cerr << "中断ハンドラを登録できません\n"; return 1;
    }
    bool patch = false, recursive = false, overwrite = false;
    std::vector<fs::path> inputs;
    bool positional = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring value = argv[i];
        const auto option = lower(value);
        if (!positional && option == L"--") positional = true;
        else if (!positional && (option == L"-help" || option == L"--help" || option == L"-h")) { usage(); return 0; }
        else if (!positional && (option == L"-patch" || option == L"-p" || option == L"-f")) patch = true;
        else if (!positional && option == L"-sub") recursive = true;
        else if (!positional && option == L"-overwrite") overwrite = true;
        else if (!positional && option == L"-info") {}
        else if (!positional && option == L"-wait") {}
        else if (!positional && !value.empty() && value[0] == L'-')
        {
            std::cerr << "不明なオプション: " << utf8(value) << '\n'; return 2;
        }
        else inputs.emplace_back(value);
    }
    if (inputs.empty()) { usage(); return argc == 1 ? 0 : 2; }
    if (overwrite && !patch) { std::cerr << "-overwriteには-patch（-p / -f）が必要です\n"; return 2; }
    size_t failures = 0, changed = 0, unchanged = 0, inspected = 0;
    try
    {
        std::vector<fs::path> files;
        for (const auto& input : inputs)
        {
            checkCancel();
            const size_t before = files.size();
            try
            {
                collect(input, recursive, files);
                if (files.size() == before) { ++failures; std::cerr << utf8(input.wstring()) << ": 対象の.aepがありません\n"; }
            }
            catch (const std::exception& error) { ++failures; std::cerr << utf8(input.wstring()) << ": " << error.what() << '\n'; }
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return lower(nativePath(a)) < lower(nativePath(b)); });
        std::set<std::wstring> seen;
        for (const auto& path : files)
        {
            checkCancel();
            if (!seen.insert(lower(nativePath(path))).second) continue;
            std::cout << utf8(path.wstring()) << '\n';
            try
            {
                File source(path);
                const auto result = analyze(source);
                ++inspected;
                std::cout << "  ファイル: " << result.fileSize << " bytes / CPPl: " << result.cppl.size() << " 個\n";
                for (const auto& chunk : result.cppl)
                    std::cout << "  CPPl内容: " << chunk.size - 4 << " bytes / LISTサイズ: " << chunk.size
                        << " / 位置: " << chunk.offset << " / 階層: " << chunk.depth << '\n';
                if (!patch) continue;
                if (result.cppl.empty()) { ++unchanged; std::cout << "  変更不要（CPPlなし）\n"; continue; }
                if (result.cppl.size() != 1 || result.cppl.front().depth != 0)
                    throw std::runtime_error("複数または入れ子のCPPlは情報表示のみ対応しています");
                if (result.cppl.front().size == 4) { ++unchanged; std::cout << "  変更不要（CPPlは空です）\n"; continue; }
                patchFile(source, path, result, overwrite); ++changed;
            }
            catch (const std::exception& error) { ++failures; std::cerr << "  失敗: " << error.what() << '\n'; }
        }
        checkCancel();
    }
    catch (const Interrupted&)
    {
        std::cerr << "中断しました。確定済みの出力は保持されます。\n";
        std::cout << "確認: " << inspected << " / 削除: " << changed << " / 変更不要: " << unchanged << " / 失敗: " << failures << '\n';
        return 130;
    }
    catch (const std::exception& error) { ++failures; std::cerr << "失敗: " << error.what() << '\n'; }
    std::cout << "確認: " << inspected << " / 削除: " << changed << " / 変更不要: " << unchanged << " / 失敗: " << failures << '\n';
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t* argv[])
{
    bool wait = false;
    for (int i = 1; i < argc; ++i)
    {
        const auto option = lower(argv[i]);
        if (option == L"--") break;
        if (option == L"-wait") wait = true;
    }
    const int result = run(argc, argv);
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    // リダイレクト時と中断後は待たずに終了する。
    if (!wait || result == 130 || !GetConsoleMode(input, &mode)) return result;
    if (!FlushConsoleInputBuffer(input)) return result;
    std::cout << "終了するには何かキーを押してください . . ." << std::flush;
    while (!InterlockedCompareExchange(&cancelled, 0, 0))
    {
        const DWORD state = WaitForSingleObject(input, 100);
        if (state == WAIT_TIMEOUT) continue;
        if (state != WAIT_OBJECT_0) break;
        INPUT_RECORD record{};
        DWORD count = 0;
        if (!ReadConsoleInputW(input, &record, 1, &count)) break;
        if (count && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown)
        {
            const auto key = record.Event.KeyEvent.wVirtualKeyCode;
            if (key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU) break;
        }
    }
    std::cout << '\n';
    return InterlockedCompareExchange(&cancelled, 0, 0) ? 130 : result;
}
