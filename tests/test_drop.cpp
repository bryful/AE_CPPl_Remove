// 実際のWM_DROPFILES処理を同一プロセス内で検証する。
#define wWinMain unusedUiEntry
#include "../delCpplUI/delCpplUI.cpp"
#undef wWinMain
#include <shlobj.h>
#include <chrono>

void dropFiles(HWND window, const std::vector<std::wstring>& files)
{
    std::wstring names;
    for (const auto& path : files) { names += path; names.push_back(L'\0'); }
    names.push_back(L'\0');
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + names.size() * sizeof(wchar_t));
    if (!memory) throw std::runtime_error("GlobalAlloc");
    auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
    drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    SendMessageW(window, WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0);
}
void pump(App& app)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (app.busy && std::chrono::steady_clock::now() < deadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(1);
    }
    if (app.busy) throw std::runtime_error("worker timeout");
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    Gdiplus::GdiplusStartupInput graphicsInput;
    ULONG_PTR graphicsToken = 0;
    if (Gdiplus::GdiplusStartup(&graphicsToken, &graphicsInput, nullptr) != Gdiplus::Ok) return 1;
    struct GraphicsLifetime
    {
        ULONG_PTR token;
        ~GraphicsLifetime() { Gdiplus::GdiplusShutdown(token); }
    } graphics{ graphicsToken };
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    App app;
    WNDCLASSW cls{};
    cls.lpfnWndProc = windowProc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"delCpplDropTest";
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowW(cls.lpszClassName, L"test", WS_OVERLAPPEDWINDOW, 0, 0, 800, 540, nullptr, nullptr, cls.hInstance, &app);
    if (!window) return 1;
    int result = 0;
    try
    {
        const auto file = fs::absolute(argv[1]).lexically_normal();
        dropFiles(window, { file.wstring(), L"does-not-exist.aep" }); pump(app);
        if (app.input != file || !app.ready) throw std::runtime_error("first file not selected");
        dropFiles(window, { file.parent_path().wstring(), file.wstring() }); pump(app);
        if (app.ready || app.input != file.parent_path()) throw std::runtime_error("directory accepted or second file selected");
        std::cout << "WM_DROPFILES: first file only; directory rejected: PASS\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    DestroyWindow(window);
    return result;
}
