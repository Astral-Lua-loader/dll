// Offline check of src/Menu.cpp: a WARP device, the real menu fed a sample schema and a scripted mouse. Writes
// preview_<n>.bmp to the working folder and checks clicks reach the values and the config round-trips. rc 0 = pass.
#include <d3d11.h>
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "Avatar.h"
#include "Menu.h"

namespace {

const char* kSchema =
    "page\tRage\n"
    "group\tTest\t0\n"
    "check\t3\tEnabled\t0\t1\n"
    "slider\t4\tField of view\t10\t1\t180\t1\t\xC2\xB0\n"
    "combo\t5\tTarget\t0\tClosest to crosshair|Lowest health|Distance\n"
    "check\t6\tSilent\t1\t0\n"
    "group\tAccuracy\t1\n"
    "slider\t7\tHit chance\t60\t0\t100\t1\t%\n"
    "slider\t8\tSmoothing\t2.5\t0\t10\t0.1\t\n"
    "check\t9\tAuto stop\t1\t1\n"
    "page\tVisuals\n"
    "group\tESP\t0\n"
    "check\t10\tBoxes\t1\t1\n"
    "check\t11\tNames\t1\t0\n"
    "check\t12\tHealth bar\t0\t0\n"
    "combo\t13\tBox style\t1\tFull|Corners|Rounded\n"
    "group\tWorld\t1\n"
    "slider\t14\tBrightness\t100\t0\t200\t5\t%\n"
    "page\tMisc\n"
    "group\tMovement\t0\n"
    "check\t15\tBunny hop\t1\t1\n"
    "check\t16\tAuto strafe\t0\t1\n"
    "page\tScripts\n"
    "group\tScripts\t0\n"
    "list\t20\tScripts\t380\tairduck^ecvd|Anime Menu^1tsuki|anti backstab^lordmouse|Chams^paradise|Spotify implementation^paradise|"
    "Whitelist^april|watermark^sumeng|Skeleton^majesty|Region changer^paradise|Quick retreat^Yuk9|Hostage ESP^jack|"
    "zeus range (BIG HUGE UPDATE)^abc|Floating killstreaks!^keer|Damage indicator^sumeng|Custom Hit Sound^silv|china hat^balls\n"
    "group\tScript\t1\n"
    "info\t90\tInfo\t20\n";

constexpr int kW = 1280, kH = 760;
ID3D11Device* dev = nullptr;
ID3D11DeviceContext* ctx = nullptr;
ID3D11Texture2D* target = nullptr;
ID3D11Texture2D* staging = nullptr;
ID3D11RenderTargetView* rtv = nullptr;
int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failed;
}

void Frame(float mx, float my, bool left, bool right = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(kW, kH), io.DeltaTime = 1.f / 60;
    io.AddMousePosEvent(mx, my);
    io.AddMouseButtonEvent(0, left);
    io.AddMouseButtonEvent(1, right);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    alm::menu::Frame();
    ImGui::Render();
    const float top[4] = {0.20f, 0.22f, 0.26f, 1};
    ctx->ClearRenderTargetView(rtv, top);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, kW, kH, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
void Frames(int n, float mx, float my, bool left = false) { for (int i = 0; i < n; ++i) Frame(mx, my, left); }
void Click(float x, float y, bool right = false) {
    Frame(x, y, false), Frame(x, y, !right, right), Frame(x, y, false), Frames(20, x, y);
}

void Shot(const char* name) {
    ctx->CopyResource(staging, target);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return;
    std::ofstream out(name, std::ios::binary);
    BITMAPFILEHEADER fh{0x4D42, static_cast<DWORD>(54 + kW * kH * 4), 0, 0, 54};
    BITMAPINFOHEADER ih{sizeof(ih), kW, -kH, 1, 32, BI_RGB};
    out.write(reinterpret_cast<char*>(&fh), sizeof(fh));
    out.write(reinterpret_cast<char*>(&ih), sizeof(ih));
    std::vector<unsigned char> row(kW * 4);
    for (int y = 0; y < kH; ++y) {
        const auto* src = static_cast<unsigned char*>(m.pData) + y * m.RowPitch;
        for (int x = 0; x < kW; ++x)
            row[x * 4] = src[x * 4 + 2], row[x * 4 + 1] = src[x * 4 + 1], row[x * 4 + 2] = src[x * 4], row[x * 4 + 3] = 255;
        out.write(reinterpret_cast<char*>(row.data()), row.size());
    }
    ctx->Unmap(staging, 0);
}

}  // namespace

