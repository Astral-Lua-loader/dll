#include "Hooks.h"

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "Menu.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace alm::hooks {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_present = nullptr;
ResizeFn g_resize = nullptr;
void* g_targets[2] = {};
WNDPROC g_wndProc = nullptr;
HWND g_window = nullptr;
std::atomic<bool> g_shutdown{false};
std::atomic<int> g_inside{0};  // threads in our Present / WndProc
bool g_imgui = false, g_failed = false;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

struct Inside {
    Inside() { ++g_inside; }
    ~Inside() { --g_inside; }
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Inside in;
    if (g_shutdown || !g_imgui) return CallWindowProcW(g_wndProc, hwnd, msg, wp, lp);
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
    if (menu::WantsMouse()) {
        switch (msg) {  // over our window: the click is ours, not Astral's or the game's (releases still go through)
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                return 0;
        }
    }
    return CallWindowProcW(g_wndProc, hwnd, msg, wp, lp);
}

void MakeTarget(IDXGISwapChain* swap) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

bool InitImGui(IDXGISwapChain* swap) {
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swap->GetDesc(&desc)) || FAILED(swap->GetDevice(IID_PPV_ARGS(&g_device)))) return false;
    g_window = desc.OutputWindow;
    g_device->GetImmediateContext(&g_context);
    MakeTarget(swap);
    menu::Fonts(desc.BufferDesc.Height >= 1440 ? desc.BufferDesc.Height / 1080.0f : 1.0f);
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);
    g_wndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
    return true;
}

void Draw(IDXGISwapChain* swap) {
    if (!g_imgui) g_imgui = InitImGui(swap);
    if (!g_imgui) return;
    if (GetWindowLongPtrW(g_window, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(WndProc))  // someone took the window back
        g_wndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    menu::Frame();
    ImGui::Render();
    ID3D11RenderTargetView* saved[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* savedDepth = nullptr;
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved, &savedDepth);
    if (g_rtv) {
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved, savedDepth);
    for (auto* r : saved)
        if (r) r->Release();
    if (savedDepth) savedDepth->Release();
}

void SafeDraw(IDXGISwapChain* swap) {
    __try {
        Draw(swap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed = true;  // a fault: the menu goes, the game stays
    }
}

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
    Inside in;
    if (!g_shutdown && !g_failed) SafeDraw(swap);
    return g_present(swap, sync, flags);
}

HRESULT STDMETHODCALLTYPE HkResize(IDXGISwapChain* swap, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    Inside in;
    if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
    const HRESULT hr = g_resize(swap, count, w, h, fmt, flags);
    if (g_device && !g_shutdown) MakeTarget(swap);
    return hr;
}

bool Hook(int slot, void* target, void* detour, void** original) {
    if (!target || MH_CreateHook(target, detour, original) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) return MH_RemoveHook(target), false;
    g_targets[slot] = target;
    return true;
}

}  // namespace

bool Initialize() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr,
                   nullptr, L"AlmMenuProbe", nullptr};
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1, sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM, sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy, sd.SampleDesc.Count = 1, sd.Windowed = TRUE, sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx);
    void *present = nullptr, *resize = nullptr;
    if (swap) {
        void** vt = *reinterpret_cast<void***>(swap);
        present = vt[8], resize = vt[13];
        swap->Release(), dev->Release(), ctx->Release();
    }
    DestroyWindow(dummy);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
    return Hook(0, present, reinterpret_cast<void*>(&HkPresent), reinterpret_cast<void**>(&g_present)) &&
           Hook(1, resize, reinterpret_cast<void*>(&HkResize), reinterpret_cast<void**>(&g_resize));
}

void Shutdown() {
    g_shutdown = true;
    for (void* t : g_targets)
        if (t) MH_DisableHook(t), MH_RemoveHook(t);
    MH_Uninitialize();
    if (g_window && g_wndProc && GetWindowLongPtrW(g_window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(WndProc))
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_wndProc));
    for (int i = 0; i < 50 && g_inside > 0; ++i) Sleep(10);  // a frame or message still in our code
    Sleep(50);
    menu::Save();
    if (g_imgui) ImGui_ImplDX11_Shutdown(), ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_rtv = nullptr, g_context = nullptr, g_device = nullptr, g_imgui = false;
}

}  // namespace alm::hooks
