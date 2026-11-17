#include "Avatar.h"

#include <windows.h>
#include <urlmon.h>
#include <wincodec.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alm::avatar {
namespace {

std::thread g_thread;
std::mutex g_lock;
std::vector<unsigned char> g_pixels;  // RGBA, ready for the render thread
UINT g_w = 0, g_h = 0;
std::atomic<bool> g_ready{false}, g_started{false};
ID3D11ShaderResourceView* g_srv = nullptr;

std::string Download(const std::wstring& url) {
    IStream* s = nullptr;
    if (FAILED(URLOpenBlockingStreamW(nullptr, url.c_str(), &s, 0, nullptr)) || !s) return {};
    std::string out;
    char buf[8192];
    ULONG n = 0;
    while (SUCCEEDED(s->Read(buf, sizeof(buf), &n)) && n > 0 && out.size() < (8u << 20)) out.append(buf, n);
    s->Release();
    return out;
}

// the logged-in Steam account (Steam keeps its 32-bit id under ActiveProcess) as a SteamID64
unsigned long long SteamId() {
    DWORD id = 0, size = sizeof(id);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", RRF_RT_REG_DWORD, nullptr, &id, &size) != ERROR_SUCCESS)
        return 0;
    return id ? 76561197960265728ULL + id : 0;
}

bool Decode(const std::string& bytes) {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    bool ok = false;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(f->CreateStream(&st)) &&
        SUCCEEDED(st->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size()))) &&
        SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) && SUCCEEDED(dec->GetFrame(0, &frame)) &&
        SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (w && h && w <= 1024 && h <= 1024) {
            std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) {
                std::lock_guard lock(g_lock);
                g_pixels = std::move(px), g_w = w, g_h = h, ok = true;
            }
        }
    }
    for (IUnknown* u : {static_cast<IUnknown*>(conv), static_cast<IUnknown*>(frame), static_cast<IUnknown*>(dec),
                        static_cast<IUnknown*>(st), static_cast<IUnknown*>(f)})
        if (u) u->Release();
    return ok;
}

void Fetch() {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    struct Done { bool com; ~Done() { if (com) CoUninitialize(); } } done{com};
    const unsigned long long id = SteamId();
    if (!id) return;
    const std::string xml = Download(L"https://steamcommunity.com/profiles/" + std::to_wstring(id) + L"/?xml=1");
    const std::string open = "<avatarFull><![CDATA[";
    const size_t a = xml.find(open);
    if (a == std::string::npos) return;
    const size_t b = xml.find("]]>", a + open.size());
    if (b == std::string::npos) return;
    const std::string url = xml.substr(a + open.size(), b - a - open.size());
    if (url.rfind("https://", 0) != 0) return;
    const std::string img = Download(std::wstring(url.begin(), url.end()));
    if (img.empty()) return;
    if (Decode(img)) g_ready = true;
}

}  // namespace

void Start() {
    if (g_started.exchange(true)) return;
    g_thread = std::thread(Fetch);
}

ImTextureID Texture(ID3D11Device* device) {
    if (g_srv) return reinterpret_cast<ImTextureID>(g_srv);
    if (!g_ready || !device) return 0;
    std::lock_guard lock(g_lock);
    D3D11_TEXTURE2D_DESC td{g_w, g_h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA data{g_pixels.data(), g_w * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&td, &data, &tex)) && tex) {
        device->CreateShaderResourceView(tex, nullptr, &g_srv);
        tex->Release();
    }
    g_ready = false;  // made (or failed): don't try every frame
    g_pixels.clear();
    return reinterpret_cast<ImTextureID>(g_srv);
}

void Stop() {
    if (g_thread.joinable()) g_thread.join();
    if (g_srv) g_srv->Release(), g_srv = nullptr;
    g_started = false, g_ready = false;
}

}  // namespace alm::avatar
