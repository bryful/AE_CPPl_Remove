// アプリの描画を同一プロセス内でPNGに書き出す。画面は表示しない。
#define wWinMain unusedUiEntry
#include "../delCpplUI/delCpplUI.cpp"
#undef wWinMain
#include <chrono>
int wmain(int argc,wchar_t** argv)
{
    if(argc!=2) return 2;
    Gdiplus::GdiplusStartupInput input;ULONG_PTR token{};
    if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int result=0;
    {
        App app;
        WNDCLASSW cls{};cls.lpfnWndProc=windowProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"CpplPreview";
        RegisterClassW(&cls);
        HWND window=CreateWindowW(cls.lpszClassName,L"preview",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,0,0,920,780,nullptr,nullptr,cls.hInstance,&app);
        try
        {
            if(!window) throw std::runtime_error("create window");
            openFile(app,argv[1]);
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
            while(app.busy && std::chrono::steady_clock::now()<deadline)
            {
                MSG message{};
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message);DispatchMessageW(&message); }
                Sleep(1);
            }
            if(app.busy) throw std::runtime_error("timeout");
            UINT count{},bytes{};Gdiplus::GetImageEncodersSize(&count,&bytes);
            std::vector<unsigned char> encoders(bytes);
            auto* codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(encoders.data());
            Gdiplus::GetImageEncoders(count,bytes,codecs);CLSID png{};
            for(UINT i=0;i<count;++i) if(std::wstring(codecs[i].MimeType)==L"image/png") png=codecs[i].Clsid;
            for(int theme=1;theme<=2;++theme)
            {
                app.theme=theme;applyTheme(app);
                RECT rect{};GetClientRect(window,&rect);
                Gdiplus::Bitmap bitmap(rect.right,rect.bottom,PixelFormat32bppARGB);
                Gdiplus::Graphics graphics(&bitmap);HDC dc=graphics.GetHDC();
                paint(app,dc);
                for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
                {
                    RECT childRect{};GetWindowRect(child,&childRect);
                    MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&childRect),2);
                    const int saved=SaveDC(dc);
                    SetViewportOrgEx(dc,childRect.left,childRect.top,nullptr);
                    IntersectClipRect(dc,0,0,childRect.right-childRect.left,childRect.bottom-childRect.top);
                    SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_ERASEBKGND);
                    RestoreDC(dc,saved);
                }
                graphics.ReleaseHDC(dc);
                const auto path=theme==1 ? L"tests/ui_dark.png" : L"tests/ui_light.png";
                if(bitmap.Save(path,&png)!=Gdiplus::Ok) throw std::runtime_error("save PNG");
                Gdiplus::Color pixel;bitmap.GetPixel(5,5,&pixel);
                if(pixel.GetR()!=GetRValue(app.colors.background)) throw std::runtime_error("background pixel mismatch");
            }
            std::cout<<"dark and light render: PASS\n";
        }
        catch(const std::exception& error) { std::cerr<<error.what()<<'\n';result=1; }
        if(window) DestroyWindow(window);
    }
    Gdiplus::GdiplusShutdown(token);return result;
}