int main() {
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (!dev) return std::puts("no WARP device"), 2;
    D3D11_TEXTURE2D_DESC td{kW, kH, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    dev->CreateTexture2D(&td, nullptr, &target);
    dev->CreateRenderTargetView(target, nullptr, &rtv);
    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, nullptr, &staging);

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring cfg = std::wstring(tmp) + L"alm_preview.cfg";
    DeleteFileW(cfg.c_str());
    std::ofstream(cfg) << "Visuals/ESP/Boxes\t1\t70\nMisc/Movement/Bunny hop\t1\t32\n";  // keys, to show the chips

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    alm::menu::Fonts(GetEnvironmentVariableA("ALM_REAL_SCALE", nullptr, 0) ? alm::menu::Scale() : 1.0f);  // ALM_REAL_SCALE: look at the real size (the click tests assume 1.0)
    ImGui_ImplDX11_Init(dev, ctx);
    alm::menu::SetConfigPath(cfg);
    alm::menu::SetSchema(kSchema);
    alm::menu::SetState(true, -1, -1, -1);
    alm::menu::SetUser("Bogdanuq");
    alm::menu::SetDevice(dev);
    Sleep(3000);  // the Steam avatar downloads in the background

    for (int f = 0; f < 8; ++f) { Frames(6, 1200, 740); char n[32]; std::snprintf(n, sizeof(n), "open_%d.bmp", f); Shot(n); }  // the header animating in, every 0.1 s
    Frames(40, 1200, 740);
    Check(alm::menu::Get(3) == 0 && alm::menu::Get(4) == 10 && alm::menu::Get(8) == 2.5, "defaults");
    float x = 0, y = 0;
    Check(alm::menu::RowAt(3, &x, &y), "rows are laid out");
    Click(x - 60, y);
    Check(alm::menu::Get(3) == 1, "click toggles a checkbox");
    Click(x, y, true);  // right-click: waiting for a key (chip shows "...")
    Frames(5, x, y);
    Shot("preview_bind.bmp");
    Click(x + 400, y + 300);  // a click elsewhere cancels
    alm::menu::RowAt(4, &x, &y);
    const float sx0 = x - 120, sx1 = x + 60;  // the track spans the row; drag from left to the middle
    Frame(sx0, y + 8, false), Frame(sx0, y + 8, true);
    Frames(3, sx1, y + 8, true);
    Frame(sx1, y + 8, false);
    Check(alm::menu::Get(4) > 10, "drag sets a slider");
    Frames(20, 1200, 740);
    Shot("preview_0.bmp");

    alm::menu::RowAt(5, &x, &y);
    Click(x, y + 10);  // open the Target dropdown (field under its label)
    Frames(10, x, y + 10);
    Shot("preview_1.bmp");
    Click(x, y + 10 + 2 * 33);  // an option below
    Check(alm::menu::Get(5) != 0, "dropdown picks an option");

    Check(alm::menu::TabAt(1, &x, &y), "tabs are laid out");
    Click(x, y);  // Visuals
    Frames(30, 1200, 740);
    Shot("preview_2.bmp");
    Check(alm::menu::RowAt(10, &x, &y) && alm::menu::Get(10) == 1, "visuals page shown, defaults");

    Check(alm::menu::TabAt(2, &x, &y), "misc tab");
    Click(x, y);  // a one-column page: the narrow window
    Frames(30, 1200, 740);
    Shot("preview_3.bmp");
    Check(alm::menu::TabAt(3, &x, &y), "scripts tab");
    Click(x, y);
    Frames(30, 1200, 740);
    float lx = 0, ly = 0;
    Check(alm::menu::RowBox(20, &lx, &ly), "list laid out");
    const float first = ly + 30 + 8 + 6 + 13;  // search line, gap, list padding, half a row
    Click(lx + 16, first + 26);  // the second item's circle: on
    Check(alm::menu::Get(22) == 1 && alm::menu::Get(20) == 1, "circle turns an item on and selects it");
    Click(lx + 120, first + 3 * 26);  // the fourth item's name: selected, still off
    Check(alm::menu::Get(20) == 3 && alm::menu::Get(24) == 0, "a name click selects only");
    alm::menu::SetValue(25, 2);  // the Lua says item 5 failed
    Frames(20, 1200, 740);
    Shot("preview_4.bmp");
    const double picked = alm::menu::Get(5);
    alm::menu::Save();
    alm::menu::SetSchema(kSchema);  // a reload: values come back from the config
    Check(alm::menu::Get(3) == 1 && alm::menu::Get(5) == picked, "config round-trip");

    alm::avatar::Stop();
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    std::printf("%s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}
