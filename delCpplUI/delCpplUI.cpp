#include "../CpplCore.h"
#include "../assets/resource.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <sstream>
#include <thread>
#include <memory>
#include <gdiplus.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <iomanip>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
constexpr UINT WORK_DONE = WM_APP + 1;
constexpr int SELECT_ID = 1001, SAVE_ID = 1002, CANCEL_ID = 1003, THEME_ID = 1004;
struct Result
{
    Analysis analysis;
    std::wstring text;
    bool success = false;
    bool saved = false;
    bool interrupted = false;
};
struct Palette
{
    COLORREF background{}, surface{}, raised{}, border{}, text{}, muted{}, accent{}, accentText{}, hover{}, disabled{};
};
struct App
{
    HWND window{}, hint{}, pathBox{}, selectButton{}, infoBox{}, saveButton{}, cancelButton{}, status{}, themeButton{};
    HFONT font{}, titleFont{}, numberFont{}, labelFont{};
    HBRUSH backgroundBrush{}, surfaceBrush{};
    Palette colors;
    int theme = 0;
    bool dark = false, highContrast = false;
    fs::path input;
    Analysis analysis;
    std::wstring info;
    std::thread worker;
    bool busy = false, ready = false, closing = false;
    unsigned dpi = 96;
    ~App()
    {
        if (worker.joinable()) worker.join();
        for (auto item : { font, titleFont, numberFont, labelFont }) if (item) DeleteObject(item);
        if (backgroundBrush) DeleteObject(backgroundBrush);
        if (surfaceBrush) DeleteObject(surfaceBrush);
    }
};
std::wstring wide(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}
std::wstring displayPath(const fs::path& path)
{
    auto text = path.wstring();
    if (text.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + text.substr(8);
    if (text.starts_with(L"\\\\?\\")) return text.substr(4);
    return text;
}
std::wstring describe(const Analysis& value)
{
    std::wostringstream text;
    text << L"ファイルサイズ: " << value.fileSize << L" bytes\r\n"
         << L"CPPl: " << value.cppl.size() << L" 個\r\n";
    for (const auto& chunk : value.cppl)
        text << L"\r\nCPPl内容: " << chunk.size - 4 << L" bytes\r\n"
             << L"LISTサイズ: " << chunk.size << L" bytes\r\n"
             << L"位置: " << chunk.offset << L" / 階層: " << chunk.depth << L"\r\n";
    if (value.cppl.empty()) text << L"\r\n変更不要: CPPlはありません。";
    else if (value.cppl.size() != 1 || value.cppl.front().depth != 0)
        text << L"\r\n複数または入れ子のCPPlは情報表示のみ対応しています。";
    else if (value.cppl.front().size == 4) text << L"\r\n変更不要: CPPlは空です。";
    else text << L"\r\n実行ボタンから保存先を選択してください。\r\n原本を保持し、CPPlの内容を削除したファイルを保存します。";
    return text.str();
}
bool canPatch(const Analysis& value)
{
    return value.cppl.size() == 1 && value.cppl.front().depth == 0 && value.cppl.front().size > 4;
}
void setBusy(App& app, bool busy, const wchar_t* message)
{
    app.busy = busy;
    EnableWindow(app.selectButton, !busy);
    EnableWindow(app.saveButton, !busy && app.ready);
    EnableWindow(app.cancelButton, busy);
    DragAcceptFiles(app.window, !busy);
    SetWindowTextW(app.status, message);
    InvalidateRect(app.window, nullptr, FALSE);
}
void sendResult(HWND window, std::unique_ptr<Result> result)
{
    if (PostMessageW(window, WORK_DONE, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
}
void openFile(App& app, const fs::path& input)
{
    if (app.busy) return;
    app.ready = false;
    app.input = fs::absolute(input).lexically_normal();
    SetWindowTextW(app.pathBox, displayPath(app.input).c_str());
    app.info.clear();
    app.analysis = {};
    SetWindowTextW(app.infoBox, L"CPPlを確認しています…");
    InterlockedExchange(&cancelled, 0);
    setBusy(app, true, L"解析中…");
    app.worker = std::thread([window = app.window, path = app.input]()
    {
        auto result = std::make_unique<Result>();
        try
        {
            const auto attributes = GetFileAttributesW(nativePath(path).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) winError("ファイルが見つかりません");
            if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("通常のAEPファイルを1つ指定してください。フォルダ・リンクは対象外です");
            if (lower(path.extension().wstring()) != L".aep") throw std::runtime_error("対象拡張子は.aepです");
            File source(path);
            result->analysis = analyze(source);
            result->text = describe(result->analysis);
            result->success = true;
        }
        catch (const Interrupted&) { result->interrupted = true; result->text = L"解析を中断しました。"; }
        catch (const std::exception& error) { result->text = L"解析失敗: " + wide(error.what()); }
        sendResult(window, std::move(result));
    });
}
// COMオブジェクトはダイアログの終了経路によらず解放する。
template<class T> struct ComPtr
{
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T* operator->() const { return value; }
};
void checkHr(HRESULT hr, const char* message)
{
    if (FAILED(hr)) throw std::runtime_error(std::string(message) + " (HRESULT=" + std::to_string(static_cast<unsigned long>(hr)) + ")");
}
bool chooseFile(App& app, bool save, fs::path& selected)
{
    ComPtr<IFileDialog> dialog;
    checkHr(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog.value)), "ファイルダイアログを開けません");
    DWORD options{};
    checkHr(dialog->GetOptions(&options), "ダイアログ設定の取得失敗");
    options = (options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR) & ~FOS_ALLOWMULTISELECT;
    options |= save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
    checkHr(dialog->SetOptions(options), "ダイアログ設定失敗");
    const COMDLG_FILTERSPEC types[] = { { L"After Effects プロジェクト (*.aep)", L"*.aep" } };
    checkHr(dialog->SetFileTypes(1, types), "ファイル種類の設定失敗");
    checkHr(dialog->SetDefaultExtension(L"aep"), "拡張子の設定失敗");
    checkHr(dialog->SetTitle(save ? L"CPPl削除後のAEPを保存" : L"AEPファイルを選択"), "タイトルの設定失敗");
    if (save)
    {
        checkHr(dialog->SetFileName((app.input.stem().wstring() + L"_patched.aep").c_str()), "保存名の設定失敗");
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(app.input.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder.value))))
            checkHr(dialog->SetFolder(folder.value), "保存フォルダの設定失敗");
    }
    const HRESULT shown = dialog->Show(app.window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return false;
    checkHr(shown, "ファイル選択に失敗しました");
    ComPtr<IShellItem> item;
    checkHr(dialog->GetResult(&item.value), "選択結果の取得失敗");
    PWSTR path = nullptr;
    checkHr(item->GetDisplayName(SIGDN_FILESYSPATH, &path), "パスの取得失敗");
    try { selected = path; }
    catch (...) { CoTaskMemFree(path); throw; }
    CoTaskMemFree(path);
    return true;
}
void saveFile(App& app, const fs::path& destination)
{
    const auto outputPath = fs::absolute(destination).lexically_normal();
    if (lower(nativePath(outputPath)) == lower(nativePath(app.input)) ||
        (fs::exists(outputPath) && fs::equivalent(app.input, outputPath)))
        throw std::runtime_error("原本を保持するため、入力ファイルとは別の保存先を選んでください");
    if (lower(outputPath.extension().wstring()) != L".aep") throw std::runtime_error("保存先の拡張子は.aepにしてください");
    InterlockedExchange(&cancelled, 0);
    setBusy(app, true, L"CPPlを削除して保存・検証中…");
    app.worker = std::thread([window = app.window, input = app.input, outputPath]()
    {
        auto result = std::make_unique<Result>();
        result->saved = true;
        try
        {
            File source(input);
            // 選択から保存までの間に変わる可能性があるため、入力を再解析する。
            const auto analysis = analyze(source);
            if (!canPatch(analysis)) throw std::runtime_error("削除できるCPPlがありません。ファイルを選び直してください");
            TempFile temp;
            for (unsigned i = 0;; ++i)
            {
                checkCancel();
                const auto candidate = outputPath.parent_path() / (L".delCpplUI_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(i) + L".tmp");
                if (GetFileAttributesW(nativePath(candidate).c_str()) != INVALID_FILE_ATTRIBUTES) continue;
                {
                    File output(candidate, true); temp.path = candidate;
                    writePatched(source, output, analysis);
                }
                break;
            }
            checkCancel();
            if (!MoveFileExW(nativePath(temp.path).c_str(), nativePath(outputPath).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                winError("保存先への確定に失敗しました");
            temp.path.clear();
            result->success = true;
            result->text = L"保存しました: " + displayPath(outputPath) + L"\r\n原本は保持されています。";
        }
        catch (const Interrupted&) { result->interrupted = true; result->text = L"保存を中断しました。原本は保持されています。"; }
        catch (const std::exception& error) { result->text = L"保存失敗: " + wide(error.what()); }
        sendResult(window, std::move(result));
    });
}
int px(const App& app, int value) { return MulDiv(value, static_cast<int>(app.dpi), 96); }
void updateIcon(App& app)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    for (const bool isSmall : { false, true })
    {
        const int width = GetSystemMetricsForDpi(isSmall ? SM_CXSMICON : SM_CXICON, app.dpi);
        const int height = GetSystemMetricsForDpi(isSmall ? SM_CYSMICON : SM_CYICON, app.dpi);
        const auto icon = LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, width, height, LR_SHARED);
        if (icon) SendMessageW(app.window, WM_SETICON, isSmall ? ICON_SMALL : ICON_BIG, reinterpret_cast<LPARAM>(icon));
    }
}
Gdiplus::Color color(COLORREF value)
{
    return Gdiplus::Color(255, GetRValue(value), GetGValue(value), GetBValue(value));
}
void rounded(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border)
{
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x = static_cast<float>(rect.left) + .5f, y = static_cast<float>(rect.top) + .5f;
    const float width = static_cast<float>(rect.right - rect.left) - 1, height = static_cast<float>(rect.bottom - rect.top) - 1;
    const float diameter = static_cast<float>(radius * 2);
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(color(fill)); graphics.FillPath(&brush, &path);
    Gdiplus::Pen pen(color(border), 1); graphics.DrawPath(&pen, &path);
}
void drawText(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF ink, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS)
{
    const auto oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, ink);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}
