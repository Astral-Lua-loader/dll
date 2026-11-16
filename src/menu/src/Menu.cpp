#include "Menu.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

namespace alm::menu {
namespace {

// ---------------------------------------------------------------- model
enum Kind { kCheck, kSlider, kCombo };
struct Ctl {
    Kind kind = kCheck;
    int slot = 0;
    std::string name, id;  // id: "page/group/name", the config key
    double def = 0, min = 0, max = 1, step = 1;
    std::string suffix;
    std::vector<std::string> opts;
    bool bindable = false;
    int key = 0;  // vk, 0 none
};
struct Group { std::string name; int col = 0; std::vector<int> ctls; };
struct Page { std::string name; std::vector<Group> groups; };

constexpr int kSlots = 4096;
std::array<std::atomic<double>, kSlots> g_vals{};
std::mutex g_lock;  // the model below: SetSchema (Lua thread) vs Frame (render thread)
std::vector<Page> g_pages;
std::vector<Ctl> g_ctls;
std::wstring g_cfgPath;
std::map<std::string, std::pair<double, int>> g_cfg;  // id -> value, key (what menu.cfg holds)
float g_x = -1, g_y = -1;
int g_tab = 0;
bool g_dirty = false;
double g_dirtyAt = 0;

std::atomic<bool> g_open{false}, g_wantMouse{false};
std::atomic<int> g_accent{0x7C8CFF};

// ---------------------------------------------------------------- look
float S = 1;  // scale (1 at 1080p)
ImFont *g_regular = nullptr, *g_semi = nullptr, *g_brand = nullptr, *g_small = nullptr;
float g_alpha = 0;  // the open fade

constexpr float W = 620, HEAD = 48, FOOT = 30, PAD = 16, GAP = 12, TITLE = 34, BOXW = 150;
constexpr float GW = (W - 2 * PAD - GAP) / 2;
float RowH(Kind k) { return k == kSlider ? 42.f : k == kCombo ? 32.f : 28.f; }

ImU32 Col(unsigned rgb, float a = 1) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, static_cast<int>(255 * std::clamp(a * g_alpha, 0.f, 1.f)));
}
ImU32 Mix(unsigned a, unsigned b, float t, float alpha = 1) {
    auto ch = [&](int s) { return static_cast<unsigned>(((a >> s) & 255) + (static_cast<int>((b >> s) & 255) - static_cast<int>((a >> s) & 255)) * t); };
    return Col((ch(16) << 16) | (ch(8) << 8) | ch(0), alpha);
}
enum : unsigned {
    cBg = 0x0F1014, cLine = 0x1D2026, cEdge = 0x23262D, cGroup = 0x15171C, cGroupEdge = 0x20232A, cText = 0xD4D7DD,
    cHot = 0xF4F5F7, cDim = 0x8B909B, cFaint = 0x5C616C, cTitle = 0xA3A8B3, cTrack = 0x2A2D35, cBox = 0x1B1D23,
    cBoxHot = 0x23262D, cWhite = 0xFFFFFF,
};
unsigned Accent() { return static_cast<unsigned>(g_accent.load()); }

// eased per-widget values
std::unordered_map<ImGuiID, float> g_anim;
float Anim(ImGuiID id, float target, float speed = 14) {
    auto [it, fresh] = g_anim.try_emplace(id, target);
    float& v = it->second;
    if (!fresh) v += (target - v) * std::min(1.f, ImGui::GetIO().DeltaTime * speed);
    return v;
}

// ---------------------------------------------------------------- keys
int g_listen = -1;          // a control waiting for its key
bool g_listenArmed = false;  // the right click that started it is released
bool g_keyDown[256] = {};

