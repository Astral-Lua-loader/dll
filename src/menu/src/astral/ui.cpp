#include "ui.h"

#include <windows.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>

#include <imgui_internal.h>

#include "config.h"
#include "fonts.h"

namespace ui {

ImFont *fText = nullptr, *fTitle = nullptr, *fHeader = nullptr, *fIcons = nullptr, *fSmall = nullptr;

namespace {

float g_scale = 1.f;
std::unordered_map<ImGuiID, float> g_anim, g_height;
std::string g_search;
ImRect g_lastRow;
ImGuiID g_lastRowId = 0;
float g_lastWant = 0.f;
float g_hoverTime = 0.f;
ImVec2 g_pageMin, g_pageMax;
float g_colX = 0.f, g_colW = 0.f;
struct GroupFrame { ImVec2 pos; float w, h; ImGuiID id; };
std::vector<GroupFrame> g_groups;
std::vector<Bind> g_binds;
ImGuiID g_waitKey = 0;  // the keybind button waiting for a key
std::string g_nextKey;   // cfgKey() for the next row
bool g_inBindMenu = false;
void bindMenu(const char* label, const std::string& cfgKey, int kind, void* target, float min = 0.f, float max = 1.f,
              const char* fmt = "%.0f", const char* const* items = nullptr, int count = 0);
std::string takeKey() {
    std::string k;
    k.swap(g_nextKey);
    return k;
}
bool g_keysAtWait[256];

ImVec4 lerp(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
ImRect lerp(const ImRect& a, const ImRect& b, float t) { return ImRect(ImLerp(a.Min, b.Min, t), ImLerp(a.Max, b.Max, t)); }

bool matches(const char* label) {
    if (g_search.empty() || !label) return true;
    std::string a(label), b(g_search);
    for (auto& c : a) c = (char)tolower((unsigned char)c);
    for (auto& c : b) c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
}

struct Dim {  // rows the search doesn't match fade back
    bool on;
    explicit Dim(const char* label) : on(!matches(label)) {
        if (on) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.22f);
    }
    ~Dim() {
        if (on) ImGui::PopStyleVar();
    }
};

const char* visible(const char* label) {  // text before "##"
    static char buf[128];
    const char* hash = std::strstr(label, "##");
    if (!hash) return label;
    std::snprintf(buf, sizeof(buf), "%.*s", (int)(hash - label), label);
    return buf;
}

// allocates a full-width row of height h; returns the interaction state
bool row(const char* id, float h, ImRect& bb, bool* hovered = nullptr, bool* held = nullptr, float hitRight = 0.f) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImVec2 p = w->DC.CursorPos;
    bb = ImRect(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + h));
    ImGui::ItemSize(bb);
    const ImGuiID iid = w->GetID(id);
    ImRect hit = bb;
    hit.Max.x -= hitRight;
    g_lastRow = bb;
    if (!ImGui::ItemAdd(hit, iid)) return false;
    bool hov = false, hl = false;
    const bool pressed = ImGui::ButtonBehavior(hit, iid, &hov, &hl);
    if (hov && g_lastRowId == iid) g_hoverTime += ImGui::GetIO().DeltaTime;
    else if (hov) g_lastRowId = iid, g_hoverTime = 0.f;
    else if (g_lastRowId == iid) g_lastRowId = 0;
    if (hovered) *hovered = hov;
    if (held) *held = hl;
    return pressed;
}

void textAt(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 c, const char* s, const char* end = nullptr) {
    dl->AddText(f, f->FontSize, ImVec2(std::floor(p.x), std::floor(p.y)), c, s, end);
}
ImVec2 measure(ImFont* f, const char* s) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, s); }
float midY(const ImRect& r, ImFont* f) { return std::floor((r.Min.y + r.Max.y - f->FontSize) * 0.5f); }

// text clipped at a width with the last letters fading out (Astral's long multi-select values)
void textFade(ImDrawList* dl, ImFont* f, ImVec2 p, float maxW, const ImVec4& c, const char* s) {
    dl->PushClipRect(p, ImVec2(p.x + maxW, p.y + f->FontSize + 2), true);
    textAt(dl, f, p, col(c), s);
    dl->PopClipRect();
}

void plusGlyph(ImDrawList* dl, ImVec2 c, float r, ImU32 color, bool minus = false) {
    const float t = std::max(1.f, S(1.6f));
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), color, t);
    if (!minus) dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), color, t);
}

// soft halos fading out from a shape: the video's "+" buttons, ticked boxes and group edges
void glowCircle(ImDrawList* dl, ImVec2 c, float r, const ImVec4& color, float alpha, float spread) {
    for (int i = 8; i >= 1; --i) {
        const float k = i / 8.f;
        dl->AddCircleFilled(c, r + spread * k, col(color, alpha * (1.f - k) * (1.f - k) * 0.55f), 32);
    }
}
void glowRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, const ImVec4& color, float alpha, float spread) {
    for (int i = 8; i >= 1; --i) {
        const float k = i / 8.f, g = spread * k;
        dl->AddRectFilled(a - ImVec2(g, g), b + ImVec2(g, g), col(color, alpha * (1.f - k) * (1.f - k) * 0.55f), rounding + g);
    }
}

void chevron(ImDrawList* dl, ImVec2 c, float r, float turn, ImU32 color) {  // turn 0 = down, 1 = up
    const float a = turn * IM_PI;
    const float cs = std::cos(a), sn = std::sin(a);
    auto rot = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
    const ImVec2 pts[3] = { rot(-r, -r * 0.5f), rot(0, r * 0.5f), rot(r, -r * 0.5f) };
    dl->AddPolyline(pts, 3, color, 0, std::max(1.f, S(2.f)));
}

void popupBg(ImDrawList* dl, const ImRect& r, float alpha, const ImVec4& fill = rgb(41, 46, 61), float rounding = -1.f) {
    if (rounding < 0) rounding = S(7);
    dl->AddRectFilled(ImVec2(r.Min.x - 1, r.Min.y - 1), ImVec2(r.Max.x + 1, r.Max.y + 1), col(rgb(64, 70, 92), alpha), rounding + 1);
    dl->AddRectFilled(r.Min, r.Max, col(fill, alpha), rounding);
}

