// The GitHub and Discord marks of the start screen's links (ui/launcher/launcher_brand.cpp). ImGui's thread.
#pragma once
#include <SDL3/SDL.h>

#include "imgui.h"

enum Brand { BRAND_GITHUB, BRAND_DISCORD, BRAND_COUNT };

// The mark of b, white on clear, px pixels square, as a texture of ren: drawn with a tint (ImDrawList::AddImage's col)
// it takes the link's colour. Made once per size; nullptr when it could not be made.
ImTextureID launcher_brand_icon(SDL_Renderer *ren, Brand b, int px);
// The textures made, before ren goes.
void launcher_brand_free(void);