const char* KeyName(int vk, char* buf) {
    static const std::map<int, const char*> names = {
        {4, "M3"}, {5, "M4"}, {6, "M5"}, {8, "BACK"}, {9, "TAB"}, {13, "ENTER"}, {16, "SHIFT"}, {17, "CTRL"}, {18, "ALT"},
        {20, "CAPS"}, {32, "SPACE"}, {33, "PGUP"}, {34, "PGDN"}, {35, "END"}, {36, "HOME"}, {37, "LEFT"}, {38, "UP"},
        {39, "RIGHT"}, {40, "DOWN"}, {45, "INS"}, {46, "DEL"}, {160, "LSHIFT"}, {161, "RSHIFT"}, {162, "LCTRL"},
        {163, "RCTRL"}, {164, "LALT"}, {165, "RALT"}, {192, "`"}};
    if (auto it = names.find(vk); it != names.end()) return it->second;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::snprintf(buf, 8, "%c", vk), buf;
    if (vk >= 0x60 && vk <= 0x69) return std::snprintf(buf, 8, "NUM%d", vk - 0x60), buf;
    if (vk >= 0x70 && vk <= 0x87) return std::snprintf(buf, 8, "F%d", vk - 0x6F), buf;
    return std::snprintf(buf, 8, "0x%02X", vk), buf;
}

bool GameFocused() {
    DWORD pid = 0;
    if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

void MarkDirty() { g_dirty = true, g_dirtyAt = ImGui::GetTime(); }

void Set(int slot, double v) {
    if (slot >= 0 && slot < kSlots && g_vals[slot].load() != v) g_vals[slot] = v, MarkDirty();
}
double Val(int slot) { return slot >= 0 && slot < kSlots ? g_vals[slot].load() : 0; }

void Keys() {  // every frame, menu open or not
    bool pressed[256] = {};
    const bool focus = GameFocused();
    for (int vk = 1; vk < 256; ++vk) {
        const bool down = focus && (GetAsyncKeyState(vk) & 0x8000) != 0;
        pressed[vk] = down && !g_keyDown[vk];
        g_keyDown[vk] = down;
    }
    if (g_listen >= 0 && g_listen < static_cast<int>(g_ctls.size())) {
        if (!g_listenArmed) {
            g_listenArmed = !g_keyDown[VK_RBUTTON] && !g_keyDown[VK_LBUTTON];
            return;
        }
        for (int vk = 3; vk < 256; ++vk) {
            if (!pressed[vk] || vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_INSERT) continue;
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;  // the L/R ones come too: keep those
            g_ctls[g_listen].key = vk == VK_ESCAPE || vk == VK_BACK ? 0 : vk;
            g_listen = -1;
            MarkDirty();
            return;
        }
        if (pressed[VK_LBUTTON]) g_listen = -1;  // a click elsewhere: cancelled
        return;
    }
    if (ImGui::GetIO().WantTextInput) return;
    for (Ctl& c : g_ctls)
        if (c.kind == kCheck && c.key > 0 && pressed[c.key]) Set(c.slot, Val(c.slot) != 0 ? 0 : 1);
}

// ---------------------------------------------------------------- config
std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) out.push_back(cur);
    if (!s.empty() && s.back() == sep) out.emplace_back();
    return out;
}

void LoadConfig() {
    g_cfg.clear();
    std::ifstream in(g_cfgPath);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto f = Split(line, '\t');
        if (f.size() < 2) continue;
        const double v = std::atof(f[1].c_str());
        if (f[0] == "@x") g_x = static_cast<float>(v);
        else if (f[0] == "@y") g_y = static_cast<float>(v);
        else if (f[0] == "@tab") g_tab = static_cast<int>(v);
        else g_cfg[f[0]] = {v, f.size() > 2 ? std::atoi(f[2].c_str()) : 0};
    }
}

void SaveLocked() {
    if (g_cfgPath.empty()) return;
    for (const Ctl& c : g_ctls) g_cfg[c.id] = {Val(c.slot), c.key};  // controls of other loads stay in the file
    const std::wstring part = g_cfgPath + L".part";
    {
        std::ofstream out(part, std::ios::trunc);
        out << "@x\t" << g_x << "\n@y\t" << g_y << "\n@tab\t" << g_tab << "\n";
        for (const auto& [id, vk] : g_cfg) out << id << '\t' << vk.first << '\t' << vk.second << '\n';
        if (!out) return;
    }
    if (MoveFileExW(part.c_str(), g_cfgPath.c_str(), MOVEFILE_REPLACE_EXISTING)) g_dirty = false;
}

