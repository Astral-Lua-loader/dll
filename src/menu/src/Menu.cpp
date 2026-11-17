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

#include "astral/config.h"
#include "astral/ui.h"
#include "Avatar.h"

namespace alm::menu {
namespace {

// ---------------------------------------------------------------- model
enum Kind { kCheck, kSlider, kCombo, kList, kInfo };
struct Ctl {
    Kind kind = kCheck;
    int slot = 0;
    std::string name, id;  // id: "page/group/name", the config key
    double def = 0, min = 0, max = 1, step = 1;
    std::string suffix;
    std::vector<std::string> opts;
    std::vector<std::pair<std::string, std::string>> items;  // list: title, author
    char filter[64] = {};
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
    for (const Ctl& c : g_ctls) {  // controls of other loads stay in the file
        if (c.kind == kInfo) continue;
        g_cfg[c.id] = {Val(c.slot), c.key};
        for (size_t i = 0; i < c.items.size(); ++i) g_cfg[c.id + "/" + c.items[i].first] = {Val(c.slot + 1 + static_cast<int>(i)) == 1 ? 1 : 0, 0};
    }
    const std::wstring part = g_cfgPath + L".part";
    {
        std::ofstream out(part, std::ios::trunc);
        out << "@x\t" << g_x << "\n@y\t" << g_y << "\n@tab\t" << g_tab << "\n";
        for (const auto& [id, vk] : g_cfg) out << id << '\t' << vk.first << '\t' << vk.second << '\n';
        if (!out) return;
    }
    if (MoveFileExW(part.c_str(), g_cfgPath.c_str(), MOVEFILE_REPLACE_EXISTING)) g_dirty = false;
}

// ---------------------------------------------------------------- look: astralmenu's widgets (astral/ui.*)
float S = 1;          // scale (1 at 1080p)
float g_openT = 0;
float g_openAt = -10;  // when the menu last opened (header animation)    // Astral: opens in 4 frames, closes at once
float g_pageT = 1;    // tab switch: fade in + 7 px rise
float g_want = 300;   // the page's measured height (1080p px)
bool g_drag = false;
std::string g_user;                 // Astral's username (from the Lua)
ID3D11Device* g_device = nullptr;   // for the avatar texture
int g_cols = 2;
float g_barX = -1, g_barW = 0;  // the active tab's underline
std::unordered_map<int, ImRect> g_rowRects;  // slot -> its row on screen (the preview clicks by these)
std::vector<ImRect> g_tabRects;

constexpr float HEAD = 35, FOOT = 35, PADX = 15, PADY = 14;

void Star(ImDrawList* dl, ImVec2 c, float r, ImU32 color, float rot) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float a = rot + i * IM_PI / 5.f - IM_PI / 2.f, rr = (i & 1) ? r * 0.52f : r;
        pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
    }
    dl->AddConcavePolyFilled(pts, 10, color);
}

// Astral's header streaks (from a recording of it): thin light trails running along curves like comets, the tail
// fading out behind the head, one with a small sparkle at its head. Each has its own path, speed and start, so they
// cross the header at different times; on open they all start at once and spread out.
struct Streak { ImVec2 a, b, c, d; float period, offset, tail, alpha; bool sparkle; };
const Streak kStreaks[] = {
    {{0.04f, -0.15f}, {0.09f, 0.35f}, {0.15f, 0.75f}, {0.24f, 1.15f}, 3.2f, 0.f, 0.7f, 0.50f, false},  // the dive on the left
    {{0.10f, -0.15f}, {0.15f, 0.30f}, {0.21f, 0.80f}, {0.30f, 1.15f}, 3.6f, 0.f, 0.6f, 0.32f, false},  // its twin
    {{0.58f, 1.15f}, {0.66f, 1.00f}, {0.70f, 0.40f}, {0.79f, 0.12f}, 4.1f, 0.f, 0.8f, 0.48f, true},    // rising by the icons, sparkle
    {{0.30f, 1.10f}, {0.40f, 0.55f}, {0.48f, 0.95f}, {0.58f, 0.35f}, 5.3f, 0.f, 0.7f, 0.28f, false},   // an S in the middle
};

