// The menu: controls declared by the Lua (a text schema), their values, keybinds and the config file, drawn with ImGui.
// Everything but the getters runs on the render thread (Present).
#pragma once
#include <string>

struct ID3D11Device;

namespace alm::menu {

// One control per line, tab separated (ui/menu.lua writes it):
//   page   <name>
//   group  <name> <col 0|1>
//   check  <slot> <name> <default 0|1> <bindable 0|1>
//   slider <slot> <name> <default> <min> <max> <step> <suffix>
//   combo  <slot> <name> <default index> <opt1|opt2|...>
//   list   <slot> <name> <height px> <title^author|...>   (slot = selected, slot+1+i = item i: 0 off, 1 on, 2 failed)
//   info   <slot> <name> <list slot>                       (the list's selected item)
void SetSchema(const std::string& text);  // any thread; applied next frame, saved values loaded into it
double Get(int slot);                     // any thread
void SetUser(const std::string& name);    // any thread: the footer's username
void SetDevice(ID3D11Device* device);     // render thread, once: starts the footer's Steam avatar
void ReleaseTextures();                   // shutdown: the header swirl texture
void SetValue(int slot, double v);         // any thread (a list item's state from the Lua: 2 = failed)
void SetState(bool open, int r, int g, int b);  // any thread: Astral's menu open, its accent (-1 unknown)

void SetConfigPath(const std::wstring& path);
float Scale();            // the size to draw at: 600/720 x Astral's render scale (alm_scale)
void SetRenderScale(float s);  // any thread, before alm_init: Astral's "Render scale" (1 = 100%)
void Fonts(float scale);  // after ImGui::CreateContext, before the backend builds the atlas
void Frame();             // between NewFrame and Render
bool WantsMouse();        // the cursor is over the menu (the window keeps those messages from the game)
void Save();              // now, if anything changed
bool RowAt(int slot, float* x, float* y);  // the centre of that control's row, last frame (the preview clicks by it)
bool TabAt(int page, float* x, float* y);
bool RowBox(int slot, float* x0, float* y0);  // that row's top left

}  // namespace alm::menu