// ---------------------------------------------------------------- widgets
struct Ctx { ImDrawList* dl; ImVec2 o; };  // the group's top left (screen)

void Text(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 c, const std::string& s) { dl->AddText(f, f->FontSize, p, c, s.c_str()); }
float TextW(ImFont* f, const std::string& s) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, s.c_str()).x; }

bool Hit(const char* id, ImVec2 p, ImVec2 size) {
    ImGui::SetCursorScreenPos(p);
    return ImGui::InvisibleButton(id, size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
}

int g_combo = -1;  // the control whose list is open
ImVec2 g_comboAt, g_comboRow0, g_comboRow1;  // the list's spot; the open combo's row

void RowCheck(Ctx& x, Ctl& c, int index, float y) {
    const ImVec2 p(x.o.x, x.o.y + y * S);
    ImGui::PushID(index);
    const bool clicked = Hit("row", p, ImVec2(GW * S, 28 * S));
    const bool hover = ImGui::IsItemHovered();
    const bool right = clicked && ImGui::GetIO().MouseReleased[ImGuiMouseButton_Right];
    ImGui::PopID();
    if (clicked && !right) Set(c.slot, Val(c.slot) != 0 ? 0 : 1);
    if (right && c.bindable) g_listen = g_listen == index ? -1 : index, g_listenArmed = false;

    const bool on = Val(c.slot) != 0;
    const ImGuiID id = ImGui::GetID(c.id.c_str());
    const float t = Anim(id, on ? 1.f : 0.f), h = Anim(id + 1, hover ? 1.f : 0.f);
    Text(x.dl, g_regular, ImVec2(p.x + 12 * S, p.y + 5 * S), Mix(cText, cHot, std::max(t, h)), c.name);
    const ImVec2 sw(p.x + (GW - 42) * S, p.y + 6 * S);
    x.dl->AddRectFilled(sw, ImVec2(sw.x + 30 * S, sw.y + 16 * S), Mix(cTrack, Accent(), t), 8 * S);
    x.dl->AddCircleFilled(ImVec2(sw.x + (8 + 14 * t) * S, sw.y + 8 * S), 6 * S, Mix(cDim, cWhite, t));

    if (!c.bindable) return;
    char buf[8];
    const bool listening = g_listen == index;
    const char* name = listening ? "..." : c.key > 0 ? KeyName(c.key, buf) : nullptr;
    if (!name) return;
    const float tw = TextW(g_small, name), cw = std::max(22 * S, tw + 12 * S);
    const ImVec2 a(sw.x - 8 * S - cw, p.y + 5 * S);
    x.dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + 18 * S), Col(cBox), 4 * S);
    Text(x.dl, g_small, ImVec2(a.x + (cw - tw) / 2, a.y + 2 * S), listening ? Col(Accent()) : Col(cDim), name);
}

std::string Fmt(const Ctl& c, double v) {
    int dec = 0;
    for (double s = c.step; s < 0.999 && dec < 3; s *= 10) ++dec;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f%s", dec, v, c.suffix.c_str());
    return buf;
}

void RowSlider(Ctx& x, Ctl& c, int index, float y) {
    const ImVec2 p(x.o.x, x.o.y + y * S);
    const float tx = p.x + 12 * S, tw = (GW - 24) * S;
    ImGui::PushID(index);
    Hit("row", p, ImVec2(GW * S, 42 * S));
    const bool hover = ImGui::IsItemHovered(), active = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    ImGui::PopID();
    if (active) {
        const double f = std::clamp((ImGui::GetIO().MousePos.x - tx) / tw, 0.f, 1.f);
        const double step = c.step > 0 ? c.step : 1;
        Set(c.slot, std::clamp(std::round((c.min + f * (c.max - c.min)) / step) * step, c.min, c.max));
    }
    const double v = Val(c.slot);
    const float f = c.max > c.min ? static_cast<float>(std::clamp((v - c.min) / (c.max - c.min), 0.0, 1.0)) : 0.f;
    const ImGuiID id = ImGui::GetID(c.id.c_str());
    const float h = Anim(id + 1, hover || active ? 1.f : 0.f), fa = active ? f : Anim(id, f, 18);
    Text(x.dl, g_regular, ImVec2(tx, p.y + 4 * S), Mix(cText, cHot, h), c.name);
    const std::string vs = Fmt(c, v);
    Text(x.dl, g_regular, ImVec2(p.x + (GW - 12) * S - TextW(g_regular, vs), p.y + 4 * S), Mix(cDim, cHot, active ? 1.f : 0.f), vs);
    const float ty = p.y + 29 * S;
    x.dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + 4 * S), Col(cTrack), 2 * S);
    if (fa > 0.001f) x.dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw * fa, ty + 4 * S), Col(Accent()), 2 * S);
    x.dl->AddCircleFilled(ImVec2(tx + tw * fa, ty + 2 * S), (5 + 1.5f * h) * S, Col(cWhite));
}