ImVec2 Bez(const Streak& s, float u, ImVec2 p, float w, float h) {
    const float v = 1 - u;
    const float x = v * v * v * s.a.x + 3 * v * v * u * s.b.x + 3 * v * u * u * s.c.x + u * u * u * s.d.x;
    const float y = v * v * v * s.a.y + 3 * v * v * u * s.b.y + 3 * v * u * u * s.c.y + u * u * u * s.d.y;
    return ImVec2(p.x + x * w, p.y + y * h);
}

void Sparkle(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {  // a four-point glint
    const ImVec2 pts[8] = {{c.x, c.y - r}, {c.x + r * 0.22f, c.y - r * 0.22f}, {c.x + r, c.y}, {c.x + r * 0.22f, c.y + r * 0.22f},
                           {c.x, c.y + r}, {c.x - r * 0.22f, c.y + r * 0.22f}, {c.x - r, c.y}, {c.x - r * 0.22f, c.y - r * 0.22f}};
    dl->AddConcavePolyFilled(pts, 8, col);
}

void Swirl(ImDrawList* dl, ImVec2 p, float w, float h, float since, float t) {
    (void)t;
    constexpr int N = 40;
    for (const Streak& s : kStreaks) {
        // the head runs from the path's start to past its end (so the tail leaves too), then waits and goes again
        const float cycle = s.period, run = cycle * 0.75f;
        const float local = std::fmod(std::max(0.f, since), cycle);  // on open all start together; their periods differ, so they drift apart
        if (local > run) continue;
        const float head = local / run * (1.f + s.tail);
        const float t0 = std::max(0.f, head - s.tail), t1 = std::min(1.f, head);
        if (t1 <= t0) continue;
        ImVec2 prev = Bez(s, t0, p, w, h);
        for (int i = 1; i <= N; ++i) {
            const float u = t0 + (t1 - t0) * i / N;
            const ImVec2 cur = Bez(s, u, p, w, h);
            const float k = (u - (head - s.tail)) / s.tail;  // 0 at the tail end, 1 at the head
            dl->AddLine(prev, cur, ui::col(ui::rgb(255, 255, 255), s.alpha * (0.2f + 0.8f * k)), ui::S(1.6f + 1.2f * k));
            prev = cur;
        }
        if (s.sparkle && head <= 1.f) Sparkle(dl, prev, ui::S(4.5f), ui::col(ui::rgb(255, 255, 255), 0.9f));
    }
}

void Header(ImDrawList* dl, ImVec2 p, float w, float starsEnd) {  // stars stop before the tabs
    const float h = ui::S(HEAD);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32_WHITE, ui::S(10), ImDrawFlags_RoundCornersTop);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p, ImVec2(p.x + w, p.y), ui::col(ui::accent()), ui::col(ui::accent2()));
    static const float stars[][3] = {{85, 14, 5}, {111, 10, 5}, {139, 19, 7.5f}, {170, 13, 5.5f}, {214, 9, 5}, {237, 16, 7.5f},
                                     {272, 10, 5}, {283, 23, 5}, {313, 19, 7.5f}, {334, 9, 5}, {348, 24, 5}, {367, 12, 7.5f},
                                     {397, 18, 5.5f}, {416, 9, 5}, {431, 23, 5}};
    const float t = static_cast<float>(ImGui::GetTime()), since = t - g_openAt;
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    Swirl(dl, p, w, h, since, t);
    dl->PopClipRect();
    for (int i = 0; i < 15; ++i) {
        const float r = ui::S(stars[i][2] * 1.2f);
        if (p.x + ui::S(stars[i][0]) + r >= starsEnd) continue;
        const ImVec2 c(p.x + ui::S(stars[i][0]), p.y + ui::S(stars[i][1]));
        // twinkle: each star on its own slow beat; at the top of it the star brightens and swells for a moment (no halo rings)
        const float beat = 0.5f + 0.5f * std::sin(t * (0.9f + 0.23f * (i % 5)) + i * 2.399f);
        const float peak = std::pow(beat, 6.f);
        const float pop = ui::ease(std::clamp((since - 0.04f * i) / 0.25f, 0.f, 1.f));  // on open the stars pop in left to right
        Star(dl, c, r * (0.6f + 0.4f * pop) * (1.f + 0.18f * peak), ui::col(ui::rgb(255, 255, 255), (0.7f + 0.3f * std::max(beat * 0.6f, peak)) * pop), i * 0.37f);
    }
    dl->AddText(ui::fHeader, ui::fHeader->FontSize, ImVec2(p.x + ui::S(12), p.y + (h - ui::fHeader->FontSize) * 0.5f), ui::col(ui::rgb(255, 255, 255)), "Astral");
}

