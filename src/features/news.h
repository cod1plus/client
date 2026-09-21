#ifndef COD1RELOADED_NEWS_H
#define COD1RELOADED_NEWS_H
// Main-menu announcement card ("MAJOR IN PROGRESS ..."), driven by a text file online
// so it can be changed without a release - same trust root as the rulesets:
//   https://raw.githubusercontent.com/cod1plus/rulesets/main/news.txt
// Format, first three non-empty non-comment lines:
//     line 1  title      (short, drawn big)          MAJOR IN PROGRESS
//     line 2  text       (one line)                   CoDBase Major Cup #18 - vCoD 1.6
//     line 3  url        (optional)                   https://fpschallenge.eu/...
// An empty file (or comments only) hides the card. Fetched at start-up and every 10
// minutes. The card is drawn by the GL overlay while no game is loaded (main menu,
// connect screen); Ctrl+N opens the url in the browser.
#include <windows.h>
#include <cstddef>

namespace patches {

struct NewsConfig {
    bool enable;
    char url[256];      // ini: news_url
};
extern NewsConfig g_news_config;

void news_start();                                   // DllMain: fetch thread
// Snapshot for the drawer (any thread). Returns false when there is nothing to show.
bool news_get(char* title, size_t tn, char* text, size_t xn, char* url, size_t un);
void news_open_link();                               // ShellExecute the url, if any

}  // namespace patches

#endif