void RowCombo(Ctx& x, Ctl& c, int index, float y) {
    const ImVec2 p(x.o.x, x.o.y + y * S);
    const ImVec2 b(p.x + (GW - 12 - BOXW) * S, p.y + 4 * S);
    ImGui::PushID(index);
    const bool clicked = Hit("row", p, ImVec2(GW * S, 32 * S));
    const bool hover = ImGui::IsItemHovered();
    const bool right = clicked && ImGui::GetIO().MouseReleased[ImGuiMouseButton_Right];
    ImGui::PopID();
    if (clicked && !right) g_combo = g_combo == index ? -1 : index, g_comboAt = ImVec2(b.x, b.y + 28 * S);
    if (g_combo == index) g_comboRow0 = p, g_comboRow1 = ImVec2(p.x + GW * S, p.y + 32 * S);
    const ImGuiID id = ImGui::GetID(c.id.c_str());
    const float h = Anim(id + 1, hover || g_combo == index ? 1.f : 0.f);
    Text(x.dl, g_regular, ImVec2(p.x + 12 * S, p.y + 7 * S), Mix(cText, cHot, h), c.name);
    x.dl->AddRectFilled(b, ImVec2(b.x + BOXW * S, b.y + 24 * S), Mix(cBox, cBoxHot, h), 5 * S);
    x.dl->AddRect(b, ImVec2(b.x + BOXW * S, b.y + 24 * S), Col(cEdge), 5 * S);
    const int i = static_cast<int>(Val(c.slot));
    const std::string cur = i >= 0 && i < static_cast<int>(c.opts.size()) ? c.opts[i] : "";
    x.dl->PushClipRect(b, ImVec2(b.x + (BOXW - 24) * S, b.y + 24 * S), true);
    Text(x.dl, g_regular, ImVec2(b.x + 10 * S, b.y + 4 * S), Col(cText), cur);
    x.dl->PopClipRect();
    const ImVec2 ch(b.x + (BOXW - 15) * S, b.y + 10 * S);  // chevron
    const float open = Anim(id + 2, g_combo == index ? 1.f : 0.f, 18), d = 3.5f * S * (1 - 2 * open);
    x.dl->AddLine(ImVec2(ch.x - 3.5f * S, ch.y + 2 * S - d * 0.3f), ImVec2(ch.x, ch.y + 2 * S + d * 0.7f), Col(cDim), 1.5f * S);
    x.dl->AddLine(ImVec2(ch.x, ch.y + 2 * S + d * 0.7f), ImVec2(ch.x + 3.5f * S, ch.y + 2 * S - d * 0.3f), Col(cDim), 1.5f * S);
}

