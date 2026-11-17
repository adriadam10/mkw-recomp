// The start screen's logo and its motion (ui/launcher/launcher_logo.cpp): the port's logo drawn with ImGui, the main
// page's intro and the fade of a page coming in. All of it only looks: input works from the first frame. ImGui's thread.
#pragma once
#include "imgui.h"

// The port's logo (tools/build/icon.py, docs/images/logo.svg) at size px with its top left at p.
void launcher_logo(ImDrawList *dl, ImVec2 p, float size);

// The ease-out cube of progress p in 0..1: quick start, soft landing.
float launcher_ease_out(float p);
// Element i of the main page's intro, 0..1: each element 0.08 s after the one before and 0.35 s long, from t0 (the
// ImGui time the page was first shown). t0 <= 0 (a shot, or no intro): 1.
float launcher_intro(double t0, int i);
// The vertices dl got since start (dl->VtxBuffer.Size before drawing an element) faded to k and lowered by the rest of
// the intro's rise (12 px at 1080 lines, times scale).
void launcher_intro_apply(ImDrawList *dl, int start, float k, float scale);
// A cover of colour c over the whole w x h window, from opaque at t0 to clear secs later (eased): a page or a picture
// coming in. false once it is clear.
bool launcher_fade(ImDrawList *dl, float w, float h, ImU32 c, double t0, float secs);