// a page's tab: Astral's icon for the pages it has one for (Font Awesome, embedded in astral/fonts.h), else its name
std::pair<ImFont*, const char*> TabLabel(const std::string& name) {
    if (name == "Scripts") return {ui::fIcons, "\xef\x84\xa1"};  // U+F121 </>, Astral's scripting tab icon
    return {ui::fText, name.c_str()};
}

float Tabs(ImDrawList* dl, ImVec2 p, float w) {  // page names, right-aligned on the header; returns where they start
    const int pages = static_cast<int>(g_pages.size());
    const float h = ui::S(HEAD);
    g_tabRects.assign(pages, ImRect());
    float x = p.x + w - ui::S(16), barX = 0, barW = 0;
    for (int i = pages - 1; i >= 0; --i) {
        const auto [font, name] = TabLabel(g_pages[i].name);
        const bool icon = font == ui::fIcons;
        const float tw = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, name).x;
        x -= tw;
        const ImRect bb(ImVec2(x - ui::S(9), p.y), ImVec2(x + tw + ui::S(9), p.y + h));
        g_tabRects[i] = bb;
        ImGui::PushID(1000 + i);
        ImGui::SetCursorScreenPos(bb.Min);
        if (ImGui::InvisibleButton("##tab", bb.GetSize()) && g_tab != i) g_tab = i, g_pageT = 0, MarkDirty();
        const float on = ui::anim(ImGui::GetItemID(), g_tab == i ? 1.f : 0.f), hv = ui::anim(ImGui::GetItemID() + 1, ImGui::IsItemHovered() ? 1.f : 0.f);
        ImGui::PopID();
        const float rest = icon ? 0.78f : 0.62f;  // Astral's icons: 78% at rest, full when active or hovered
        dl->AddText(font, font->FontSize, ImVec2(std::floor(x), std::floor(p.y + (h - font->FontSize) * 0.5f)),
                    ui::col(ImVec4(1, 1, 1, rest + (1 - rest) * std::max(on, hv))), name);
        if (g_tab == i) barX = x, barW = icon ? 0.f : tw;  // no underline under an icon (Astral has none)
        x -= ui::S(22);
    }
    if (pages) {
        if (g_barX < 0) g_barX = barX, g_barW = barW;
        g_barX = ui::anim(0xBA1, barX, 18), g_barW = ui::anim(0xBA2, barW, 18);
        dl->AddRectFilled(ImVec2(g_barX, p.y + h - ui::S(4)), ImVec2(g_barX + g_barW, p.y + h - ui::S(2)), ui::col(ui::rgb(255, 255, 255)), ui::S(1));
    }
    return x;
}