std::wstring sizeText(uint64_t bytes)
{
    std::wostringstream text;
    if (bytes < 1024) text << bytes << L" B";
    else if (bytes < 1024 * 1024) text << std::fixed << std::setprecision(1) << static_cast<double>(bytes) / 1024 << L" KB";
    else text << std::fixed << std::setprecision(2) << static_cast<double>(bytes) / (1024 * 1024) << L" MB";
    return text.str();
}
void applyTheme(App& app)
{
    DWORD light = 1, length = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &length);
    HIGHCONTRASTW contrast{ sizeof(contrast) };
    app.highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
    app.dark = app.theme == 1 || (app.theme == 0 && !light);
    if (app.highContrast)
    {
        app.colors = { GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_BTNFACE), GetSysColor(COLOR_WINDOWTEXT),
            GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHTTEXT),
            GetSysColor(COLOR_BTNFACE), GetSysColor(COLOR_GRAYTEXT) };
    }
    else if (app.dark)
    {
        app.colors = { RGB(18,20,25), RGB(27,30,37), RGB(38,42,51), RGB(53,58,70), RGB(239,241,247), RGB(158,167,185),
            RGB(177,161,255), RGB(26,18,51), RGB(49,54,66), RGB(110,118,135) };
    }
    else
    {
        app.colors = { RGB(245,246,250), RGB(255,255,255), RGB(241,242,248), RGB(221,225,234), RGB(30,34,46), RGB(103,112,132),
            RGB(106,75,220), RGB(255,255,255), RGB(232,234,243), RGB(142,150,167) };
    }
    if (app.backgroundBrush) DeleteObject(app.backgroundBrush);
    if (app.surfaceBrush) DeleteObject(app.surfaceBrush);
    app.backgroundBrush = CreateSolidBrush(app.colors.background);
    app.surfaceBrush = CreateSolidBrush(app.colors.surface);
    const BOOL dark = app.dark && !app.highContrast;
    DwmSetWindowAttribute(app.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(app.window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    SetWindowTextW(app.themeButton, app.theme == 0 ? L"テーマ: 自動" : app.theme == 1 ? L"テーマ: ダーク" : L"テーマ: ライト");
    for (auto edit : { app.pathBox, app.infoBox }) SetWindowTheme(edit, L"", L"");
    RedrawWindow(app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}
void paint(App& app, HDC dc)
{
    RECT client{}; GetClientRect(app.window, &client);
    FillRect(dc, &client, app.backgroundBrush);
    const auto& c = app.colors;
    const int margin = px(app, 28), width = client.right;
    const auto rect = [&](int x, int y, int w, int h) { return RECT{ x, px(app,y), x+w, px(app,y+h) }; };
    drawText(dc, L"delCppl", rect(margin, 22, px(app,200), 40), app.titleFont, c.text);
    rounded(dc, rect(margin+px(app,151), 30, px(app,36), 23), px(app,5), c.raised, c.border);
    drawText(dc, L"UI", rect(margin+px(app,151), 30, px(app,36), 23), app.labelFont, c.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    drawText(dc, L"After Effects プロジェクトのCPPlを整理", rect(margin, 69, width-2*margin, 23), app.font, c.muted);
    rounded(dc, rect(margin, 112, width-2*margin, 158), px(app,14), c.surface, c.border);
    drawText(dc, L"01  /  プロジェクト", rect(margin+px(app,22), 127, width-2*margin-px(app,44), 24), app.labelFont, c.text);
    const int gap = px(app,12), metricWidth = (width-2*margin-2*gap)/3;
    uint64_t payload = 0;
    for (const auto& chunk : app.analysis.cppl) payload += chunk.size - 4;
    const bool hasInfo = !app.info.empty() && app.analysis.fileSize != 0;
    std::wstring reduction = L"—";
    if (hasInfo)
    {
        std::wostringstream value;
        value << std::fixed << std::setprecision(1) << 100.0 * static_cast<double>(payload) / static_cast<double>(app.analysis.fileSize) << L" %";
        reduction = value.str();
    }
    const std::wstring values[] = { hasInfo ? sizeText(app.analysis.fileSize) : L"—", hasInfo ? sizeText(payload) : L"—", reduction };
    const wchar_t* labels[] = { L"ファイルサイズ", L"CPPlの内容", L"CPPlの割合" };
    for (int i = 0; i < 3; ++i)
    {
        const int x = margin + i*(metricWidth+gap);
        rounded(dc, rect(x,286,metricWidth,98), px(app,12), c.surface, c.border);
        drawText(dc, labels[i], rect(x+px(app,18),298,metricWidth-px(app,36),22), app.font, c.muted);
        drawText(dc, values[i], rect(x+px(app,18),326,metricWidth-px(app,36),38), app.numberFont, i==1 ? c.accent : c.text);
    }
    const RECT details{ margin, px(app,400), width-margin, client.bottom-px(app,104) };
    rounded(dc, details, px(app,14), c.surface, c.border);
    drawText(dc, L"02  /  CPPl情報", rect(margin+px(app,22),415,width-2*margin-px(app,44),24), app.labelFont, c.text);
    const RECT dot{ margin, client.bottom-px(app,67), margin+px(app,7), client.bottom-px(app,60) };
    rounded(dc, dot, px(app,3), app.busy ? c.accent : c.muted, app.busy ? c.accent : c.muted);
    drawText(dc, L"原本を保持して、別のAEPとして保存します", RECT{ margin,client.bottom-px(app,40),width-margin,client.bottom-px(app,20) }, app.font, c.muted);
}
LRESULT CALLBACK buttonProc(HWND button, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_MOUSEMOVE && !GetPropW(button, L"CpplHot"))
    {
        SetPropW(button, L"CpplHot", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, button, 0 }; TrackMouseEvent(&tracking);
        InvalidateRect(button, nullptr, FALSE);
    }
    else if (message == WM_MOUSELEAVE)
    {
        RemovePropW(button, L"CpplHot"); InvalidateRect(button, nullptr, FALSE);
    }
    else if (message == WM_NCDESTROY)
    {
        RemovePropW(button, L"CpplHot"); RemoveWindowSubclass(button, buttonProc, 1);
    }
    return DefSubclassProc(button, message, wParam, lParam);
}
void drawButton(App& app, const DRAWITEMSTRUCT& item)
{
    const auto& c = app.colors;
    const bool enabled = !(item.itemState & ODS_DISABLED);
    const bool primary = item.CtlID == SAVE_ID && enabled;
    const bool hot = enabled && GetPropW(item.hwndItem, L"CpplHot");
    const bool pressed = enabled && (item.itemState & ODS_SELECTED);
    const COLORREF fill = primary ? c.accent : pressed || hot ? c.hover : c.raised;
    FillRect(item.hDC, &item.rcItem, item.CtlID == SELECT_ID ? app.surfaceBrush : app.backgroundBrush);
    RECT rect = item.rcItem; InflateRect(&rect, -1, -1);
    rounded(item.hDC, rect, px(app,8), fill, primary ? c.accent : c.border);
    if (primary && (hot || pressed))
    {
        RECT inside = rect; InflateRect(&inside, -2, -2);
        rounded(item.hDC, inside, px(app,6), fill, c.accentText);
    }
    wchar_t text[128]{}; GetWindowTextW(item.hwndItem, text, 128);
    drawText(item.hDC, text, rect, app.labelFont, !enabled ? c.disabled : primary ? c.accentText : c.text, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT))
    {
        RECT focus = rect; InflateRect(&focus, -px(app,4), -px(app,4));
        DrawFocusRect(item.hDC, &focus);
    }
}
void layout(App& app)
{
    RECT rect{}; GetClientRect(app.window, &rect);
    const int margin=px(app,28), inset=px(app,22), width=rect.right, bottom=rect.bottom;
    MoveWindow(app.themeButton, width-margin-px(app,140), px(app,29), px(app,140), px(app,34), TRUE);
    MoveWindow(app.hint, margin+inset, px(app,161), width-2*margin-2*inset, px(app,24), TRUE);
    MoveWindow(app.pathBox, margin+inset, px(app,212), std::max(1,width-2*margin-2*inset-px(app,162)), px(app,27), TRUE);
    MoveWindow(app.selectButton, width-margin-inset-px(app,140), px(app,201), px(app,140), px(app,44), TRUE);
    MoveWindow(app.infoBox, margin+inset, px(app,450), width-2*margin-2*inset, std::max(1,bottom-px(app,574)), TRUE);
    MoveWindow(app.status, margin+px(app,17), bottom-px(app,76), std::max(1,width-2*margin-px(app,300)), px(app,26), TRUE);
    MoveWindow(app.cancelButton, width-margin-px(app,254), bottom-px(app,79), px(app,90), px(app,44), TRUE);
    MoveWindow(app.saveButton, width-margin-px(app,152), bottom-px(app,79), px(app,152), px(app,44), TRUE);
    InvalidateRect(app.window, nullptr, FALSE);
}
void updateFont(App& app)
{
    const auto make = [&](int points, int weight)
    {
        return CreateFontW(-MulDiv(points, static_cast<int>(app.dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
    };
    HFONT body=make(10,FW_NORMAL), title=make(24,FW_SEMIBOLD), number=make(22,FW_SEMIBOLD), label=make(10,FW_SEMIBOLD);
    if (!body || !title || !number || !label)
    {
        for (auto item : { body,title,number,label }) if(item) DeleteObject(item);
        return;
    }
    for (HWND child = GetWindow(app.window,GW_CHILD); child; child=GetWindow(child,GW_HWNDNEXT))
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(body),TRUE);
    for(auto item : { app.font,app.titleFont,app.numberFont,app.labelFont }) if(item) DeleteObject(item);
    app.font=body;app.titleFont=title;app.numberFont=number;app.labelFont=label;
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    try
    {
        switch (message)
        {
        case WM_CREATE:
        {
            app->dpi = GetDpiForWindow(window);
            updateIcon(*app);
            const auto control = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id = 0)
            {
                HWND child = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                    window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
                if (!child) winError("コントロールの作成失敗");
                return child;
            };
            app->themeButton = control(L"BUTTON", L"テーマ: 自動", WS_TABSTOP | BS_OWNERDRAW, THEME_ID);
            app->hint = control(L"STATIC", L"ここにAEPをドラッグ＆ドロップ  ·  1ファイルのみ", 0);
            app->pathBox = control(L"EDIT", L"ファイルが選択されていません", ES_READONLY | ES_AUTOHSCROLL, 1010);
            app->selectButton = control(L"BUTTON", L"ファイルを選択…", WS_TABSTOP | BS_OWNERDRAW, SELECT_ID);
            app->infoBox = control(L"EDIT", L"ファイルを1つ選択してください。複数ドロップ時は最初の1つだけを読み込みます。",
                WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 1011);
            app->saveButton = control(L"BUTTON", L"実行して保存…", WS_TABSTOP | BS_OWNERDRAW, SAVE_ID);
            app->cancelButton = control(L"BUTTON", L"中断", WS_TABSTOP | BS_OWNERDRAW, CANCEL_ID);
            app->status = control(L"STATIC", L"待機中", 0, 1012);
            for (auto button : { app->selectButton, app->saveButton, app->cancelButton, app->themeButton })
                SetWindowSubclass(button,buttonProc,1,0);
            SendMessageW(app->infoBox,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(0,0));
            updateFont(*app); applyTheme(*app); setBusy(*app, false, L"待機中"); layout(*app);
            return 0;
        }
        case WM_SIZE: layout(*app); return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = { px(*app, 760), px(*app, 680) }; return 0;
        case WM_DPICHANGED:
        {
            app->dpi = HIWORD(wParam); updateFont(*app); updateIcon(*app);
            const RECT& rect = *reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(*app); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PRINTCLIENT: paint(*app,reinterpret_cast<HDC>(wParam));return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT state{}; HDC dc=BeginPaint(window,&state);
            RECT rect{};GetClientRect(window,&rect);
            HDC memory=CreateCompatibleDC(dc); HBITMAP bitmap=CreateCompatibleBitmap(dc,std::max(1L,rect.right),std::max(1L,rect.bottom));
            if(memory && bitmap)
            {
                const auto old=SelectObject(memory,bitmap);paint(*app,memory);
                BitBlt(dc,0,0,rect.right,rect.bottom,memory,0,0,SRCCOPY);
                SelectObject(memory,old);
            }
            else paint(*app,dc);
            if(bitmap) DeleteObject(bitmap);if(memory) DeleteDC(memory);
            EndPaint(window,&state);return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        {
            const auto dc=reinterpret_cast<HDC>(wParam);
            const auto child=reinterpret_cast<HWND>(lParam);
            const bool surface=child==app->pathBox || child==app->infoBox || child==app->hint;
            SetTextColor(dc,child==app->hint || child==app->status ? app->colors.muted : app->colors.text);
            SetBkColor(dc,surface ? app->colors.surface : app->colors.background);
            return reinterpret_cast<LRESULT>(surface ? app->surfaceBrush : app->backgroundBrush);
        }
        case WM_DRAWITEM:
            drawButton(*app,*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));return TRUE;
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED: applyTheme(*app);return 0;
        case WM_COMMAND:
            if (LOWORD(wParam)==THEME_ID)
            {
                app->theme=(app->theme+1)%3;applyTheme(*app);return 0;
            }
            if (LOWORD(wParam) == CANCEL_ID && app->busy)
            {
                InterlockedExchange(&cancelled, 1); EnableWindow(app->cancelButton, FALSE);
                SetWindowTextW(app->status, L"中断しています…");
            }
            else if (!app->busy && LOWORD(wParam) == SELECT_ID)
            {
                fs::path path;
                if (chooseFile(*app, false, path)) openFile(*app, path);
            }
            else if (!app->busy && app->ready && LOWORD(wParam) == SAVE_ID)
            {
                fs::path path;
                if (chooseFile(*app, true, path)) saveFile(*app, path);
            }
            return 0;
        case WM_DROPFILES:
        {
            const HDROP drop = reinterpret_cast<HDROP>(wParam);
            const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, 0, path.data(), length + 1); DragFinish(drop);
            path.resize(length);
            if (!app->busy && length) openFile(*app, path);
            return 0;
        }
        case WORK_DONE:
        {
            std::unique_ptr<Result> result(reinterpret_cast<Result*>(lParam));
            if (app->worker.joinable()) app->worker.join();
            if (!result->saved)
            {
                app->analysis = result->analysis;
                app->ready = result->success && canPatch(app->analysis);
                app->info = result->text;
                SetWindowTextW(app->infoBox, app->info.c_str());
            }
            else
            {
                SetWindowTextW(app->infoBox, (app->info + L"\r\n\r\n" + result->text).c_str());
            }
            setBusy(*app, false, result->interrupted ? L"中断しました" : result->success ? (result->saved ? L"保存完了" : L"解析完了") : L"失敗しました");
            if (app->closing) DestroyWindow(window);
            return 0;
        }
        case WM_CLOSE:
            if (app->busy)
            {
                app->closing = true; InterlockedExchange(&cancelled, 1);
                EnableWindow(app->cancelButton, FALSE); SetWindowTextW(app->status, L"中断して終了しています…");
            }
            else DestroyWindow(window);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
    }
    catch (const std::exception& error)
    {
        MessageBoxW(window, wide(error.what()).c_str(), L"delCpplUI", MB_OK | MB_ICONERROR);
        if (message == WM_CREATE) return -1;
        if (app->busy && !app->worker.joinable()) setBusy(*app, false, L"開始できませんでした");
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    Gdiplus::GdiplusStartupInput graphicsInput;
    ULONG_PTR graphicsToken=0;
    if(Gdiplus::GdiplusStartup(&graphicsToken,&graphicsInput,nullptr)!=Gdiplus::Ok) return 1;
    struct GraphicsLifetime { ULONG_PTR token; ~GraphicsLifetime() { Gdiplus::GdiplusShutdown(token); } } graphics{graphicsToken};
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) return 1;
    int result = 0;
    {
        App app;
        INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES }; InitCommonControlsEx(&controls);
        WNDCLASSEXW cls{ sizeof(cls) };
        cls.lpfnWndProc = windowProc; cls.hInstance = instance; cls.lpszClassName = L"delCpplUIWindow";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
        cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        cls.hbrBackground = nullptr;
        if (!RegisterClassExW(&cls)) { CoUninitialize(); return 1; }
        HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, cls.lpszClassName, L"delCpplUI", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 920, 780, nullptr, nullptr, instance, &app);
        if (!window) { CoUninitialize(); return 1; }
        ShowWindow(window, show); UpdateWindow(window);
        int count = 0;
        PWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (args && count > 1)
        {
            try { openFile(app, args[1]); }
            catch (const std::exception& error) { MessageBoxW(window, wide(error.what()).c_str(), L"delCpplUI", MB_OK | MB_ICONERROR); }
        }
        if (args) LocalFree(args);
        MSG message{};
        BOOL received = 0;
        while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0)
        {
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        result = received == -1 ? 1 : static_cast<int>(message.wParam);
    }
    CoUninitialize();
    return result;
}
