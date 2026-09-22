// What the RPCS3 wiki says about a title, as this core shipped it: the table is
// generated from wiki-compat.json (tools/wiki-import.py) by gen-wiki-compat.py
// at build time, so a project's machine is versioned with the core that reads
// it and never fetched at run time. Provenance and licence: wiki-compat.json.
#pragma once
#include <cstddef>

struct chimera_wiki_entry
{
    const char* id;          // PS3 title id, e.g. BLUS30000 - the table is sorted by it
    const char* title;       // the wiki page's title
    const char* status;      // compatibility: Playable, Ingame, Intro, Loadable, Nothing
    char kind;               // 's' page with recommendations, 'n' page with none,
                             // 'p' in the compatibility list but no wiki page,
                             // 'u' a wiki page exists but this snapshot did not read it
    const char* page;        // the wiki page, for the reader to check
    const char* apply;       // recommendations this core can express, "name=value, ..."
    const char* unsupported; // recommendations it cannot, "Wiki name: value, ..."
};

extern const chimera_wiki_entry chimera_wiki[];
extern const size_t chimera_wiki_count;
extern const char* const chimera_wiki_snapshot;         // when the wiki was read
extern const char* const chimera_wiki_compat_snapshot;  // when the compatibility list was

// nullptr when the title is not in the snapshot
const chimera_wiki_entry* chimera_wiki_find(const char* title_id);