void Footer(ImDrawList* dl, ImVec2 p, float w) {  // Astral's: round profile picture, "name / User"
    const float h = ui::S(FOOT), cy = p.y + h * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::col(ui::pal::group), ui::S(10), ImDrawFlags_RoundCornersBottom);
    const ImVec2 ac(p.x + ui::S(19), cy), r(ui::S(12), ui::S(12));
    if (const ImTextureID av = avatar::Texture(g_device))
        dl->AddImageRounded(av, ImVec2(ac.x - r.x, ac.y - r.y), ImVec2(ac.x + r.x, ac.y + r.y), ImVec2(0, 0), ImVec2(1, 1), ui::col(ui::rgb(255, 255, 255)), ui::S(12));
    else
        dl->AddCircleFilled(ac, ui::S(12), ui::col(ui::pal::field), 24);
    const std::string name = g_user.empty() ? "User" : g_user;
    ImFont* f = ui::fText;
    const float ty = std::floor(cy - f->FontSize * 0.5f), nw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, name.c_str()).x;
    dl->AddText(f, f->FontSize, ImVec2(p.x + ui::S(38), ty), ui::col(ui::pal::textOn), name.c_str());
    dl->AddText(f, f->FontSize, ImVec2(p.x + ui::S(38) + nw + ui::S(4), ty), ui::col(ui::pal::textDim), "/ User");
    if (g_listen >= 0) {
        const char* hint = "Press a key  \xC2\xB7  Esc clears";
        const float hw = ui::fSmall->CalcTextSizeA(ui::fSmall->FontSize, FLT_MAX, 0, hint).x;
        dl->AddText(ui::fSmall, ui::fSmall->FontSize, ImVec2(p.x + w - ui::S(14) - hw, std::floor(cy - ui::fSmall->FontSize * 0.5f)), ui::col(ui::pal::textDim), hint);
    }
}

std::string Fmt(const Ctl& c) {  // a printf format for the slider's value: the step's decimals + the suffix
    int dec = 0;
    for (double s = c.step; s < 0.999 && dec < 3; s *= 10) ++dec;
    std::string suffix;
    for (char ch : c.suffix) suffix += ch == '%' ? "%%" : std::string(1, ch);
    return "%." + std::to_string(dec) + "f" + suffix;
}

void KeyChip(const Ctl& c, int index, const ImRect& row) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsMouseHoveringRect(row.Min, row.Max) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        g_listen = g_listen == index ? -1 : index, g_listenArmed = false;
    char buf[8];
    const bool listening = g_listen == index;
    const char* name = listening ? "..." : c.key > 0 ? KeyName(c.key, buf) : nullptr;
    if (!name) return;
    ImFont* f = ui::fSmall;
    const float tw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, name).x, cw = std::max(ui::S(24), tw + ui::S(12)), ch = ui::S(20);
    const ImVec2 a(row.Max.x - cw, std::floor((row.Min.y + row.Max.y - ch) * 0.5f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + ch), ui::col(ui::pal::field), ui::S(4));
    dl->AddText(f, f->FontSize, ImVec2(std::floor(a.x + (cw - tw) / 2), std::floor(a.y + (ch - f->FontSize) / 2)),
                ui::col(listening ? ui::accent() : ui::pal::textOff), name);
}

ImU32 Red(float a = 1.f) { return ui::col(ui::rgb(232, 96, 96), a); }

int ItemState(const Ctl& c, int i) { return static_cast<int>(Val(c.slot + 1 + i)); }
void Toggle(const Ctl& c, int i) { Set(c.slot + 1 + i, ItemState(c, i) == 1 ? 0 : 1); }