// a popup opened from a rect; the content fades in after the window has grown out of `from`
struct PopupState { ImRect from; bool morph; };
std::unordered_map<ImGuiID, PopupState> g_popups;

void openPopupAt(const char* id, const ImRect& from, bool morph) {
    const ImGuiID pid = ImGui::GetID(id);
    g_popups[pid] = { from, morph };
    g_anim[pid] = 0.f;
    ImGui::OpenPopup(id);
}

bool beginPopupAt(const char* id, ImVec2 pos, ImVec2 pivot, float width, const ImVec4* morphFrom = nullptr) {
    const ImGuiID pid = ImGui::GetID(id);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(11), S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    const bool open = ImGui::BeginPopup(id, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open) return false;
    const float t = anim(pid, 1.f, 9.f);
    const PopupState& st = g_popups[pid];
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImRect win(w->Pos, w->Pos + w->Size);
    ImDrawList* dl = w->DrawList;
    dl->PushClipRectFullScreen();
    if (st.morph && morphFrom) {  // the swatch swells into the picker, its colour giving way to the panel's
        const float e = ease(std::min(1.f, t * 1.15f));
        const float r = ImLerp(st.from.GetWidth() * 0.5f, S(6), e);
        popupBg(dl, lerp(st.from, win, e), 1.f, lerp(*morphFrom, rgb(41, 46, 61), ImClamp((e - 0.6f) / 0.4f, 0.f, 1.f)), r);
    } else {
        const float e = ease(t);
        ImRect r = win;
        r.Min.y += (1.f - e) * S(6);
        popupBg(dl, r, e);
    }
    dl->PopClipRect();
    const float contentAlpha = st.morph ? ImClamp((t - 0.55f) / 0.45f, 0.f, 1.f) : ease(t);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, contentAlpha);
    return true;
}

void endPopupAt() {
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) { ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b); }

struct PickerExtra { float rainbow = 0.f, pulse = 0.f; };
std::unordered_map<const ImVec4*, PickerExtra> g_extra;
std::unordered_map<const ImVec4*, float> g_hue;  // the hue kept while the colour is grey

void picker(ImVec4* c) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImDrawList* dl = w->DrawList;
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(c->x, c->y, c->z, h, s, v);
    if (s < 0.001f || v < 0.001f) h = g_hue[c];
    const ImVec2 p = w->DC.CursorPos;
    const float sq = S(196), barW = S(9), colGap = S(14);  // sizes from the video's picker
    const ImRect sv(p, p + ImVec2(sq, S(150)));
    const ImRect hb(ImVec2(sv.Max.x + S(6), sv.Min.y), ImVec2(sv.Max.x + S(6) + barW, sv.Max.y));
    // saturation/value square
    float hr, hg, hbb;
    hsvToRgb(h, 1, 1, hr, hg, hbb);
    const ImU32 hue = col(ImVec4(hr, hg, hbb, 1));
    dl->AddRectFilledMultiColor(sv.Min, sv.Max, col(rgb(255, 255, 255)), hue, hue, col(rgb(255, 255, 255)));
    dl->AddRectFilledMultiColor(sv.Min, sv.Max, 0, 0, col(rgb(0, 0, 0)), col(rgb(0, 0, 0)));
    ImGui::SetCursorScreenPos(sv.Min);
    ImGui::InvisibleButton("##sv", sv.GetSize());
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        s = ImSaturate((m.x - sv.Min.x) / sv.GetWidth());
        v = 1.f - ImSaturate((m.y - sv.Min.y) / sv.GetHeight());
    }
    // hue bar
    const ImU32 hues[7] = { col(rgb(255, 0, 0)), col(rgb(255, 255, 0)), col(rgb(0, 255, 0)), col(rgb(0, 255, 255)),
                            col(rgb(0, 0, 255)), col(rgb(255, 0, 255)), col(rgb(255, 0, 0)) };
    for (int i = 0; i < 6; ++i) {
        const float y0 = hb.Min.y + hb.GetHeight() * i / 6.f, y1 = hb.Min.y + hb.GetHeight() * (i + 1) / 6.f;
        dl->AddRectFilledMultiColor(ImVec2(hb.Min.x, y0), ImVec2(hb.Max.x, y1), hues[i], hues[i], hues[i + 1], hues[i + 1]);
    }
    ImGui::SetCursorScreenPos(hb.Min);
    ImGui::InvisibleButton("##hue", hb.GetSize());
    if (ImGui::IsItemActive()) h = ImSaturate((ImGui::GetIO().MousePos.y - hb.Min.y) / hb.GetHeight()) * 0.9999f;
    g_hue[c] = h;
    dl->AddCircle(ImVec2(sv.Min.x + s * sv.GetWidth(), sv.Min.y + (1 - v) * sv.GetHeight()), S(3.5f), col(rgb(255, 255, 255)), 12, S(1.5f));
    dl->AddRectFilled(ImVec2(hb.Min.x - S(1), hb.Min.y + h * hb.GetHeight() - S(1.5f)),
                      ImVec2(hb.Max.x + S(1), hb.Min.y + h * hb.GetHeight() + S(1.5f)), col(rgb(255, 255, 255)), S(1));
    hsvToRgb(h, s, v, c->x, c->y, c->z);
    // right column: Rainbow / Pulse / Copy / Paste
    const float rx = hb.Max.x + colGap;
    PickerExtra& ex = g_extra[c];
    ImGui::SetCursorScreenPos(ImVec2(rx, sv.Min.y - S(4)));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(-1);
    ImGui::BeginChild("##pickcol", ImVec2(S(128), sv.GetHeight() + S(6)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    slider("Rainbow", &ex.rainbow, 0, 10, "%.0f");
    slider("Pulse", &ex.pulse, 0, 10, "%.0f");
    if (button("Copy")) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X", (int)(c->x * 255), (int)(c->y * 255), (int)(c->z * 255), (int)(c->w * 255));
        ImGui::SetClipboardText(hex);
    }
    if (button("Paste")) {
        unsigned r = 0, g = 0, b = 0, a = 255;
        if (const char* t = ImGui::GetClipboardText(); t && std::sscanf(t[0] == '#' ? t + 1 : t, "%02x%02x%02x%02x", &r, &g, &b, &a) >= 3)
            *c = rgb((int)r, (int)g, (int)b, (int)a);
    }
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p.x, sv.Max.y + S(2)));
    ImGui::Dummy(ImVec2(1, 1));
}



}  // namespace

