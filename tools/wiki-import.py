#!/usr/bin/env python3
"""The RPCS3 wiki harvest, turned into the table this core ships.

usage: wiki-import.py <rpcs3-wiki-settings.json> [--out waterbox/wiki-compat.json]

Input is what tools/wiki-harvest.js downloads from a browser session. Output is
a table keyed by PS3 title id - a page covers every regional id the
compatibility API lists for it - carrying, for each id, what the wiki says and
what this core can do with it. It is committed, so the machine a project gets is
versioned with the core that interprets the table, never fetched at run time.

PROVENANCE AND LICENCE travel with the data. The wiki is CC BY-SA 4.0; the
harvest keeps only setting names and option values - facts about a game - and
drops the Notes column, which is somebody's prose. The table still names its
source, its snapshot date and the licence, and every entry links its page.

A setting name that tools/wiki-settings-map.json does not list is an ERROR and
nothing is written: the wiki must not be able to grow a recommendation this core
silently ignores. A known name whose VALUE the map cannot translate is not an
error - it is recorded, per game, as unsupported, and never rounded to the
nearest option the core happens to have.
"""
import argparse
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("harvest", nargs="+",
                    help="one or more harvests; later files add pages, and fill in any the earlier ones lacked")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "waterbox", "wiki-compat.json"))
    ap.add_argument("--map", default=os.path.join(HERE, "wiki-settings-map.json"))
    ap.add_argument("--ids", default=os.path.join(HERE, "wiki-ids.json"),
                    help="every compatibility-list title id and the wiki page it links to "
                         "(tools/wiki-resolve-ids.py)")
    ap.add_argument("--compat-date", default=None,
                    help="when the compatibility list was fetched (defaults to the harvest date)")
    a = ap.parse_args()

    harvests = [json.load(open(h, encoding="utf-8")) for h in a.harvest]
    harvest = harvests[0]
    names = json.load(open(a.map, encoding="utf-8"))
    pages = {}
    for h in harvests:
        for k, v in h["pages"].items():
            pages.setdefault(k, v)
    # the snapshot is the NEWEST harvest's date: the table is as current as that
    harvest = max(harvests, key=lambda h: h.get("harvested") or "")

    unknown = sorted({s["setting"] for v in pages.values() for s in (v.get("settings") or [])}
                     - set(k for k in names if not k.startswith("_")))
    if unknown:
        print("REFUSED: setting names the map does not list (add them to "
              "wiki-settings-map.json, mapped or with a reason):", file=sys.stderr)
        for n in unknown:
            print("   " + n, file=sys.stderr)
        return 1

    snapshot = (harvest.get("harvested") or "")[:10]
    compat_date = a.compat_date or snapshot
    resolved = json.load(open(a.ids, encoding="utf-8"))

    # one entry per harvested wiki PAGE, keyed by its curid
    by_page = {}
    for key, v in pages.items():
        curid = int(key) if str(key).isdigit() else None
        page = (f"https://wiki.rpcs3.net/index.php?curid={curid}" if curid
                else "https://wiki.rpcs3.net/index.php?title=" + v["title"].replace(" ", "_"))
        if v.get("noPage"):
            kind = "nopage"
        elif v.get("settings"):
            kind = "settings"
        else:
            kind = "none"
        apply, unsupported = collections.OrderedDict(), []
        for st in v.get("settings") or []:
            m = names[st["setting"]]
            if m.get("setting") is not None and st["option"] in m["values"]:
                apply[m["setting"]] = m["values"][st["option"]]
            else:
                unsupported.append(f'{st["setting"]}: {st["option"]}')
        by_page[curid if curid is not None else key] = (v["title"], kind, page, apply, unsupported)

    # every title id the compatibility list knows - its STATUS comes from there,
    # its settings from whichever wiki page it links to
    titles = {}
    counts = collections.Counter()
    unharvested = 0
    for tid, r in sorted(resolved.items()):
        wid = r.get("wiki_id")
        hit = by_page.get(wid) if wid is not None else None
        if hit is None:
            # the list knows the title; no article is linked to it, or the
            # linked article was not in the harvest. Either way: status only.
            if wid is not None:
                # the article EXISTS; it simply was not read in this snapshot.
                # Saying "the wiki has no page" here would be false.
                unharvested += 1
                title, kind, page, apply, unsupported = (
                    (r.get("title") or tid), "unread",
                    f"https://wiki.rpcs3.net/index.php?curid={wid}", {}, [])
            else:
                title, kind, page, apply, unsupported = (r.get("title") or tid), "nopage", "", {}, []
        else:
            title, kind, page, apply, unsupported = hit
        titles[tid] = collections.OrderedDict([
            ("title", title), ("status", r.get("status")), ("kind", kind),
            ("page", page),
            ("apply", apply), ("unsupported", unsupported)])
        counts[kind] += 1
    clashes = []

    out = collections.OrderedDict()
    out["_provenance"] = collections.OrderedDict([
        ("source", "https://wiki.rpcs3.net/ - the per-game Configuration tables"),
        ("compatibility", "https://rpcs3.net/compatibility - the status of each title"),
        ("snapshot", snapshot),
        ("compatibility_snapshot", compat_date),
        ("licence", "The RPCS3 wiki's content is licensed CC BY-SA 4.0 "
                    "(https://creativecommons.org/licenses/by-sa/4.0/)."),
        ("attribution", "Recommended settings compiled by the contributors of the RPCS3 wiki."),
        ("kept", "Setting names and option values only, which are facts about a game. "
                 "The Notes column, which is contributors' prose, is not kept."),
        ("harvested_by", "tools/wiki-harvest.js, run in a browser session; imported by "
                         "tools/wiki-import.py against tools/wiki-settings-map.json"),
    ])
    out["titles"] = collections.OrderedDict(sorted(titles.items()))
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1, ensure_ascii=True)
        f.write("\n")

    applicable = sum(1 for e in titles.values() if e["apply"])
    print(f"{len(pages)} wiki pages, {len(titles)} title ids (wiki {snapshot}, compatibility "
          f"{compat_date}): {counts['settings']} with recommendations ({applicable} with at "
          f"least one this core can apply), {counts['none']} with none, {counts['unread']} "
          f"whose page exists but was not read, {counts['nopage']} with no wiki page")
    if clashes:
        print(f"{len(clashes)} title id(s) claimed by two pages; the first was kept:")
        for tid, first, second in clashes[:10]:
            print(f"   {tid}: {first!r} over {second!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