void ComboList() {
    if (g_combo < 0 || g_combo >= static_cast<int>(g_ctls.size())) return void(g_combo = -1);
    Ctl& c = g_ctls[g_combo];
    const float ih = 26 * S, w = BOXW * S, h = c.opts.size() * ih + 6 * S;
    const float t = Anim(0xC0B0, 1.f, 20);
    ImGui::SetNextWindowPos(ImVec2(g_comboAt.x, g_comboAt.y - (1 - t) * 4 * S));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##almlist", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float a = t;
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), Col(cBox, a), 5 * S);
    dl->AddRect(o, ImVec2(o.x + w, o.y + h), Col(cEdge, a), 5 * S);
    const int sel = static_cast<int>(Val(c.slot));
    for (int i = 0; i < static_cast<int>(c.opts.size()); ++i) {
        const ImVec2 p(o.x + 3 * S, o.y + 3 * S + i * ih);
        ImGui::PushID(i);
        const bool clicked = Hit("opt", p, ImVec2(w - 6 * S, ih));
        const bool hover = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hover) dl->AddRectFilled(p, ImVec2(p.x + w - 6 * S, p.y + ih), Col(cBoxHot, a), 4 * S);
        Text(dl, g_regular, ImVec2(p.x + 8 * S, p.y + 5 * S), i == sel ? Col(Accent(), a) : Col(hover ? cHot : cText, a), c.opts[i]);
        if (clicked) Set(c.slot, i), g_combo = -1;
    }
    const bool overList = ImGui::IsMouseHoveringRect(o, ImVec2(o.x + w, o.y + h), false);
    ImGui::End();
    if (g_combo >= 0 && !overList && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)) &&
        !ImGui::IsMouseHoveringRect(g_comboRow0, g_comboRow1, false))
        g_combo = -1;
    if (g_combo < 0) g_anim.erase(0xC0B0);
}

float PageHeight(const Page& page) {
    float cols[2] = {0, 0};
    for (const Group& g : page.groups) {
        float h = TITLE + 8;
        for (int i : g.ctls) h += RowH(g_ctls[i].kind);
        cols[g.col ? 1 : 0] += h + GAP;
    }
    return std::max({cols[0], cols[1], 120.f + GAP}) - GAP;
}