// ---------------------------------------------------------------- basics

void setup(float scale) {
    g_scale = scale;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = 2;
    static const ImWchar latin[] = { 0x0020, 0x00FF, 0x2026, 0x2026, 0 };
    static const ImWchar icons[] = { 0xE000, 0xF8FF, 0 };
    fText = io.Fonts->AddFontFromMemoryTTF((void*)kBold, sizeof(kBold), std::round(18.f * scale), &cfg, latin);
    fTitle = io.Fonts->AddFontFromMemoryTTF((void*)kBold, sizeof(kBold), std::round(20.5f * scale), &cfg, latin);
    fHeader = io.Fonts->AddFontFromMemoryTTF((void*)kBold, sizeof(kBold), std::round(24.f * scale), &cfg, latin);
    fSmall = io.Fonts->AddFontFromMemoryTTF((void*)kBold, sizeof(kBold), std::round(15.f * scale), &cfg, latin);
    fIcons = io.Fonts->AddFontFromMemoryTTF((void*)kIcons, sizeof(kIcons), std::round(24.f * scale), &cfg, icons);
    io.Fonts->Build();
    io.FontDefault = fText;

    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowPadding = ImVec2(0, 0), st.ItemSpacing = ImVec2(0, 0), st.ItemInnerSpacing = ImVec2(0, 0);
    st.WindowBorderSize = 0, st.ChildBorderSize = 0, st.PopupBorderSize = 0, st.FrameBorderSize = 0;
    st.FrameRounding = S(4), st.WindowRounding = S(10), st.PopupRounding = S(7);
    st.Colors[ImGuiCol_Text] = pal::textOn;
    st.Colors[ImGuiCol_FrameBg] = pal::field;
    st.Colors[ImGuiCol_FrameBgHovered] = pal::field;
    st.Colors[ImGuiCol_FrameBgActive] = pal::field;
    st.Colors[ImGuiCol_TextSelectedBg] = ImVec4(accent().x, accent().y, accent().z, 0.35f);
    st.Colors[ImGuiCol_NavHighlight] = ImVec4(0, 0, 0, 0);
}

bool fontsDirty() {
    static const float scales[] = { 0.75f, 1.f, 1.25f, 1.5f };
    return scales[ImClamp(cfg::renderScale, 0, 3)] != g_scale;
}
float S(float px) { return px * g_scale; }
float scale() { return g_scale; }

ImVec4 rgb(int r, int g, int b, int a) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f); }
ImU32 col(const ImVec4& c, float alpha) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

ImVec4 accent() {
    ImVec4 c = cfg::accent;
    const PickerExtra& ex = g_extra[&cfg::accent];
    const double t = ImGui::GetTime();
    if (ex.rainbow > 0) {
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
        h = std::fmod(h + (float)t * ex.rainbow * 0.05f, 1.f);
        ImGui::ColorConvertHSVtoRGB(h, s, v, c.x, c.y, c.z);
    }
    if (ex.pulse > 0) c.w *= 0.55f + 0.45f * (0.5f + 0.5f * std::sin((float)t * ex.pulse));
    return c;
}
ImVec4 accent2() {
    ImVec4 c = accent();
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
    ImGui::ColorConvertHSVtoRGB(std::fmod(h + 30.f / 360.f, 1.f), s, v, c.x, c.y, c.z);
    return c;
}

float ease(float t) {
    t = ImSaturate(t);
    return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
}
float anim(ImGuiID id, float target, float speed) {
    auto it = g_anim.find(id);
    if (it == g_anim.end()) return g_anim[id] = target;
    float& v = it->second;
    v += (target - v) * (1.f - std::exp(-speed * ImGui::GetIO().DeltaTime));
    if (std::fabs(target - v) < 0.002f) v = target;
    return v;
}
float anim(const char* id, float target, float speed) { return anim(ImGui::GetID(id), target, speed); }

std::string& search() { return g_search; }

// ---------------------------------------------------------------- layout

float lastGroupWant() { return g_lastWant; }

void page(ImVec2 min, ImVec2 max) {
    g_pageMin = min, g_pageMax = max;
    g_colX = min.x, g_colW = max.x - min.x;
}

void column(int index, int of, float top) {
    const float gapX = S(15);
    g_colW = (g_pageMax.x - g_pageMin.x - gapX * (of - 1)) / of;
    g_colX = g_pageMin.x + index * (g_colW + gapX);
    ImGui::SetCursorScreenPos(ImVec2(g_colX, g_pageMin.y + top));
}

void beginGroup(const char* title, float height) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImVec2 pos(g_colX, w->DC.CursorPos.y);
    const ImGuiID id = w->GetID(title);
    const bool plain = std::strncmp(title, "##plain", 7) == 0;  // no box: the skins pages sit straight on the window
    const float titleH = title[0] && title[0] != '#' ? S(33) : plain ? 0.f : S(4);
    const float maxH = g_pageMax.y - pos.y;
    float h = height > 0 ? S(height) : titleH + (g_height.count(id) ? g_height[id] : S(40)) + S(3);
    g_lastWant = h;
    h = std::min(h, maxH);
    ImDrawList* dl = w->DrawList;
    if (!plain) {
        dl->AddRect(pos - ImVec2(1, 1), pos + ImVec2(g_colW + 1, h + 1), col(rgb(31, 36, 51)), S(8) + 1, 0, 1.f);
        dl->AddRectFilled(pos, pos + ImVec2(g_colW, h), col(pal::group), S(8));
    }
    if (titleH > S(10)) {
        const char* t = visible(title);
        const ImVec2 ts = measure(fTitle, t);
        textAt(dl, fTitle, ImVec2(pos.x + (g_colW - ts.x) * 0.5f, pos.y + (titleH - ts.y) * 0.5f - S(2)), col(pal::textOn), t);
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + titleH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(7)));
    ImGui::BeginChild(title, ImVec2(g_colW, h - titleH - S(3)), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    g_groups.push_back({ pos, g_colW, h, id });
}