// Astral's script list: a search line, then one row per item with a state circle (click it or double-click the row
// to switch the item on/off; click the row to select it). Values: slot = selected, slot + 1 + i = item i (0 off, 1 on, 2 failed).
void ListRow(Ctl& c) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const float wd = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::S(12), ui::S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui::S(4));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::pal::field);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ui::pal::textDim);
    ImGui::SetNextItemWidth(wd);
    ImGui::InputTextWithHint("##search", "Search", c.filter, sizeof(c.filter));
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::Dummy(ImVec2(0, ui::S(8)));

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ui::S(static_cast<float>(c.def));
    w->DrawList->AddRectFilled(p, ImVec2(p.x + wd, p.y + h), ui::col(ui::pal::field), ui::S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, ui::S(6)));
    ImGui::BeginChild("##list", ImVec2(wd, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string needle = c.filter;
    for (char& ch : needle) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    const int sel = static_cast<int>(Val(c.slot));
    const float rowH = ui::S(26);
    int shown = 0;
    for (int i = 0; i < static_cast<int>(c.items.size()); ++i) {
        if (!needle.empty()) {
            std::string hay = c.items[i].first + " " + c.items[i].second;
            for (char& ch : hay) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (hay.find(needle) == std::string::npos) continue;
        }
        ++shown;
        ImGui::PushID(i);
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const float rw = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##it", ImVec2(rw, rowH));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (ImGui::GetIO().MousePos.x < r0.x + ui::S(30) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Toggle(c, i);
            Set(c.slot, i);
        }
        const float hv = ui::anim(ImGui::GetItemID() + 9, hov ? 1.f : 0.f, 18.f);
        const int st = ItemState(c, i);
        const float on = ui::anim(ImGui::GetItemID() + 10, st == 1 ? 1.f : 0.f, 16.f);
        const ImVec2 cc(r0.x + ui::S(16), r0.y + rowH * 0.5f);
        const float rad = ui::S(6.5f);
        if (st == 2) {
            dl->AddCircleFilled(cc, rad, Red());
            dl->AddLine(ImVec2(cc.x, cc.y - ui::S(3.5f)), ImVec2(cc.x, cc.y + ui::S(0.8f)), IM_COL32_WHITE, ui::S(1.6f));
            dl->AddCircleFilled(ImVec2(cc.x, cc.y + ui::S(3)), ui::S(0.9f), IM_COL32_WHITE);
        } else {
            dl->AddCircle(cc, rad, ui::col(ui::pal::textDim, 1 - on), 20, ui::S(1.5f));
            if (on > 0.01f) {
                dl->AddCircleFilled(cc, rad, ui::col(ui::accent(), on), 20);
                const ImVec2 a(cc.x - ui::S(3), cc.y), m(cc.x - ui::S(0.8f), cc.y + ui::S(2.4f)), e(cc.x + ui::S(3.2f), cc.y - ui::S(2.4f));
                dl->AddLine(a, m, ui::col(ui::rgb(255, 255, 255), on), ui::S(1.6f));
                dl->AddLine(m, e, ui::col(ui::rgb(255, 255, 255), on), ui::S(1.6f));
            }
        }
        const ImVec4 tc = sel == i ? ui::accent() : ImVec4(ui::pal::label.x + (ui::pal::textOn.x - ui::pal::label.x) * hv,
                                                           ui::pal::label.y + (ui::pal::textOn.y - ui::pal::label.y) * hv,
                                                           ui::pal::label.z + (ui::pal::textOn.z - ui::pal::label.z) * hv, 1.f);
        const float tx = r0.x + ui::S(30) + hv * ui::S(2), right = r0.x + rw - ui::S(10);
        dl->PushClipRect(ImVec2(tx, r0.y), ImVec2(right, r0.y + rowH), true);
        dl->AddText(ui::fText, ui::fText->FontSize, ImVec2(std::floor(tx), std::floor(r0.y + (rowH - ui::fText->FontSize) * 0.5f)), ui::col(tc), c.items[i].first.c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
    if (!shown) ui::text("  No matches", ui::pal::textDim);
    const float maxScroll = ImGui::GetScrollMaxY();
    if (maxScroll > 0) {
        ImGuiWindow* cw = ImGui::GetCurrentWindow();
        const float y1 = cw->Pos.y + cw->Size.y;
        if (ImGui::GetScrollY() < maxScroll - 1.f) {  // more below: the last rows fade into the list
            const ImU32 clear = ui::col(ImVec4(ui::pal::field.x, ui::pal::field.y, ui::pal::field.z, 0.f)), solid = ui::col(ui::pal::field);
            cw->DrawList->AddRectFilledMultiColor(ImVec2(cw->Pos.x, y1 - ui::S(30)), ImVec2(cw->Pos.x + cw->Size.x - ui::S(9), y1), clear, clear, solid, solid);
        }
        const float trackH = cw->Size.y - ui::S(16);
        const float thumb = std::max(ui::S(24), trackH * cw->Size.y / (cw->Size.y + maxScroll));
        const float y = cw->Pos.y + ui::S(8) + (trackH - thumb) * (ImGui::GetScrollY() / maxScroll);
        cw->DrawList->AddRectFilled(ImVec2(cw->Pos.x + cw->Size.x - ui::S(7), y), ImVec2(cw->Pos.x + cw->Size.x - ui::S(4), y + thumb), ui::col(ui::accent()), ui::S(2));
    }
    ImGui::EndChild();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ui::S(4)));
    ImGui::Dummy(ImVec2(0, 0));
}