void Window() {
    ImGuiIO& io = ImGui::GetIO();
    const int pages = static_cast<int>(g_pages.size());
    g_tab = std::clamp(g_tab, 0, std::max(0, pages - 1));
    const float targetH = HEAD + PAD + (pages ? PageHeight(g_pages[g_tab]) : 120.f) + PAD + FOOT;
    const float H = Anim(0xA11, targetH, 16);
    if (g_x < 0 || g_y < 0) g_x = 80, g_y = std::max(0.f, (io.DisplaySize.y / S - targetH) / 2);
    g_x = std::clamp(g_x, 0.f, std::max(0.f, io.DisplaySize.x / S - W));
    g_y = std::clamp(g_y, 0.f, std::max(0.f, io.DisplaySize.y / S - HEAD));
    const ImVec2 o(std::round(g_x * S), std::round(g_y * S)), size(W * S, H * S);

    ImGui::SetNextWindowPos(o);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##alm", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 e(o.x + size.x, o.y + size.y);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    for (int i = 1; i <= 4; ++i)  // a soft shadow
        bg->AddRectFilled(ImVec2(o.x - i * 3 * S, o.y - i * 2 * S), ImVec2(e.x + i * 3 * S, e.y + i * 5 * S),
                          IM_COL32(0, 0, 0, static_cast<int>(28 * g_alpha / i)), (8 + i * 3) * S);
    dl->AddRectFilled(o, e, Col(cBg), 8 * S);
    dl->AddRect(o, e, Col(cEdge), 8 * S);

    // top bar: brand, tabs (drag the bar to move)
    Text(dl, g_brand, ImVec2(o.x + 18 * S, o.y + 13 * S), Col(cHot), "astral");
    dl->AddLine(ImVec2(o.x, o.y + HEAD * S), ImVec2(e.x, o.y + HEAD * S), Col(cLine));
    ImGui::SetCursorScreenPos(o);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##drag", ImVec2(size.x, HEAD * S));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
        g_x += io.MouseDelta.x / S, g_y += io.MouseDelta.y / S;
        MarkDirty();
    }
    float tx = o.x + 104 * S, barX = 0, barW = 0;
    for (int i = 0; i < pages; ++i) {
        const float w = TextW(g_semi, g_pages[i].name) + 28 * S;
        ImGui::PushID(1000 + i);
        if (Hit("tab", ImVec2(tx, o.y), ImVec2(w, HEAD * S)) && g_tab != i) g_tab = i, g_combo = -1, MarkDirty(), g_anim[0xFADE] = 0.f;
        const bool hover = ImGui::IsItemHovered();
        ImGui::PopID();
        const float h = Anim(ImGui::GetID(g_pages[i].name.c_str()), g_tab == i ? 1.f : hover ? 0.5f : 0.f);
        Text(dl, g_semi, ImVec2(tx + 14 * S, o.y + 15 * S), h > 0.5f ? Mix(cText, cHot, (h - 0.5f) * 2) : Mix(cDim, cText, h * 2), g_pages[i].name);
        if (g_tab == i) barX = tx + 14 * S, barW = w - 28 * S;
        tx += w;
    }
    if (pages) {
        const float bx = Anim(0xBA1, barX, 18), bw = Anim(0xBA2, barW, 18);
        dl->AddRectFilled(ImVec2(bx, o.y + (HEAD - 2) * S), ImVec2(bx + bw, o.y + HEAD * S), Col(Accent()), 1 * S);
    }

    // page: two columns of groups, fading in on a switch
    if (pages) {
        const float fade = Anim(0xFADE, 1.f, 12);
        const float saved = g_alpha;
        g_alpha *= fade;
        dl->PushClipRect(ImVec2(o.x, o.y + (HEAD + 1) * S), ImVec2(e.x, e.y - FOOT * S), true);
        float ys[2] = {PAD, PAD};
        int index = 0;
        for (Group& g : g_pages[g_tab].groups) {
            const int col = g.col ? 1 : 0;
            float h = TITLE + 8;
            for (int i : g.ctls) h += RowH(g_ctls[i].kind);
            Ctx x{dl, ImVec2(o.x + (PAD + col * (GW + GAP)) * S, o.y + (HEAD + ys[col] + (1 - fade) * 6) * S)};
            ys[col] += h + GAP;
            dl->AddRectFilled(x.o, ImVec2(x.o.x + GW * S, x.o.y + h * S), Col(cGroup), 6 * S);
            dl->AddRect(x.o, ImVec2(x.o.x + GW * S, x.o.y + h * S), Col(cGroupEdge), 6 * S);
            Text(dl, g_semi, ImVec2(x.o.x + 12 * S, x.o.y + 10 * S), Col(cTitle), g.name);
            float y = TITLE;
            for (int i : g.ctls) {
                Ctl& c = g_ctls[i];
                if (c.kind == kCheck) RowCheck(x, c, i, y);
                else if (c.kind == kSlider) RowSlider(x, c, i, y);
                else RowCombo(x, c, i, y);
                y += RowH(c.kind);
                ++index;
            }
        }
        dl->PopClipRect();
        g_alpha = saved;
    } else {
        Text(dl, g_regular, ImVec2(o.x + 18 * S, o.y + (HEAD + PAD) * S), Col(cDim), "Waiting for the Lua...");
    }

    // footer
    dl->AddLine(ImVec2(o.x, e.y - FOOT * S), ImVec2(e.x, e.y - FOOT * S), Col(cLine));
    Text(dl, g_small, ImVec2(o.x + 18 * S, e.y - 22 * S), Col(cFaint),
         g_listen >= 0 ? "Press a key to bind  \xC2\xB7  Esc clears" : "Right-click a toggle to bind a key  \xC2\xB7  drag the top bar to move");
    ImGui::End();
    ComboList();
}

std::string g_pending;  // a schema from the Lua, applied by SetSchema itself (under g_lock)

}  // namespace

void SetConfigPath(const std::wstring& path) {
    std::lock_guard lock(g_lock);
    g_cfgPath = path;
    LoadConfig();
}