void endGroup() {
    const GroupFrame gf = g_groups.back();
    g_groups.pop_back();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    g_height[gf.id] = ImGui::GetCursorPosY() + S(7);  // + the bottom padding: fits without scrolling
    const float maxScroll = ImGui::GetScrollMaxY();
    if (maxScroll > 0) {  // Astral's thin accent scrollbar
        const ImRect r(w->Pos, w->Pos + w->Size);
        const float trackH = r.GetHeight() - S(8);
        const float thumb = std::max(S(24), trackH * r.GetHeight() / (r.GetHeight() + maxScroll));
        const float y = r.Min.y + S(4) + (trackH - thumb) * (ImGui::GetScrollY() / maxScroll);
        w->DrawList->AddRectFilled(ImVec2(r.Max.x - S(7), y), ImVec2(r.Max.x - S(4), y + thumb), col(accent()), S(2));
    }
    ImGui::EndChild();
    ImGui::SetCursorScreenPos(ImVec2(gf.pos.x, gf.pos.y + gf.h + S(15)));
    g_colX = gf.pos.x;
}

void gap(float px) { ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0, S(px))); }

void tip(const char* text) {
    if (!g_lastRowId || g_lastRowId != ImGui::GetItemID() || g_hoverTime < 0.45f) return;
    const float a = ImSaturate((g_hoverTime - 0.45f) * 8.f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float wrap = S(170);
    const ImVec2 ts = fSmall->CalcTextSizeA(fSmall->FontSize, FLT_MAX, wrap, text);
    const ImVec2 p(g_lastRow.Min.x - S(4), g_lastRow.Min.y - ts.y - S(14));
    popupBg(dl, ImRect(p, p + ts + ImVec2(S(14), S(10))), a);
    dl->AddText(fSmall, fSmall->FontSize, p + ImVec2(S(7), S(5)), col(pal::textOn, a), text, nullptr, wrap);
}

// ---------------------------------------------------------------- rows

void text(const char* s, const ImVec4& c) {
    ImRect bb;
    row(s, S(22), bb);
    textAt(ImGui::GetWindowDrawList(), fText, ImVec2(bb.Min.x, midY(bb, fText)), col(c), s);
}

bool checkbox(const char* label, bool* v) {
    const std::string ck = takeKey();
    Dim dim(label);
    ImRect bb;
    bool hov = false;
    const bool pressed = row(label, S(33.7f), bb, &hov, nullptr, S(34));
    if (pressed) *v = !*v;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImDrawList* dl = w->DrawList;
    const ImGuiID id = w->GetID(label);
    const float t = anim(id, *v ? 1.f : 0.f, 16.f);
    const float hv = anim(id + 1, hov ? 1.f : 0.f, 16.f);
    const float box = S(18);
    const ImVec2 b0(bb.Min.x + S(1), std::floor((bb.Min.y + bb.Max.y - box) * 0.5f));
    dl->AddRectFilled(b0, b0 + ImVec2(box, box), col(lerp(pal::field, pal::fieldHover, hv * (1 - t))), S(4));
    if (t > 0.01f) {
        glowRect(dl, b0, b0 + ImVec2(box, box), S(4), accent(), 0.32f * t, S(5));
        dl->AddRectFilled(b0, b0 + ImVec2(box, box), col(accent(), t), S(4));
        // the tick draws itself in
        const ImVec2 a = b0 + ImVec2(box * 0.24f, box * 0.52f), m = b0 + ImVec2(box * 0.42f, box * 0.70f), e = b0 + ImVec2(box * 0.77f, box * 0.31f);
        const float k = ease(t);
        const float th = std::max(1.f, S(1.9f));
        const float first = ImSaturate(k / 0.4f), second = ImSaturate((k - 0.4f) / 0.6f);
        dl->AddLine(a, ImLerp(a, m, first), col(rgb(255, 255, 255), t), th);
        if (second > 0) dl->AddLine(m, ImLerp(m, e, second), col(rgb(255, 255, 255), t), th);
    }
    const ImVec4 tc = lerp(lerp(pal::textOff, pal::textOn, t), pal::textOn, hv * 0.6f);
    textAt(dl, fText, ImVec2(bb.Min.x + S(30), midY(bb, fText)), col(tc), visible(label));
    bindMenu(label, ck, 0, v);
    return pressed;
}

bool settings(const char* id, float width, bool popup) {
    const ImRect rowBb = g_lastRow;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const float r = S(9);
    const ImVec2 c(rowBb.Max.x - r - S(1), (rowBb.Min.y + rowBb.Max.y) * 0.5f);
    const ImRect bb(c - ImVec2(r, r), c + ImVec2(r, r));
    const ImGuiID iid = w->GetID(id);
    ImGui::ItemAdd(bb, iid);
    bool hov = false, held = false;
    if (ImGui::ButtonBehavior(bb, iid, &hov, &held) && popup) openPopupAt(id, bb, false);
    const bool open = ImGui::IsPopupOpen(id);
    const float hv = anim(iid + 1, hov || open ? 1.f : 0.f, 14.f);  // iid itself animates the popup
    const ImVec4 pc = lerp(accent(), accent2(), hv);
    glowCircle(w->DrawList, c, r, pc, 0.75f, S(7));
    w->DrawList->AddCircleFilled(c, r, col(pc), 32);
    plusGlyph(w->DrawList, c, r * 0.48f, col(rgb(255, 255, 255)));
    return popup && beginPopupAt(id, ImVec2(c.x, bb.Max.y + S(6)), ImVec2(0.5f, 0.f), S(width));
}
void endSettings() { endPopupAt(); }

bool swatch(const char* id, ImVec4* c) { return swatchAt(id, g_lastRow.Min, g_lastRow.Max, c); }

bool swatchAt(const char* id, ImVec2 rowMin, ImVec2 rowMax, ImVec4* c) {
    const ImRect rowBb(rowMin, rowMax);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const float r = S(9);
    const ImVec2 ctr(rowBb.Max.x - r - S(1), (rowBb.Min.y + rowBb.Max.y) * 0.5f);
    const ImRect bb(ctr - ImVec2(r, r), ctr + ImVec2(r, r));
    const ImGuiID iid = w->GetID(id);
    ImGui::ItemAdd(bb, iid);
    bool hov = false, held = false;
    if (ImGui::ButtonBehavior(bb, iid, &hov, &held)) openPopupAt(id, bb, true);
    const float hv = anim(iid + 1, hov ? 1.f : 0.f, 14.f);  // iid itself animates the popup
    glowCircle(w->DrawList, ctr, r, ImVec4(c->x, c->y, c->z, 1), 0.45f, S(6));
    w->DrawList->AddCircleFilled(ctr, r + hv * S(1), col(ImVec4(c->x, c->y, c->z, 1)), 32);
    const bool changed = false;
    if (beginPopupAt(id, ImVec2(ctr.x - S(30), ctr.y - r), ImVec2(0.f, 0.f), S(392), c)) {
        picker(c);
        endPopupAt();
    }
    return changed;
}

bool colorRow(const char* label, ImVec4* c) {
    Dim dim(label);
    ImRect bb;
    row(label, S(33.7f), bb, nullptr, nullptr, S(30));
    textAt(ImGui::GetWindowDrawList(), fText, ImVec2(bb.Min.x, midY(bb, fText)), col(pal::textOff), visible(label));
    return swatch((std::string(label) + "##sw").c_str(), c);
}

// shared by combo / multi: the list grows out of the field, and shrinks back into it (narrowing as it goes) on close
namespace {
struct Drop { ImRect field; float t = 0.f; bool open = false; };
std::unordered_map<ImGuiID, Drop> g_drops;

void drawDrop(ImDrawList* dl, const ImRect& field, float t, const char* head, const char* const items[], int count,
              std::function<bool(int)> selected, int hovered, bool multiSign, ImGuiID baseId) {
    const float e = ease(t);
    const float itemH = S(20), headH = field.GetHeight();
    const float fullH = headH + itemH * count + S(5);
    ImRect box(field.Min, ImVec2(field.Min.x + ImLerp(field.GetWidth() * 0.55f, field.GetWidth(), e), field.Min.y + ImLerp(headH, fullH, e)));
    const float a = ImSaturate(t * 1.4f);
    dl->AddRectFilled(box.Min, box.Max, col(pal::drop, a), S(4));
    dl->PushClipRect(box.Min, box.Max, true);
    textFade(dl, fText, ImVec2(box.Min.x + S(10), midY(ImRect(field.Min, ImVec2(field.Max.x, field.Min.y + headH)), fText)),
             box.GetWidth() - S(34), pal::textOn, head);
    const ImVec2 sign(box.Max.x - S(13), field.Min.y + headH * 0.5f);
    if (multiSign) plusGlyph(dl, sign, S(4.5f), col(pal::textOn, a), true);
    else chevron(dl, sign, S(4), 1.f, col(pal::textOn, a));
    for (int i = 0; i < count; ++i) {
        const float y = field.Min.y + headH + itemH * i;
        const float hv = anim(baseId + 100 + i, i == hovered ? 1.f : 0.f, 18.f);
        const ImVec4 c = selected(i) ? lerp(accent(), accent2(), hv * 0.5f) : lerp(pal::textOff, pal::textOn, hv);
        textAt(dl, fText, ImVec2(box.Min.x + S(10) + hv * S(6), y + (itemH - fText->FontSize) * 0.5f), col(c, e), items[i]);
    }
    dl->PopClipRect();
}

// returns the clicked item (or -1); keeps the dropdown's open/close animation
int dropdown(const char* label, const ImRect& field, const char* head, const char* const items[], int count,
             std::function<bool(int)> selected, bool multiple) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImGuiID id = w->GetID(label);
    Drop& d = g_drops[id];
    d.field = field;
    const std::string pid = std::string("##drop") + label;
    int clicked = -1;
    const float itemH = S(20);
    const float fullH = field.GetHeight() + itemH * count + S(5);
    ImGui::SetNextWindowPos(field.Min);
    ImGui::SetNextWindowSize(ImVec2(field.GetWidth(), fullH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    const bool open = ImGui::BeginPopup(pid.c_str(), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    d.t = anim(id + 7, open ? 1.f : 0.f, 16.f);
    if (open) {
        ImGuiWindow* pw = ImGui::GetCurrentWindow();
        const ImVec2 m = ImGui::GetIO().MousePos;
        int hovered = -1;
        if (ImGui::IsWindowHovered() && m.y >= field.Min.y + field.GetHeight())
            hovered = (int)((m.y - field.Min.y - field.GetHeight()) / itemH);
        if (hovered >= count) hovered = -1;
        drawDrop(pw->DrawList, field, d.t, head, items, count, selected, hovered, multiple, id);
        if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered()) {
            if (hovered >= 0) clicked = hovered;
            if (!multiple || hovered < 0) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (d.t > 0.01f) {
        drawDrop(ImGui::GetForegroundDrawList(), field, d.t, head, items, count, selected, -1, multiple, id);
    }
    return clicked;
}

void openDrop(const char* label) {
    ImGui::OpenPopup((std::string("##drop") + label).c_str());
}

ImRect fieldRow(const char* label, bool labelAbove, bool& pressed, bool& hov) {
    ImRect bb;
    const float fieldH = S(22);
    pressed = row(label, labelAbove ? S(58) : S(32), bb, &hov);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (labelAbove)
        textAt(dl, fText, ImVec2(bb.Min.x, bb.Min.y + S(4)), col(pal::label), visible(label));
    const float y = labelAbove ? bb.Min.y + S(24) : bb.Min.y + (bb.GetHeight() - fieldH) * 0.5f;
    return ImRect(ImVec2(bb.Min.x, y), ImVec2(bb.Max.x, y + fieldH));
}
}  // namespace

bool combo(const char* label, int* v, const char* const items[], int count, bool labelAbove) {
    const std::string ck = takeKey();
    Dim dim(label);
    bool pressed = false, hov = false;
    const ImRect f = fieldRow(label, labelAbove, pressed, hov);
    if (pressed) openDrop(label);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImGuiID id = w->GetID(label);
    const float hv = anim(id + 3, hov ? 1.f : 0.f, 14.f);
    w->DrawList->AddRectFilled(f.Min, f.Max, col(lerp(pal::field, pal::fieldHover, hv)), S(4));
    const char* cur = (*v >= 0 && *v < count) ? items[*v] : "";
    textAt(w->DrawList, fText, ImVec2(f.Min.x + S(12), midY(f, fText)), col(lerp(pal::value, pal::textOn, hv)), cur);
    chevron(w->DrawList, ImVec2(f.Max.x - S(13), (f.Min.y + f.Max.y) * 0.5f), S(4), g_drops[id].t, col(pal::textOn, 0.85f));
    const int sel = *v;
    bindMenu(label, ck, 2, v, 0.f, 1.f, nullptr, items, count);
    const int clicked = dropdown(label, f, cur, items, count, [sel](int i) { return i == sel; }, false);
    if (clicked >= 0) *v = clicked;
    return clicked >= 0;
}

bool multi(const char* label, unsigned* mask, const char* const items[], int count) {
    Dim dim(label);
    bool pressed = false, hov = false;
    const ImRect f = fieldRow(label, true, pressed, hov);
    if (pressed) openDrop(label);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImGuiID id = w->GetID(label);
    const float hv = anim(id + 3, hov ? 1.f : 0.f, 14.f);
    std::string joined;
    for (int i = 0; i < count; ++i)
        if (*mask & (1u << i)) joined += (joined.empty() ? "" : ", ") + std::string(items[i]);
    if (joined.empty()) joined = "None";
    w->DrawList->AddRectFilled(f.Min, f.Max, col(lerp(pal::field, pal::fieldHover, hv)), S(4));
    textFade(w->DrawList, fText, ImVec2(f.Min.x + S(12), midY(f, fText)), f.GetWidth() - S(36), lerp(pal::value, pal::textOn, hv), joined.c_str());
    plusGlyph(w->DrawList, ImVec2(f.Max.x - S(13), (f.Min.y + f.Max.y) * 0.5f), S(4.5f), col(pal::textOn, 0.85f));
    const unsigned m = *mask;
    const int clicked = dropdown(label, f, joined.c_str(), items, count, [m](int i) { return (m >> i) & 1u; }, true);
    if (clicked >= 0) *mask ^= 1u << clicked;
    return clicked >= 0;
}

namespace {
// slider row: label left, value right, a thin track under both; right-click opens the "Bind" popup
bool sliderRow(const char* label, float frac, const char* valueText, bool lit, float& newFrac) {
    Dim dim(label);
    ImRect bb;
    bool hov = false, held = false;
    row(label, S(36), bb, &hov, &held);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImDrawList* dl = w->DrawList;
    const ImGuiID id = w->GetID(label);
    const float hv = anim(id + 5, hov || held ? 1.f : 0.f, 14.f);
    const ImVec4 lc = lerp(lit ? pal::label : pal::textOff, pal::textOn, hv * 0.5f);
    textAt(dl, fText, ImVec2(bb.Min.x, bb.Min.y + S(3)), col(lc), visible(label));
    const ImVec2 vs = measure(fText, valueText);
    textAt(dl, fText, ImVec2(bb.Max.x - vs.x, bb.Min.y + S(3)), col(lc), valueText);
    const ImRect track(ImVec2(bb.Min.x, bb.Min.y + S(24)), ImVec2(bb.Max.x, bb.Min.y + S(27)));
    const float shown = anim(id + 6, frac, 22.f);
    dl->AddRectFilled(track.Min, track.Max, col(pal::field), S(2));
    if (shown > 0.001f) {
        dl->PushClipRect(track.Min, ImVec2(track.Min.x + track.GetWidth() * shown, track.Max.y), true);
        const ImU32 a = col(accent()), b = col(accent2());
        dl->AddRectFilledMultiColor(track.Min, track.Max, a, b, b, a);
        dl->PopClipRect();
    }
    if (held) {
        newFrac = ImSaturate((ImGui::GetIO().MousePos.x - track.Min.x) / track.GetWidth());
        return true;
    }
    return false;
}

Bind* bindFor(const std::string& ident) {
    for (auto& b : g_binds)
        if ((b.key_.empty() ? b.label : b.key_) == ident) return &b;
    return nullptr;
}

// right-click on the row just drawn -> "Bind": the value to switch to, the key, Hold/Toggle, Create/Cancel (+ Remove)
void bindMenu(const char* label, const std::string& cfgKey, int kind, void* target, float min, float max, const char* fmt,
              const char* const* items, int count) {
    if (!bindMenus || g_inBindMenu || label[0] == '#') return;
    const std::string ident = cfgKey.empty() ? std::string(visible(label)) : cfgKey;
    const std::string pid = "##bind" + std::string(label);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && g_lastRow.Contains(ImGui::GetIO().MousePos) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        openPopupAt(pid.c_str(), ImRect(m, m), false);
    }
    const ImVec2 at = g_popups[ImGui::GetID(pid.c_str())].from.Min;
    if (!beginPopupAt(pid.c_str(), ImVec2(at.x + S(8), at.y + S(8)), ImVec2(0.f, 0.f), S(220))) return;
    g_inBindMenu = true;
    static float alt = 1;
    static bool altB = true;
    static int altI = 0, key = 0, mode = 0;
    Bind* b = bindFor(ident);
    if (ImGui::IsWindowAppearing()) {
        const float cur = kind == 0 ? (*(bool*)target ? 1.f : 0.f) : kind == 1 ? *(float*)target : (float)*(int*)target;
        alt = b ? b->alt : kind == 0 ? 1.f - cur : cur;
        altB = alt > 0.5f, altI = (int)alt, key = b ? b->key : 0, mode = b ? b->mode : 0;
    }
    const ImVec2 ts = measure(fTitle, "Bind");
    ImRect head;
    row("##head", S(28), head);
    textAt(ImGui::GetWindowDrawList(), fTitle, ImVec2((head.Min.x + head.Max.x - ts.x) * 0.5f, midY(head, fTitle)), col(pal::textOn), "Bind");
    if (kind == 0) checkbox("New value", &altB), alt = altB ? 1.f : 0.f;
    else if (kind == 1) slider("New value", &alt, min, max, fmt);
    else combo("New value", &altI, items, count), alt = (float)altI;
    keybind("##key", &key);
    static const char* modes[] = { "Hold", "Toggle" };
    combo("##mode", &mode, modes, 2, false);
    const float half = (ImGui::GetContentRegionAvail().x - S(8)) * 0.5f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    if (button(b ? "Update" : "Create", half)) {
        if (!b) g_binds.push_back(Bind{ visible(label), cfgKey }), b = &g_binds.back();
        b->key = key, b->mode = mode, b->kind = kind, b->target = target, b->alt = alt, b->active = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x + half + S(8), start.y));
    if (button("Cancel", half)) ImGui::CloseCurrentPopup();
    if (b && button("Remove")) {
        for (size_t i = 0; i < g_binds.size(); ++i)
            if (&g_binds[i] == b) {
                if (b->active && b->target) {  // put the menu's value back before it goes
                    if (kind == 0) *(bool*)b->target = b->rest > 0.5f;
                    else if (kind == 1) *(float*)b->target = b->rest;
                    else *(int*)b->target = (int)b->rest;
                }
                g_binds.erase(g_binds.begin() + i);
                break;
            }
        ImGui::CloseCurrentPopup();
    }
    g_inBindMenu = false;
    endPopupAt();
}
}  // namespace

bool slider(const char* label, float* v, float min, float max, const char* fmt, const char* minText, const char* maxText) {
    const std::string ck = takeKey();
    char buf[48];
    if (minText && *v <= min) std::snprintf(buf, sizeof(buf), "%s", minText);
    else if (maxText && *v >= max) std::snprintf(buf, sizeof(buf), "%s", maxText);
    else std::snprintf(buf, sizeof(buf), fmt, *v);
    float f = 0;
    const bool changed = sliderRow(label, (*v - min) / (max - min), buf, true, f);
    if (changed) {
        const char* dot = std::strstr(fmt, ".");
        const float step = dot && dot[1] >= '0' && dot[1] <= '3' ? std::pow(10.f, -(float)(dot[1] - '0')) : 1.f;
        *v = std::round((min + f * (max - min)) / step) * step;
    }
    bindMenu(label, ck, 1, v, min, max, fmt);
    return changed;
}

bool slider(const char* label, float* v, float min, float max, const char* (*text)(float)) {
    const std::string ck = takeKey();
    float f = 0;
    const bool changed = sliderRow(label, (*v - min) / (max - min), text(*v), *v > min, f);
    if (changed) *v = std::round(min + f * (max - min));
    bindMenu(label, ck, 1, v, min, max, "%.0f");
    return changed;
}

bool steps(const char* label, int* v, const char* const names[], int count) {
    float f = 0;
    const bool changed = sliderRow(label, count > 1 ? *v / float(count - 1) : 0.f, names[*v], *v > 0, f);
    if (changed) *v = (int)std::round(f * (count - 1));
    return changed;
}

bool button(const char* label, float width) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImGuiID id = w->GetID(label);
    const ImVec2 p = w->DC.CursorPos;
    const float wd = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const ImRect bb(p + ImVec2(0, S(4)), p + ImVec2(wd, S(28)));
    ImGui::ItemSize(ImRect(p, p + ImVec2(wd, S(38.4f))));
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hov = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
    const float hv = anim(id, hov ? 1.f : 0.f, 14.f);
    const float pr = anim(id + 1, held ? 1.f : 0.f, 24.f);
    w->DrawList->AddRectFilled(bb.Min, bb.Max, col(lerp(lerp(pal::field, pal::fieldHover, hv), accent(), pr * 0.35f)), S(4));
    const char* t = visible(label);
    const ImVec2 ts = measure(fText, t);
    textAt(w->DrawList, fText, ImVec2((bb.Min.x + bb.Max.x - ts.x) * 0.5f, midY(bb, fText)), col(lerp(pal::label, pal::textOn, hv)), t);
    return pressed;
}

const char* keyName(int vk) {
    static char buf[32];
    switch (vk) {
        case 0: return "None";
        case VK_LBUTTON: return "Mouse 1";
        case VK_RBUTTON: return "Mouse 2";
        case VK_MBUTTON: return "Mouse 3";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "Page up";
        case VK_NEXT: return "Page down";
        case VK_SHIFT: case VK_LSHIFT: return "Shift";
        case VK_CONTROL: case VK_LCONTROL: return "Ctrl";
        case VK_MENU: case VK_LMENU: return "Alt";
        case VK_SPACE: return "Space";
        case VK_CAPITAL: return "Caps lock";
        case VK_TAB: return "Tab";
    }
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    if (GetKeyNameTextA((LONG)(sc << 16), buf, sizeof(buf)) > 0) return buf;
    std::snprintf(buf, sizeof(buf), "Key %d", vk);
    return buf;
}

bool keybind(const char* label, int* key) {
    Dim dim(label);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImGuiID id = w->GetID(label);
    ImRect bb;
    row(label, S(34), bb, nullptr, nullptr, 1e5f);
    const bool hasLabel = label[0] != '#';
    if (hasLabel) textAt(w->DrawList, fText, ImVec2(bb.Min.x, midY(bb, fText)), col(pal::label), visible(label));
    const ImRect btn(ImVec2(hasLabel ? bb.Min.x + bb.GetWidth() * 0.5f : bb.Min.x, bb.Min.y + S(6)), ImVec2(bb.Max.x, bb.Max.y - S(6)));
    ImGui::ItemAdd(btn, id + 1);
    bool hov = false, held = false;
    if (ImGui::ButtonBehavior(btn, id + 1, &hov, &held) && g_waitKey != id) {
        g_waitKey = id;
        for (int k = 0; k < 256; ++k) g_keysAtWait[k] = keyDown(k);
    }
    bool changed = false;
    if (g_waitKey == id) {
        for (int k = 1; k < 256; ++k) {
            const bool down = keyDown(k);
            if (down && !g_keysAtWait[k]) {
                *key = k == VK_ESCAPE ? 0 : k, g_waitKey = 0, changed = true;
                break;
            }
            if (!down) g_keysAtWait[k] = false;
        }
    }
    const float hv = anim(id + 2, hov || g_waitKey == id ? 1.f : 0.f, 14.f);
    w->DrawList->AddRectFilled(btn.Min, btn.Max, col(lerp(pal::field, pal::fieldHover, hv)), S(4));
    const char* t = g_waitKey == id ? "..." : keyName(*key);
    const ImVec2 ts = measure(fText, t);
    textAt(w->DrawList, fText, ImVec2((btn.Min.x + btn.Max.x - ts.x) * 0.5f, midY(btn, fText)), col(pal::label), t);
    return changed;
}

bool input(const char* label, char* buf, size_t size) {
    Dim dim(label);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImRect bb;
    row(label, S(38.4f), bb, nullptr, nullptr, 1e5f);
    textAt(w->DrawList, fText, ImVec2(bb.Min.x, midY(bb, fText)), col(pal::label), visible(label));
    const float x = bb.Min.x + bb.GetWidth() * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(x, bb.Min.y + S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), (bb.GetHeight() - S(12) - fText->FontSize) * 0.5f));
    ImGui::SetNextItemWidth(bb.Max.x - x);
    const bool changed = ImGui::InputText((std::string("##in") + label).c_str(), buf, size);
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(bb.Min.x, bb.Max.y));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

