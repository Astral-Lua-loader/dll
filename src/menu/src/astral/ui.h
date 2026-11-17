// Astral's widget set on Dear ImGui: grouped rows, "+" settings popups, colour swatches with the morphing picker,
// collapsing dropdowns, thin sliders. Sizes are 1080p pixels from the showcase videos, times the render scale.
#pragma once
#include <imgui.h>

#include <string>
#include <vector>

namespace ui {

// fonts, rebuilt by setup() whenever the render scale changes
extern ImFont *fText, *fTitle, *fHeader, *fIcons, *fSmall;
void setup(float scale);
bool fontsDirty();  // the render scale changed: the caller rebuilds the atlas (setup) and the backend's font texture
float S(float px);
float scale();

// colours: accent comes from the config, accent2 is it turned 30 degrees round the hue wheel (blue -> violet, red -> orange)
ImU32 col(const ImVec4& c, float alpha = 1.f);
ImVec4 accent();
ImVec4 accent2();
ImVec4 rgb(int r, int g, int b, int a = 255);
namespace pal {
inline const ImVec4 bg{ 35 / 255.f, 40 / 255.f, 55 / 255.f, 1 }, group{ 44 / 255.f, 49 / 255.f, 64 / 255.f, 1 },
    field{ 52 / 255.f, 57 / 255.f, 72 / 255.f, 1 }, fieldHover{ 61 / 255.f, 66 / 255.f, 83 / 255.f, 1 },
    drop{ 58 / 255.f, 63 / 255.f, 80 / 255.f, 1 }, textOn{ 235 / 255.f, 237 / 255.f, 243 / 255.f, 1 },
    textOff{ 160 / 255.f, 165 / 255.f, 180 / 255.f, 1 }, textDim{ 108 / 255.f, 113 / 255.f, 128 / 255.f, 1 },
    label{ 196 / 255.f, 200 / 255.f, 212 / 255.f, 1 }, value{ 168 / 255.f, 173 / 255.f, 186 / 255.f, 1 };  // dropdown values are grey till hovered
}

float anim(ImGuiID id, float target, float speed = 14.f);  // eases towards target, per id
float anim(const char* id, float target, float speed = 14.f);
float ease(float t);                                         // cubic out

// the footer search: rows whose label doesn't match are dimmed
std::string& search();

// layout: a page is split into columns; groups stack down a column
void page(ImVec2 min, ImVec2 max);  // the page area (screen space) the columns and groups fit in
void column(int index, int of = 2, float top = 0.f);
void beginGroup(const char* title, float height = 0.f);  // 0: fit the content, capped at the page bottom
void endGroup();
float lastGroupWant();  // the last beginGroup's full height before the page bottom capped it
inline bool bindMenus = true;  // false: no right-click Bind popups (the host binds keys itself)
void gap(float px);
void tip(const char* text);

// a popup under the last item (a button): openPopup on click, then if (beginPopup) { ...; endPopup(); }
void openPopup(const char* id);
bool beginPopup(const char* id, float width = 200.f);
void endPopup();  // description shown when the row above is hovered for a moment

// rows
bool checkbox(const char* label, bool* v);
bool settings(const char* id, float width = 200.f, bool popup = true);  // "+" on the row above; true while its popup is open
// (popup false: the button alone, for features whose settings the videos never show)
void endSettings();
bool swatch(const char* id, ImVec4* c);               // colour circle on the row above, opens the picker
bool swatchAt(const char* id, ImVec2 rowMin, ImVec2 rowMax, ImVec4* c);  // same, at the right end of that row
bool colorRow(const char* label, ImVec4* c);          // label + swatch
bool combo(const char* label, int* v, const char* const items[], int count, bool labelAbove = true);
bool multi(const char* label, unsigned* mask, const char* const items[], int count);
bool slider(const char* label, float* v, float min, float max, const char* fmt, const char* minText = nullptr,
            const char* maxText = nullptr);
bool slider(const char* label, float* v, float min, float max, const char* (*text)(float));  // value shown via text()
bool steps(const char* label, int* v, const char* const names[], int count);  // a slider over named stops ("2D", "3D")
bool button(const char* label, float width = 0.f);
bool keybind(const char* label, int* key);
bool input(const char* label, char* buf, size_t size);
bool listbox(const char* id, int* selected, const std::vector<std::string>& items, float height, char* filter = nullptr);
void text(const char* s, const ImVec4& c);
const char* keyName(int vk);

// key binds, Astral style: right-click a checkbox / dropdown / slider -> "Bind" popup (new value, key, Hold/Toggle);
// checkbox "+" popups carry the same key row. Hold: the new value while the key is down. Toggle: flips per press.
struct Bind {
    std::string label, key_;  // shown name; identity = config key of the row (empty: the label)
    int key = 0, mode = 0;    // mode: 0 hold, 1 toggle
    int kind = 0;             // what target points at: 0 bool, 1 float, 2 int
    void* target = nullptr;
    float alt = 1.f, rest = 0.f;
    bool active = false;
};
extern bool (*keyDown)(int vk);  // GetAsyncKeyState by default; the offline preview swaps in its own
void cfgKey(const char* key);    // the config key of the next row: its bind is then saved with configs
bool capturingKey();             // a keybind button is waiting for a key
void bindRow(const char* label, const char* key);  // inside a settings popup: the key button + Hold/Toggle for cfg::B(key)
void addBind(const Bind& b);
void pollBinds();                // once a frame, outside the menu too
const std::vector<Bind>& binds();

}  // namespace ui
