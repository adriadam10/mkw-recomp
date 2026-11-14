// The start screen's Online page (launcher_online.cpp): this player's name and friend code, how they play (off, hosting
// the built-in server, joining a friend's), friend requests and friends, LAN play through Tailscale.
#pragma once

void online_opened(void);                        // the page was opened: fetch the server's lists again
void online_page(float scale, float bottom);     // draws the page inside the launcher's panel
void online_host_boot(void);                      // the start screen opens: this PC's server, when hosting is on
