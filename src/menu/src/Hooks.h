// Present (the menu), ResizeBuffers and the game window's WndProc (ImGui's input). From cs2shaders' Hooks.cpp.
#pragma once

namespace alm::hooks {

bool Initialize();  // false: no menu
void Shutdown();    // synchronous: hooks off, window restored, ImGui gone

}  // namespace alm::hooks