// the selected item of a list: its title, author and state, with a button to switch it
void InfoRow(Ctl& c) {
    const Ctl* list = nullptr;
    for (const Ctl& o : g_ctls)
        if (o.kind == kList && o.slot == static_cast<int>(c.def)) list = &o;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float wd = ImGui::GetContentRegionAvail().x;
    const int i = list ? static_cast<int>(Val(list->slot)) : -1;
    if (!list || i < 0 || i >= static_cast<int>(list->items.size())) {
        ui::text("Pick a script on the left", ui::pal::textDim);
        return;
    }
    const auto& [title, author] = list->items[i];
    ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ui::fTitle->CalcTextSizeA(ui::fTitle->FontSize, FLT_MAX, wd, title.c_str());
    dl->AddText(ui::fTitle, ui::fTitle->FontSize, p, ui::col(ui::pal::textOn), title.c_str(), nullptr, wd);
    p.y += ts.y + ui::S(4);
    const std::string by = "by " + author;
    dl->AddText(ui::fText, ui::fText->FontSize, p, ui::col(ui::pal::textDim), by.c_str());
    p.y += ui::fText->FontSize + ui::S(14);
    const int st = ItemState(*list, i);
    const char* state = st == 1 ? "On" : st == 2 ? "Failed to load (see the console)" : "Off";
    dl->AddCircleFilled(ImVec2(p.x + ui::S(5), p.y + ui::fText->FontSize * 0.5f), ui::S(4),
                        st == 1 ? ui::col(ui::accent()) : st == 2 ? Red() : ui::col(ui::pal::textDim));
    dl->AddText(ui::fText, ui::fText->FontSize, ImVec2(p.x + ui::S(16), p.y), st == 2 ? Red() : ui::col(ui::pal::label), state);
    p.y += ui::fText->FontSize + ui::S(14);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(0, 0));
    const char* note = st == 2 ? "Click its circle to try again." : "Click its circle (or double-click the name) to switch it. Off unloads it and hides its menu items.";
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const ImVec2 ns = ui::fSmall->CalcTextSizeA(ui::fSmall->FontSize, FLT_MAX, wd, note);
    dl->AddText(ui::fSmall, ui::fSmall->FontSize, q, ui::col(ui::pal::textDim), note, nullptr, wd);
    ImGui::Dummy(ImVec2(0, ns.y + ui::S(4)));
}

void Row(Ctl& c, int index) {
    ImGui::PushID(index);
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    if (c.kind == kCheck) {
        bool v = Val(c.slot) != 0;
        if (ui::checkbox(c.name.c_str(), &v)) Set(c.slot, v ? 1 : 0);
    } else if (c.kind == kSlider) {
        float v = static_cast<float>(Val(c.slot));
        const std::string fmt = Fmt(c);
        if (ui::slider(c.name.c_str(), &v, static_cast<float>(c.min), static_cast<float>(c.max), fmt.c_str())) {
            const double step = c.step > 0 ? c.step : 1;
            Set(c.slot, std::clamp(std::round(v / step) * step, c.min, c.max));
        }
    } else if (c.kind == kList) {
        ListRow(c);
    } else if (c.kind == kInfo) {
        InfoRow(c);
    } else {
        std::vector<const char*> items;
        for (const std::string& o : c.opts) items.push_back(o.c_str());
        int v = static_cast<int>(Val(c.slot));
        if (ui::combo(c.name.c_str(), &v, items.data(), static_cast<int>(items.size()))) Set(c.slot, v);
    }
    const ImRect row(top, ImVec2(top.x + w, ImGui::GetCursorScreenPos().y));
    g_rowRects[c.slot] = row;
    if (c.kind == kCheck && c.bindable) KeyChip(c, index, row);
    ImGui::PopID();
}

