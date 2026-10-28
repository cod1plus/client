#ifndef COD1RELOADED_WORKSHOP_H
#define COD1RELOADED_WORKSHOP_H

// The Workshop tab of the 1.6X menu (ui/workshop_tab.cpp): browse maps and mods from a
// catalog, download one, get it verified and dropped where the game reads it - the Steam
// Workshop idea, for pk3s.
//
// THE CATALOG is one JSON file (ini `workshop_manifest_url`, http(s) or file://; default =
// workshop.json of the cod1plus/workshop repository, pk3s as its release assets):
//     { "title": "1.6X Workshop",
//       "items": [ { "id": "mp_liberation", "type": "map", "name": "Liberation",
//                    "author": "...", "version": "26", "description": "...",
//                    "folder": "main", "file": "zz_mp_liberation.pk3",
//                    "size": 191876653, "sha256": "<64 hex>",
//                    "url": "https://.../zz_mp_liberation.pk3",
//                    "thumbnail": "liberation.jpg" } ] }
// url / thumbnail may be relative to the catalog's own address. folder defaults to main.
// A pack of several pk3s (the HD textures) lists them instead of file/size/sha256/url:
//     "files": [ { "file": "zcod1.pk3", "size": ..., "sha256": "...", "url": "..." }, ... ],
//     "cvars": { "com_hunkMegs": "512" }       (seta'd on the main thread once installed)
// tools/workshop/make_catalog.py writes it from the pk3s (size + sha256 computed).
// A file:// link inside a catalog is only followed when the catalog itself is a local
// file (a test catalog, a LAN share): an online catalog cannot make the client copy
// files off the player's disk.
//
// INSTALL = the PAM package downloader's transfer (features/pam_install.cpp): to
// <game>\<folder>\<file>.part (resumed if left over), size capped by the catalog, SHA-256
// on the fly, renamed over the pk3 only when the hash matches. A pk3 the engine holds
// open lands as <file>.new and is swapped in at the next launch (pam_install_swap_pending).
//
// STATE of an item = what is on disk, by hash: absent, installed (same SHA-256), update
// available (a different file under that name), staged (.new waiting for a restart).
// The version we installed is noted in <game>\cod1reloaded\workshop\installed.txt so an
// update can say "v25 -> v26". The thumbnails and the last good catalog (shown offline)
// are cached next to it.
//
// HONEST LIMIT: CoD1 lists its pk3s when its filesystem starts (launch, or a server
// connect that restarts it); a pk3 added while the game runs is there on disk but the
// engine sees it after a restart. The tab says so.

#include <string>
#include <vector>

namespace patches {

struct WorkshopConfig {
    bool enable;
    char manifest_url[256];   // ini: workshop_enable / workshop_manifest_url
};
extern WorkshopConfig g_workshop_config;

enum WsLocal { WS_CHECKING = 0, WS_ABSENT, WS_INSTALLED, WS_OUTDATED, WS_STAGED };
enum WsJob   { WSJ_NONE = 0, WSJ_QUEUED, WSJ_DOWNLOADING, WSJ_VERIFYING, WSJ_DONE, WSJ_ERROR };

struct WsFile {
    std::string file, sha256, url;
    unsigned long long size = 0;
};

struct WsItem {
    std::string id, type, name, author, version, description;
    std::string folder, thumb_url;
    std::vector<WsFile> files;    // one pk3 for a map, several for a pack
    std::vector<std::pair<std::string, std::string>> cvars;   // seta'd once installed
    unsigned long long size = 0;  // all files
    std::string thumb_path;       // cached copy (UTF-8 path), "" until fetched
    int  local = WS_CHECKING;
    std::string local_version;    // the version our last install of the file on disk had, "" = unknown
    int  job = WSJ_NONE;
    float progress = 0;           // 0..1 while downloading
    std::string job_text;         // "12.3 / 183.0 MB  8.1 MB/s", the error, ...
    bool installed_now = false;   // placed during this session: the engine lists it after a restart
};

enum WsCatalog { WSC_NONE = 0, WSC_DISABLED, WSC_LOADING, WSC_READY, WSC_CACHED, WSC_ERROR };
struct WsCatalogInfo {
    int state = WSC_NONE;
    std::string title;            // the catalog's "title"
    std::string text;             // one line: where it came from, or what went wrong
    bool local = false;           // the catalog is a local file
    bool busy = false;            // a load (catalog, states, pictures) is still running
};

void workshop_refresh();                               // (re)load the catalog, in the background
void workshop_snapshot(WsCatalogInfo& info, std::vector<WsItem>& items);   // for the menu (any thread)
void workshop_install(const std::string& id);          // queue a download; no-op if queued / running
bool workshop_busy();                                  // a download is queued or running
// The cvars of the items installed since the last call, handed out once ("seta <name>
// <value>" lines for the main thread: features/pkg_frame.cpp applies them every frame).
std::vector<std::pair<std::string, std::string>> workshop_take_cvars();

}  // namespace patches

#endif
