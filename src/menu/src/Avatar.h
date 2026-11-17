// The footer's profile picture: the logged-in Steam account's avatar, fetched in the background (Steam's public profile
// page -> avatar image -> WIC decode) and turned into a texture on the render thread.
#pragma once
#include <d3d11.h>

#include <imgui.h>

namespace alm::avatar {

void Start();                              // once: starts the background fetch
ImTextureID Texture(ID3D11Device* device); // render thread: the texture once it's ready, else nullptr
void Stop();                               // joins the fetch, frees the texture

}  // namespace alm::avatar
