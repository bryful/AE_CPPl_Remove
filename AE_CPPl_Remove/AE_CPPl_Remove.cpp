#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace
{
volatile LONG cancelled = 0;
struct Interrupted {};
BOOL WINAPI controlHandler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT)
    {
        InterlockedExchange(&cancelled, 1);
        return TRUE;
    }
    return FALSE;
}
void checkCancel()
{
    if (InterlockedCompareExchange(&cancelled, 0, 0)) throw Interrupted{};
}
std::string utf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}
std::wstring lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}
std::wstring nativePath(const fs::path& path)
{
    auto value = fs::absolute(path).lexically_normal().wstring();
    if (value.starts_with(L"\\\\?\\")) return value;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
[[noreturn]] void winError(const char* operation)
{
    throw std::runtime_error(std::string(operation) + " (Win32=" + std::to_string(GetLastError()) + ")");
}
class File
{
    HANDLE handle = INVALID_HANDLE_VALUE;
public:
    File(const fs::path& path, bool output = false)
    {
        // 入力の変更と削除を、検証と確定が終わるまで禁止する。
        handle = CreateFileW(nativePath(path).c_str(), output ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
            output ? 0 : FILE_SHARE_READ, nullptr, output ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) winError(output ? "出力を作成できません" : "入力を開けません");
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() { close(); }
    void close()
    {
        if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
    }
    uint64_t size() const
    {
        LARGE_INTEGER value{};
        if (!GetFileSizeEx(handle, &value)) winError("ファイル長の取得失敗");
        return static_cast<uint64_t>(value.QuadPart);
    }
    void seek(uint64_t offset)
    {
        LARGE_INTEGER value{};
        value.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle, value, nullptr, FILE_BEGIN)) winError("シーク失敗");
    }
    void read(void* data, DWORD size)
    {
        auto* cursor = static_cast<unsigned char*>(data);
        while (size)
        {
            checkCancel();
            DWORD count = 0;
            if (!ReadFile(handle, cursor, size, &count, nullptr)) winError("読み込み失敗");
            if (!count) throw std::runtime_error("予期しないファイル終端");
            size -= count; cursor += count;
        }
    }
    void write(const void* data, DWORD size)
    {
        auto* cursor = static_cast<const unsigned char*>(data);
        while (size)
        {
            checkCancel();
            DWORD count = 0;
            if (!WriteFile(handle, cursor, size, &count, nullptr)) winError("書き込み失敗");
            if (!count) throw std::runtime_error("書き込みが進みません");
            size -= count; cursor += count;
        }
    }
    void flush()
    {
        if (!FlushFileBuffers(handle)) winError("ディスクへの保存失敗");
    }
};
uint32_t be32(const unsigned char* data)
{
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
}
void writeSize(File& file, uint64_t offset, uint32_t size)
{
    const std::array<unsigned char, 4> data{ static_cast<unsigned char>(size >> 24), static_cast<unsigned char>(size >> 16),
        static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size) };
    file.seek(offset); file.write(data.data(), 4);
}
bool tag(const unsigned char* bytes, const char* id)
{
    return std::equal(bytes, bytes + 4, id);
}
struct Chunk
{
    uint64_t offset;
    uint32_t size;
    unsigned depth;
};
struct Analysis
{
    uint64_t fileSize = 0;
    uint32_t rootSize = 0;
    std::vector<Chunk> cppl;
};
void scanChunks(File& file, uint64_t begin, uint64_t end, unsigned depth, Analysis& result)
{
    if (depth > 128) throw std::runtime_error("コンテナの階層が深すぎます");
    for (uint64_t offset = begin; offset < end;)
    {
        checkCancel();
        if (end - offset < 8) throw std::runtime_error("チャンクヘッダが途中で切れています");
        std::array<unsigned char, 8> header{};
        file.seek(offset); file.read(header.data(), 8);
        const uint32_t size = be32(header.data() + 4);
        const uint64_t physicalSize = uint64_t(8) + size + (size & 1);
        if (physicalSize > end - offset) throw std::runtime_error("チャンクが親の境界を超えています");
        if (tag(header.data(), "LIST"))
        {
            if (size < 4) throw std::runtime_error("LISTのサイズが4未満です");
            std::array<unsigned char, 4> type{};
            file.read(type.data(), 4);
            if (tag(type.data(), "CPPl")) result.cppl.push_back({ offset, size, depth });
            scanChunks(file, offset + 12, offset + 8 + size, depth + 1, result);
        }
        else if (tag(header.data(), "RIFX") || tag(header.data(), "RIFF"))
        {
            throw std::runtime_error("入れ子のRIFF/RIFXは未対応です");
        }
        offset += physicalSize;
    }
}
Analysis analyze(File& file)
{
    Analysis result;
    result.fileSize = file.size();
    if (result.fileSize < 12) throw std::runtime_error("AEPヘッダがありません");
    std::array<unsigned char, 12> header{};
    file.seek(0); file.read(header.data(), 12);
    if (!tag(header.data(), "RIFX") || !tag(header.data() + 8, "Egg!"))
        throw std::runtime_error("対応形式はRIFX / Egg!のAEPです");
    result.rootSize = be32(header.data() + 4);
    if (result.rootSize < 4 || uint64_t(result.rootSize) + 8 != result.fileSize)
        throw std::runtime_error("RIFXサイズと実ファイル長が一致しません");
    scanChunks(file, 12, result.fileSize, 0, result);
    return result;
}
void copyRange(File& source, File& destination, uint64_t start, uint64_t count)
{
    std::vector<unsigned char> buffer(1024 * 1024);
    source.seek(start);
    while (count)
    {
        checkCancel();
        const auto length = static_cast<DWORD>(std::min<uint64_t>(count, buffer.size()));
        source.read(buffer.data(), length); destination.write(buffer.data(), length);
        count -= length;
    }
}
void compareRange(File& source, File& output, uint64_t from, uint64_t to, uint64_t count)
{
    std::vector<unsigned char> left(1024 * 1024), right(left.size());
    source.seek(from); output.seek(to);
    while (count)
    {
        checkCancel();
        const auto length = static_cast<DWORD>(std::min<uint64_t>(count, left.size()));
        source.read(left.data(), length); output.read(right.data(), length);
        if (!std::equal(left.begin(), left.begin() + length, right.begin()))
            throw std::runtime_error("保存後のバイト比較に失敗しました");
        count -= length;
    }
}
struct TempFile
{
    fs::path path;
    ~TempFile()
    {
        if (!path.empty() && !DeleteFileW(nativePath(path).c_str()))
            std::cerr << "一時ファイルを削除できません: " << utf8(path.wstring()) << " (Win32=" << GetLastError() << ")\n";
    }
};
std::wstring timestamp()
{
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t text[32]{};
    swprintf_s(text, L"%04u%02u%02u_%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return text;
}
fs::path uniqueName(const fs::path& source, const std::wstring& suffix, unsigned index)
{
    return source.parent_path() / (source.stem().wstring() + suffix + (index ? L"_" + std::to_wstring(index) : L"") + L".aep");
}
void patchFile(File& source, const fs::path& path, const Analysis& original, bool overwrite)
{
    const auto chunk = original.cppl.front();
    const uint64_t removed = uint64_t(chunk.size) - 4 + (chunk.size & 1);
    TempFile temp;
    // CREATE_NEWにより、既存ファイルや同時実行の一時ファイルを保護する。
    for (unsigned i = 0;; ++i)
    {
        checkCancel();
        temp.path = path.parent_path() / (L".delCppl_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(i) + L".tmp");
        if (GetFileAttributesW(nativePath(temp.path).c_str()) == INVALID_FILE_ATTRIBUTES) break;
        temp.path.clear();
    }
    {
        // 作成に失敗した場合は、このプロセスが所有していないファイルを消さない。
        const auto candidate = temp.path; temp.path.clear();
        File output(candidate, true); temp.path = candidate;
        copyRange(source, output, 0, chunk.offset + 12);
        copyRange(source, output, chunk.offset + 8 + chunk.size + (chunk.size & 1),
            original.fileSize - (chunk.offset + 8 + chunk.size + (chunk.size & 1)));
        writeSize(output, 4, static_cast<uint32_t>(original.rootSize - removed));
        writeSize(output, chunk.offset + 4, 4);
        output.flush();
        const auto verified = analyze(output);
        if (verified.fileSize != original.fileSize - removed || verified.cppl.size() != 1 ||
            verified.cppl[0].offset != chunk.offset || verified.cppl[0].size != 4 || verified.cppl[0].depth != 0)
            throw std::runtime_error("保存後の構造検証に失敗しました");
        compareRange(source, output, 0, 0, 4);
        compareRange(source, output, 8, 8, chunk.offset - 4);
        compareRange(source, output, chunk.offset + 8, chunk.offset + 8, 4);
        compareRange(source, output, chunk.offset + 12 + removed, chunk.offset + 12,
            original.fileSize - chunk.offset - 12 - removed);
    }
    checkCancel();
    if (overwrite)
    {
        const auto suffix = L"_original_" + timestamp();
        source.close();
        for (unsigned i = 1;; ++i)
        {
            checkCancel();
            const auto backup = uniqueName(path, suffix, i);
            // 原本を退避してから出力を確定。この区間は中断を遅延する。
            if (MoveFileExW(nativePath(path).c_str(), nativePath(backup).c_str(), MOVEFILE_WRITE_THROUGH))
            {
                if (!MoveFileExW(nativePath(temp.path).c_str(), nativePath(path).c_str(), MOVEFILE_WRITE_THROUGH))
                {
                    const DWORD saveError = GetLastError();
                    if (!MoveFileExW(nativePath(backup).c_str(), nativePath(path).c_str(), MOVEFILE_WRITE_THROUGH))
                        throw std::runtime_error("置換・原本名の復元に失敗。原本退避先: " + utf8(backup.wstring()) +
                            " (置換Win32=" + std::to_string(saveError) + ", 復元Win32=" + std::to_string(GetLastError()) + ")");
                    SetLastError(saveError);
                    winError("置換失敗（原本名に復元しました）");
                }
                temp.path.clear();
                std::cout << "  保存: " << utf8(path.wstring()) << "\n  原本: " << utf8(backup.wstring()) << '\n';
                break;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS)
                winError("原本のリネーム退避に失敗しました");
        }
    }
    else
    {
        for (unsigned i = 0;; ++i)
        {
            checkCancel();
            const auto output = uniqueName(path, L"_patched", i);
            if (MoveFileExW(nativePath(temp.path).c_str(), nativePath(output).c_str(), MOVEFILE_WRITE_THROUGH))
            {
                temp.path.clear();
                std::cout << "  保存: " << utf8(output.wstring()) << '\n';
                break;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) winError("出力の確定失敗");
        }
    }
}
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
