#pragma once
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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
class ChunkReader
{
    File& file;
    const uint64_t fileSize;
    std::vector<unsigned char> buffer;
    uint64_t cacheOffset = 0;
    size_t cacheSize = 0;
public:
    explicit ChunkReader(File& input, uint64_t size) : file(input), fileSize(size), buffer(64 * 1024) {}
    void readAt(uint64_t offset, void* destination, size_t length)
    {
        checkCancel();
        if (offset > fileSize || length > fileSize - offset || length > buffer.size())
            throw std::runtime_error("チャンクの読み込み範囲が不正です");
        // 小さなヘッダごとのシークとReadFileをまとめ、検査内容は維持する。
        if (offset < cacheOffset || offset - cacheOffset > cacheSize || length > cacheSize - static_cast<size_t>(offset - cacheOffset))
        {
            cacheOffset = offset;
            cacheSize = static_cast<size_t>(std::min<uint64_t>(buffer.size(), fileSize - offset));
            file.seek(offset);
            file.read(buffer.data(), static_cast<DWORD>(cacheSize));
        }
        std::memcpy(destination, buffer.data() + static_cast<size_t>(offset - cacheOffset), length);
    }
};
void scanChunks(ChunkReader& reader, uint64_t begin, uint64_t end, unsigned depth, Analysis& result)
{
    if (depth > 128) throw std::runtime_error("コンテナの階層が深すぎます");
    for (uint64_t offset = begin; offset < end;)
    {
        checkCancel();
        if (end - offset < 8) throw std::runtime_error("チャンクヘッダが途中で切れています");
        std::array<unsigned char, 8> header{};
        reader.readAt(offset, header.data(), 8);
        const uint32_t size = be32(header.data() + 4);
        const uint64_t physicalSize = uint64_t(8) + size + (size & 1);
        if (physicalSize > end - offset) throw std::runtime_error("チャンクが親の境界を超えています");
        if (tag(header.data(), "LIST"))
        {
            if (size < 4) throw std::runtime_error("LISTのサイズが4未満です");
            std::array<unsigned char, 4> type{};
            reader.readAt(offset + 8, type.data(), 4);
            if (tag(type.data(), "CPPl")) result.cppl.push_back({ offset, size, depth });
            // btdkはLIST形式のヘッダを持つが、内容はチャンク列ではない。
            if (!tag(type.data(), "btdk"))
                scanChunks(reader, offset + 12, offset + 8 + size, depth + 1, result);
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
    const uint64_t rootEnd = uint64_t(result.rootSize) + 8;
    if (result.rootSize < 4 || rootEnd > result.fileSize)
        throw std::runtime_error("RIFXサイズが不正、またはファイルが途中で切れています");
    if (rootEnd < result.fileSize)
    {
        // AEPはRIFXの外側にXMPパケットを付加する。終端まで不変でコピーする。
        const uint64_t trailerSize = result.fileSize - rootEnd;
        if (trailerSize < 34) throw std::runtime_error("RIFX後方のデータが未対応です");
        std::array<char, 16> begin{};
        file.seek(rootEnd); file.read(begin.data(), static_cast<DWORD>(begin.size()));
        const auto tailLength = static_cast<DWORD>(std::min<uint64_t>(trailerSize, 64));
        std::string tail(tailLength, '\0');
        file.seek(result.fileSize - tailLength); file.read(tail.data(), tailLength);
        const auto last = tail.find_last_not_of(" \t\r\n");
        if (last != std::string::npos) tail.resize(last + 1);
        if (std::string(begin.data(), begin.size()) != "<?xpacket begin=" ||
            (!tail.ends_with("<?xpacket end=\"w\"?>") && !tail.ends_with("<?xpacket end=\"r\"?>") &&
             !tail.ends_with("<?xpacket end='w'?>") && !tail.ends_with("<?xpacket end='r'?>")))
            throw std::runtime_error("RIFX後方にXMPパケット以外のデータがあります");
    }
    ChunkReader reader(file, result.fileSize);
    scanChunks(reader, 12, rootEnd, 0, result);
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
void writePatched(File& source, File& output, const Analysis& original)
{
    const auto chunk = original.cppl.front();
    const uint64_t removed = uint64_t(chunk.size) - 4 + (chunk.size & 1);
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
void patchFile(File& source, const fs::path& path, const Analysis& original, bool overwrite)
{
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
        writePatched(source, output, original);
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
}
