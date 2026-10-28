# Workshop catalog (Ctrl+M > Workshop)

The client lists what one JSON catalog describes (format: `src/features/workshop.h`) and
installs each pk3 after checking its SHA-256. Point a client at a catalog with
`workshop_manifest_url` in `cod1reloaded.ini` (http, https or `file:///C:/...`).

Add or update an item (size and SHA-256 are computed from the pk3):

    python tools/workshop/make_catalog.py <folder>/workshop.json --pk3 <folder>/zz_mp_liberation.pk3 \
        --id mp_liberation --name Liberation --type map --version 27 --author "..." \
        --description "..." --thumbnail liberation.jpg

Links (`url`, `thumbnail`) may be relative to the catalog: a folder holding `workshop.json`,
the pictures and the pk3s is a complete catalog, by file:// or served over http.

`sample/` = the first catalog: Liberation (tool27) with its thumbnail (a crop of an in-game
capture, 640x360). Put the pk3 next to it to use it as is.

Checks:

    sh tools/test_workshop.sh <file.pk3> <thumbnail.jpg>      # backend, file:// and http
    mingw32-make -C build-rel workshop_preview                 # offline render of the tab
