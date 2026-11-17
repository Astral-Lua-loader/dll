// The menu: controls declared by the Lua (a text schema), their values, keybinds and the config file, drawn with ImGui.
// Everything but the getters runs on the render thread (Present).
#pragma once
#include <string>

namespace alm::menu {

// One control per line, tab separated (ui/menu.lua writes it):
//   page   <name>
//   group  <name> <col 0|1>
//   check  <slot> <name> <default 0|1> <bindable 0|1>
//   slider <slot> <name> <default> <min> <max> <step> <suffix>
//   combo  <slot> <name> <default index> <opt1|opt2|...>
void SetSchema(const std::string& text);  // any thread; applied next frame, saved values loaded into it
double Get(int slot);                     // any thread
void SetState(bool open, int r, int g, int b);  // any thread: Astral's menu open, its accent (-1 unknown)

void SetConfigPath(const std::wstring& path);
void Fonts(float scale);  // after ImGui::CreateContext, before the backend builds the atlas
void Frame();             // between NewFrame and Render
bool WantsMouse();        // the cursor is over the menu (the window keeps those messages from the game)
void Save();              // now, if anything changed
bool RowAt(int slot, float* x, float* y);  // the centre of that control's row, last frame (the preview clicks by it)
bool TabAt(int page, float* x, float* y);

}  // namespace alm::menu