void Window() {
    ImGuiIO& io = ImGui::GetIO();
    const int pages = static_cast<int>(g_pages.size());
    g_tab = std::clamp(g_tab, 0, std::max(0, pages - 1));
    const float maxH = io.DisplaySize.y / S - 20;
    const float H = std::min(HEAD + PADY + std::max(g_want, 40.f) + PADY + FOOT, maxH);
    bool two = false;  // a page with groups in one column gets a one-column window, not a wide strip
    if (pages) for (const Group& g : g_pages[g_tab].groups) two |= g.col != 0;
    float tabsW = 0;
    for (const Page& pg : g_pages) {
        const auto [font, label] = TabLabel(pg.name);
        tabsW += font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, label).x / S + 22;
    }
    const float W = ui::anim(0x57D, two ? 600.f : std::max(380.f, 230.f + tabsW), 16);
    g_cols = two ? 2 : 1;
    if (g_x < 0 || g_y < 0) g_x = std::max(0.f, (io.DisplaySize.x / S - W) / 2), g_y = std::max(0.f, (io.DisplaySize.y / S - H) / 2);
    g_x = std::clamp(g_x, 0.f, std::max(0.f, io.DisplaySize.x / S - W));
    g_y = std::clamp(g_y, 0.f, std::max(0.f, io.DisplaySize.y / S - HEAD));
    const ImVec2 p(std::round(g_x * S), std::round(g_y * S)), size(std::round(W * S), std::round(H * S));

    ImGui::SetNextWindowPos(p);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_openT);
    ImGui::Begin("##alm", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, p.y + ui::S(HEAD - 1)), ImVec2(p.x + size.x, p.y + size.y - ui::S(FOOT - 1)), ui::col(ui::pal::bg));
    Header(dl, p, size.x, p.x + size.x - ui::S(16 + 9 + 12 + tabsW - 22));
    Footer(dl, ImVec2(p.x, p.y + size.y - ui::S(FOOT)), size.x);
    const float tabsAt = Tabs(dl, p, size.x);

    const ImRect drag(p, ImVec2(tabsAt, p.y + ui::S(HEAD)));
    if (ImGui::IsMouseClicked(0) && drag.Contains(io.MousePos) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) g_drag = true;
    if (!ImGui::IsMouseDown(0)) g_drag = false;
    if (g_drag && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) g_x += io.MouseDelta.x / S, g_y += io.MouseDelta.y / S, MarkDirty();

    if (g_listen >= 0 && g_listenArmed && ImGui::IsMouseClicked(0)) g_listen = -1;  // a click anywhere cancels the bind
    g_pageT = std::min(1.f, g_pageT + io.DeltaTime / 0.16f);
    const float e = ui::ease(g_pageT);
    const ImVec2 pageMin(p.x + ui::S(PADX), p.y + ui::S(HEAD + PADY) + (1 - e) * ui::S(7));
    const ImVec2 pageSize(size.x - ui::S(2 * PADX), size.y - ui::S(HEAD + PADY) - ui::S(PADY + FOOT));
    ImGui::SetCursorScreenPos(pageMin);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_openT * e);
    ImGui::BeginChild("##page", pageSize, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ui::page(pageMin, ImVec2(pageMin.x + pageSize.x, pageMin.y + pageSize.y));
    if (pages) {
        ImGui::PushID(g_tab);
        float colH[2] = {0, 0};
        auto& groups = g_pages[g_tab].groups;
        for (int col = 0; col < 2; ++col) {
            if (col >= g_cols) break;
            ui::column(col, g_cols);
            for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi) {
                Group& g = groups[gi];
                if ((g.col ? 1 : 0) != col) continue;
                ImGui::PushID(gi);
                ui::beginGroup(g.name.c_str());
                colH[col] += ui::lastGroupWant() + ui::S(15);
                for (int i : g.ctls) Row(g_ctls[i], i);
                ui::endGroup();
                ImGui::PopID();
            }
        }
        ImGui::PopID();
        g_want = (std::max(colH[0], colH[1]) - ui::S(15)) / S;
    } else {
        ui::text("Waiting for the Lua...", ui::pal::textDim);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
    ImGui::PopStyleVar();
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
    g_pages.clear(), g_ctls.clear(), g_rowRects.clear(), g_listen = -1;
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
        else if (f[0] == "list" && f.size() >= 5) {
            c.kind = kList;
            for (const std::string& it : Split(f[4], '|')) {
                const size_t k = it.find('^');
                c.items.push_back(k == std::string::npos ? std::make_pair(it, std::string()) : std::make_pair(it.substr(0, k), it.substr(k + 1)));
            }
            if (c.slot + static_cast<int>(c.items.size()) >= kSlots) continue;
            for (size_t i = 0; i < c.items.size(); ++i) {
                const auto it = g_cfg.find(c.id + "/" + c.items[i].first);
                g_vals[c.slot + 1 + i] = it != g_cfg.end() && it->second.first == 1 ? 1 : 0;
            }
        } else if (f[0] == "info") c.kind = kInfo;
        else continue;
        double v = c.def;
        if (auto it = g_cfg.find(c.id); it != g_cfg.end()) v = it->second.first, c.key = c.bindable ? it->second.second : 0;
        else g_dirty = true, g_dirtyAt = 0;  // a new control: into menu.cfg with its default
        if (c.kind == kSlider) v = std::clamp(v, c.min, c.max);
        if (c.kind == kCombo) v = std::clamp(std::round(v), 0.0, std::max(0.0, c.opts.size() - 1.0));
        if (c.kind == kList) v = std::clamp(std::round(v), -1.0, c.items.size() - 1.0);
        if (c.kind == kInfo) v = 0;
        g_vals[c.slot] = v;
        group.ctls.push_back(static_cast<int>(g_ctls.size()));
        g_ctls.push_back(std::move(c));
    }
}

