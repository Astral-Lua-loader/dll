// What astralmenu's ui.cpp reads from its config, for the loader: the accent (from Astral) and a bool store.
#pragma once
#include <imgui.h>

#include <map>
#include <string>

namespace cfg {
inline ImVec4 accent{125 / 255.f, 168 / 255.f, 255 / 255.f, 1.f};
inline int renderScale = 1;
inline bool& B(const char* key, bool def = false) {
    static std::map<std::string, bool> store;
    return store.try_emplace(key, def).first->second;
}
}  // namespace cfg