bool listbox(const char* id, int* selected, const std::vector<std::string>& items, float height, char* filter) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImVec2 p = w->DC.CursorPos;
    const float wd = ImGui::GetContentRegionAvail().x;
    const float h = height > 0 ? S(height) : ImGui::GetContentRegionAvail().y - S(4);
    w->DrawList->AddRectFilled(p, p + ImVec2(wd, h), col(pal::field), S(4));
    bool changed = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(0), S(6)));
    ImGui::BeginChild(id, ImVec2(wd, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    if (filter) {  // the search line types into the top of the list
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(3)));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, pal::fieldHover);
        ImGui::SetNextItemWidth(wd);
        ImGui::InputTextWithHint("##filter", "", filter, 64);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    for (int i = 0; i < (int)items.size(); ++i) {
        if (filter && *filter) {
            std::string a = items[i], b = filter;
            for (auto& c : a) c = (char)tolower((unsigned char)c);
            for (auto& c : b) c = (char)tolower((unsigned char)c);
            if (a.find(b) == std::string::npos) continue;
        }
        ImGui::PushID(i);
        ImRect bb;
        bool hov = false;
        if (row("##it", S(24), bb, &hov)) *selected = i, changed = true;
        const float hv = anim(ImGui::GetID("##it") + 9, hov ? 1.f : 0.f, 18.f);
        const ImVec4 c = *selected == i ? accent() : lerp(pal::label, pal::textOn, hv);
        textAt(ImGui::GetWindowDrawList(), fText, ImVec2(bb.Min.x + S(12) + hv * S(3), midY(bb, fText)), col(c), items[i].c_str());
        ImGui::PopID();
    }
    const float maxScroll = ImGui::GetScrollMaxY();
    if (maxScroll > 0 && ImGui::GetScrollY() < maxScroll - 1.f) {  // more below: the last row fades into the list
        ImGuiWindow* cw = ImGui::GetCurrentWindow();
        const float y1 = cw->Pos.y + cw->Size.y, y0 = y1 - S(30);
        const ImU32 clear = col(ImVec4(pal::field.x, pal::field.y, pal::field.z, 0.f)), solid = col(pal::field);
        cw->DrawList->AddRectFilledMultiColor(ImVec2(cw->Pos.x, y0), ImVec2(cw->Pos.x + cw->Size.x - S(9), y1), clear, clear, solid, solid);
    }
    if (maxScroll > 0) {
        ImGuiWindow* cw = ImGui::GetCurrentWindow();
        const ImRect r(cw->Pos, cw->Pos + cw->Size);
        const float trackH = r.GetHeight() - S(16);
        const float thumb = std::max(S(24), trackH * r.GetHeight() / (r.GetHeight() + maxScroll));
        const float y = r.Min.y + S(8) + (trackH - thumb) * (ImGui::GetScrollY() / maxScroll);
        cw->DrawList->AddRectFilled(ImVec2(r.Max.x - S(7), y), ImVec2(r.Max.x - S(4), y + thumb), col(accent()), S(2));
    }
    ImGui::EndChild();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + S(8)));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