void SetSchema(const std::string& text) {
    std::lock_guard lock(g_lock);
    if (!g_ctls.empty()) SaveLocked();  // the last load's values into g_cfg first
    g_pages.clear(), g_ctls.clear(), g_combo = -1, g_listen = -1;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto f = Split(line, '\t');
        if (f.empty()) continue;
        if (f[0] == "page" && f.size() >= 2) {
            g_pages.push_back({f[1], {}});
            continue;
        }
        if (g_pages.empty()) continue;
        Page& page = g_pages.back();
        if (f[0] == "group" && f.size() >= 3) {
            page.groups.push_back({f[1], std::atoi(f[2].c_str()), {}});
            continue;
        }
        if (page.groups.empty() || f.size() < 4) continue;
        Group& group = page.groups.back();
        Ctl c;
        c.slot = std::atoi(f[1].c_str()), c.name = f[2], c.def = std::atof(f[3].c_str());
        c.id = page.name + "/" + group.name + "/" + c.name;
        if (c.slot < 0 || c.slot >= kSlots) continue;
        if (f[0] == "check") c.kind = kCheck, c.bindable = f.size() > 4 && f[4] == "1";
        else if (f[0] == "slider" && f.size() >= 7)
            c.kind = kSlider, c.min = std::atof(f[4].c_str()), c.max = std::atof(f[5].c_str()), c.step = std::atof(f[6].c_str()),
            c.suffix = f.size() > 7 ? f[7] : "";
        else if (f[0] == "combo" && f.size() >= 5) c.kind = kCombo, c.opts = Split(f[4], '|');
        else continue;
        double v = c.def;
        if (auto it = g_cfg.find(c.id); it != g_cfg.end()) v = it->second.first, c.key = c.bindable ? it->second.second : 0;
        else g_dirty = true, g_dirtyAt = 0;  // a new control: into menu.cfg with its default
        if (c.kind == kSlider) v = std::clamp(v, c.min, c.max);
        if (c.kind == kCombo) v = std::clamp(std::round(v), 0.0, std::max(0.0, c.opts.size() - 1.0));
        g_vals[c.slot] = v;
        group.ctls.push_back(static_cast<int>(g_ctls.size()));
        g_ctls.push_back(std::move(c));
    }
}

double Get(int slot) { return Val(slot); }

void SetState(bool open, int r, int g, int b) {
    g_open = open;
    if (r >= 0) g_accent = (std::clamp(r, 0, 255) << 16) | (std::clamp(g, 0, 255) << 8) | std::clamp(b, 0, 255);
}

void Fonts(float scale) {
    S = scale;
    ImGuiIO& io = ImGui::GetIO();
    char win[MAX_PATH] = "C:\\Windows";
    GetWindowsDirectoryA(win, MAX_PATH);
    const std::string dir = std::string(win) + "\\Fonts\\";
    auto load = [&](const char* file, float px) -> ImFont* {
        const std::string path = dir + file;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 2, cfg.PixelSnapH = false;
        return io.Fonts->AddFontFromFileTTF(path.c_str(), px * S, &cfg);
    };
    g_regular = load("segoeui.ttf", 15);
    g_semi = load("seguisb.ttf", 15);
    g_brand = load("seguisb.ttf", 18);
    g_small = load("segoeui.ttf", 12.5f);
    ImFont* fallback = g_regular ? g_regular : io.Fonts->AddFontDefault();
    for (ImFont** f : {&g_regular, &g_semi, &g_brand, &g_small})
        if (!*f) *f = fallback;
    io.FontDefault = g_regular;
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(0, 0), st.WindowBorderSize = 0, st.ItemSpacing = ImVec2(0, 0);
}

void Frame() {
    std::lock_guard lock(g_lock);
    Keys();
    g_alpha += ((g_open ? 1.f : 0.f) - g_alpha) * std::min(1.f, ImGui::GetIO().DeltaTime * 16);
    if (g_open && g_alpha > 0.995f) g_alpha = 1;
    if (!g_open && g_alpha < 0.01f) {
        g_alpha = 0, g_combo = -1;
        if (g_listen >= 0) g_listen = -1;
    } else {
        Window();
    }
    g_wantMouse = g_alpha > 0.5f && ImGui::GetIO().WantCaptureMouse;
    if (g_dirty && ImGui::GetTime() - g_dirtyAt > 0.5) SaveLocked();
}

bool WantsMouse() { return g_wantMouse; }

void Save() {
    std::lock_guard lock(g_lock);
    if (g_dirty) SaveLocked();
}

}  // namespace alm::menu
