// menu.dll for the loader's ui/menu.lua (Astral). The Lua declares the controls (alm_schema) and reads them
// (alm_get); this DLL draws the menu with ImGui over the game, keeps the values, keybinds and menu.cfg.
//   alm_init()     hooks in (the Lua calls it once after LoadLibrary)
//   alm_state()    each render: Astral's menu open + accent (ours shows with Astral's)
//   alm_shutdown() the Lua's shutdown: hooks off, config saved; then the Lua frees the DLL
#include <windows.h>

#include <string>

#include "Hooks.h"
#include "Menu.h"

namespace {
HMODULE g_module = nullptr;
bool g_up = false;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hModule), g_module = hModule;
    return TRUE;
}

extern "C" {

__declspec(dllexport) int alm_version() { return 1; }

__declspec(dllexport) int alm_init() {
    if (g_up) return 1;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring dir(path);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    alm::menu::SetConfigPath(dir + L"menu.cfg");  // beside the DLL: astral\lua\astral_dlls\menu.cfg
    g_up = alm::hooks::Initialize();
    return g_up ? 1 : 0;
}

__declspec(dllexport) void alm_schema(const char* text) { if (text) alm::menu::SetSchema(text); }
__declspec(dllexport) double alm_get(int slot) { return alm::menu::Get(slot); }
__declspec(dllexport) void alm_user(const char* name) { if (name) alm::menu::SetUser(name); }
__declspec(dllexport) void alm_set(int slot, double v) { alm::menu::SetValue(slot, v); }
__declspec(dllexport) void alm_state(int open, int r, int g, int b) { alm::menu::SetState(open != 0, r, g, b); }

__declspec(dllexport) void alm_shutdown() {
    if (g_up) alm::hooks::Shutdown();
    g_up = false;
}

}  // extern "C"