// ---------------------------------------------------------------- binds

void bindRow(const char* label, const char* cfgKey) {
    Bind* b = bindFor(cfgKey);
    int key = b ? b->key : 0, mode = b ? b->mode : 0;
    const bool k = keybind("Key", &key);
    static const char* modes[] = { "Hold", "Toggle" };
    const bool m = combo("##mode", &mode, modes, 2, false);
    if ((k || m) && !b) g_binds.push_back(Bind{ label, cfgKey }), b = &g_binds.back();
    if (b) b->key = key, b->mode = mode, b->kind = 0, b->target = &cfg::B(cfgKey), b->alt = 1.f;
}

void addBind(const Bind& nb) {
    Bind* b = bindFor(nb.key_.empty() ? nb.label : nb.key_);
    if (b) *b = nb;
    else g_binds.push_back(nb);
}

bool (*keyDown)(int vk) = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
void cfgKey(const char* key) { g_nextKey = key; }
bool capturingKey() { return g_waitKey != 0; }

void pollBinds() {
    static std::unordered_map<std::string, bool> prev;
    const bool typing = ImGui::GetIO().WantTextInput;
    auto get = [](const Bind& b) { return b.kind == 0 ? (*(bool*)b.target ? 1.f : 0.f) : b.kind == 1 ? *(float*)b.target : (float)*(int*)b.target; };
    auto set = [](const Bind& b, float v) {
        if (b.kind == 0) *(bool*)b.target = v > 0.5f;
        else if (b.kind == 1) *(float*)b.target = v;
        else *(int*)b.target = (int)v;
    };
    for (auto& b : g_binds) {
        const std::string id = b.key_.empty() ? b.label : b.key_;
        if (!b.target || g_waitKey || typing) continue;
        const bool down = b.key && keyDown(b.key);
        const bool want = b.mode == 0 ? down : (down && !prev[id] ? !b.active : b.active);
        prev[id] = down;
        if (want == b.active) continue;
        if (want) b.rest = get(b), set(b, b.alt);  // the menu's own value comes back when the bind lets go
        else set(b, b.rest);
        b.active = want;
    }
}

const std::vector<Bind>& binds() { return g_binds; }

}  // namespace ui

namespace ui {
namespace {
std::unordered_map<ImGuiID, ImRect> g_popupFrom;
}
void openPopup(const char* id) {
    g_popupFrom[ImGui::GetID(id)] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    openPopupAt(id, g_popupFrom[ImGui::GetID(id)], false);
}
bool beginPopup(const char* id, float width) {
    const ImRect r = g_popupFrom[ImGui::GetID(id)];
    return beginPopupAt(id, ImVec2((r.Min.x + r.Max.x) * 0.5f, r.Max.y + S(6)), ImVec2(0.5f, 0.f), S(width));
}
void endPopup() { endPopupAt(); }
}  // namespace ui