double Get(int slot) { return Val(slot); }

void SetUser(const std::string& name) {
    std::lock_guard lock(g_lock);
    g_user = name;
}

void SetDevice(ID3D11Device* device) {
    g_device = device;
    avatar::Start();
}

void SetValue(int slot, double v) {  // Lua thread: no ImGui here
    if (slot >= 0 && slot < kSlots) g_vals[slot] = v;
}

void SetState(bool open, int r, int g, int b) {
    g_open = open;
    if (r >= 0) g_accent = (std::clamp(r, 0, 255) << 16) | (std::clamp(g, 0, 255) << 8) | std::clamp(b, 0, 255);
}

void Fonts(float scale) {
    S = scale;
    ui::bindMenus = false;  // keys bind the loader's way (right-click a checkbox), saved in menu.cfg
    ui::setup(scale);
}

void Frame() {
    std::lock_guard lock(g_lock);
    Keys();
    const int a = g_accent.load();
    cfg::accent = ImVec4(((a >> 16) & 255) / 255.f, ((a >> 8) & 255) / 255.f, (a & 255) / 255.f, 1.f);
    if (!g_open) {
        g_openT = 0, g_drag = false;
        if (g_listen >= 0) g_listen = -1;
    } else {
        if (g_openT == 0) g_openAt = static_cast<float>(ImGui::GetTime());
        g_openT = std::min(1.f, g_openT + ImGui::GetIO().DeltaTime / 0.067f);
        Window();
    }
    g_wantMouse = g_open && ImGui::GetIO().WantCaptureMouse;
    if (g_dirty && ImGui::GetTime() - g_dirtyAt > 0.5) SaveLocked();
}

bool RowAt(int slot, float* x, float* y) {
    std::lock_guard lock(g_lock);
    const auto it = g_rowRects.find(slot);
    if (it == g_rowRects.end()) return false;
    *x = (it->second.Min.x + it->second.Max.x) / 2, *y = (it->second.Min.y + it->second.Max.y) / 2;
    return true;
}

bool RowBox(int slot, float* x0, float* y0) {
    std::lock_guard lock(g_lock);
    const auto it = g_rowRects.find(slot);
    if (it == g_rowRects.end()) return false;
    *x0 = it->second.Min.x, *y0 = it->second.Min.y;
    return true;
}

bool TabAt(int page, float* x, float* y) {
    std::lock_guard lock(g_lock);
    if (page < 0 || page >= static_cast<int>(g_tabRects.size())) return false;
    *x = g_tabRects[page].GetCenter().x, *y = g_tabRects[page].GetCenter().y;
    return true;
}

bool WantsMouse() { return g_wantMouse; }

void Save() {
    std::lock_guard lock(g_lock);
    if (g_dirty) SaveLocked();
}

}  // namespace alm::menu
