#!/usr/bin/env python3
"""Per-game recommended settings, out of the RPCS3 wiki and into a table.

WHY THIS IS A TOOL AND NOT A RUNTIME FETCH
------------------------------------------
A setting that changes the machine has to be pinned by the project: that is the
whole reproduction contract. If a value arrived over the network at project
creation time, two people who created a project for the same disc a month apart
would get different machines and neither would be told why. So this runs at
release time, by hand, and its OUTPUT is committed - a table shipped inside the
core, versioned with the emulator that interprets it. The same reason ppsspp's
compat.ini is .incbin'd into its guest rather than downloaded.

WHERE THE DATA COMES FROM, AND WHAT IS IN THE WAY
-------------------------------------------------
Two upstream sources, and neither is the one you would expect:

1. rpcs3.net/compatibility?api=v1&export - machine readable, 6754 title ids,
   and NO SETTINGS. Four fields per title: status, date, update, patchsets.
   Useful for the compatibility verdict and for update-package hashes; useless
   for configuration. The per-title form (api=v1&g=ID) returns more - title,
   wiki-id, status, commit, pr, network - and the wiki-id is a pointer at
   exactly the data we want.

2. wiki.rpcs3.net - where the recommendations actually live, and it is behind a
   Cloudflare challenge. Every path is 403 to automation: api.php,
   Special:Export, index.php, even robots.txt. A browser user agent does not
   help. Defeating that protection is not on the table, so this tool reads
   pages from either
     --dump DIR     a wiki XML export or saved HTML, if upstream provides one
                    (ASK THEM - a one-time dump beats scraping and gives full
                    coverage), or
     --wayback      the Internet Archive, which serves the pages with no
                    challenge. Sanctioned and it works, but coverage is
                    partial: on a 40-title sample, 23 of 37 distinct wiki
                    pages were archived, and the misses were almost all demos,
                    trials and Japanese-titled variants.

THE SETTINGS ARE NOT PROSE
--------------------------
They are a table with a fixed schema, from a template - every page carries the
same introductory sentence ("Options that deviate from RPCS3's default settings
and provide the best experience with this title are listed below"):

    Setting                    | Option      | Notes
    Write color buffers        | On          | Fixes missing graphics ingame.
    Resolution scale threshold | 640 x 640   | ...

So extraction is mechanical. The work is the NAME MAPPING - the wiki's display
names against this core's declared setting names - and that is a dictionary a
person curates once, in wiki-settings-map.json. An extracted name with no entry
is an ERROR, not a shrug: it is reported and the run fails. That is deliberate.
A tool that silently drops what it does not recognise would let the wiki grow a
recommendation we never notice we are ignoring, which is the same shape as a
gate that goes green on a thing it never checked.

usage:
  wiki-settings.py --wayback [--sample N] [--out FILE] [--map FILE]
  wiki-settings.py --dump DIR [--out FILE]
  wiki-settings.py --wayback --sample 40 --coverage   # report yield, write nothing
"""

import argparse
import html
import json
import os
import random
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

COMPAT_EXPORT = "https://rpcs3.net/compatibility?api=v1&export"
COMPAT_ONE = "https://rpcs3.net/compatibility?api=v1&g={}"
WAYBACK_AVAIL = "https://archive.org/wayback/available?url={}"
UA = "chimera-core-rpcs3/1.0 (per-game settings table; contact via the chimera repository)"

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MAP = os.path.join(HERE, "wiki-settings-map.json")


def fetch_json(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def fetch_text(url, timeout=120):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def compat_export(cache):
    """The whole compatibility export, cached on disk - it is 6.5 MB."""
    if cache and os.path.exists(cache):
        with open(cache) as f:
            return json.load(f)["results"]
    data = fetch_json(COMPAT_EXPORT, timeout=180)
    if cache:
        with open(cache, "w") as f:
            json.dump(data, f)
    return data["results"]


def title_record(title_id):
    """title, wiki-id, status and provenance for one id (regional siblings come too)."""
    try:
        res = fetch_json(COMPAT_ONE.format(title_id), timeout=45)["results"]
    except Exception as exc:  # noqa: BLE001 - the network is allowed to be down
        return None, f"api: {exc}"
    rec = res.get(title_id) or next(iter(res.values()), None)
    if rec is not None:
        # one query answers for the whole regional family, so the caller can
        # strike the siblings off its list instead of asking again
        rec = dict(rec)
        rec["_family"] = sorted(res)
    return rec, None


def page_name(title):
    """The wiki page name for a game title. Mirrors MediaWiki's own rule."""
    return title.strip().replace(" ", "_")


def wayback_url(page, tries=4):
    """The archived copy of a page, or None - and ASKING is distinguished from
    being told no.

    This returned None on any exception once, and the archive answers 429 when
    it has had enough: a run over two thousand pages then reported almost all
    of them unarchived, including two this very session had read. An error that
    wears the costume of an absence is gates.md mode C, and it produced a
    confidently wrong list. So a 429 is backed off and retried, and anything
    still unresolved raises rather than quietly counting as missing.
    """
    quoted = urllib.parse.quote(f"wiki.rpcs3.net/index.php?title={page}", safe="")
    delay = 5.0
    for attempt in range(tries):
        try:
            got = fetch_json(WAYBACK_AVAIL.format(quoted), timeout=45)
        except urllib.error.HTTPError as exc:
            if exc.code in (429, 503) and attempt < tries - 1:
                time.sleep(delay)
                delay *= 2
                continue
            raise WaybackUnavailable(f"{page}: {exc}") from exc
        except Exception as exc:  # noqa: BLE001
            if attempt < tries - 1:
                time.sleep(delay)
                delay *= 2
                continue
            raise WaybackUnavailable(f"{page}: {exc}") from exc
        closest = got.get("archived_snapshots", {}).get("closest", {})
        return closest.get("url") if closest.get("available") else None
    raise WaybackUnavailable(page)


class WaybackUnavailable(RuntimeError):
    """The archive could not be asked - which is not the same as no snapshot."""


CONFIG_ANCHOR = 'id="Configuration"'
STOP_ANCHORS = ('id="Known_Issues"', 'id="Special_Notes"', 'id="Patches"')


def settings_from_html(page_html):
    """Every (setting, option, notes) the Configuration section recommends.

    Only tables whose header starts Setting | Option are read. The page's other
    tables - recommended community patches, test history - have their own
    schemas and are deliberately left alone.
    """
    doc = re.sub(r"(?is)<script.*?</script>|<style.*?</style>", "", page_html)
    start = doc.find(CONFIG_ANCHOR)
    if start < 0:
        return []
    end = len(doc)
    for anchor in STOP_ANCHORS:
        at = doc.find(anchor, start + 1)
        if at > start:
            end = min(end, at)
    section = doc[start:end]

    out = []
    for table in re.findall(r"(?is)<table.*?</table>", section):
        rows = re.findall(r"(?is)<tr.*?</tr>", table)
        if not rows:
            continue
        header = [cell_text(c) for c in re.findall(r"(?is)<th.*?</th>", rows[0])]
        if header[:2] != ["Setting", "Option"]:
            continue
        for row in rows[1:]:
            cells = [cell_text(c) for c in re.findall(r"(?is)<t[hd].*?</t[hd]>", row)]
            cells = [c for c in cells if c]
            if len(cells) >= 2:
                out.append((cells[0], cells[1], cells[2] if len(cells) > 2 else ""))
    return out


def cell_text(cell):
    return html.unescape(re.sub(r"<[^>]+>", "", cell)).strip()


def load_map(path):
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f).get("settings", {})


def apply_map(rows, mapping):
    """Wiki (name, option) pairs into (core setting, value). Unknown names are errors."""
    values, unknown = {}, []
    for name, option, _notes in rows:
        entry = mapping.get(name)
        if not entry:
            unknown.append((name, option))
            continue
        if entry.get("ignore"):
            continue
        setting = entry["setting"]
        opts = entry.get("options", {})
        if option in opts:
            values[setting] = opts[option]
        elif entry.get("verbatim"):
            values[setting] = option
        else:
            unknown.append((f"{name} = {option}", option))
    return values, unknown



def write_missing_list(args):
    """Every wiki page we cannot read, as a link somebody can open in a browser.

    The archive holds about half of these pages. The rest exist and are
    perfectly public - a person with a browser passes the Cloudflare challenge
    that we correctly will not - so the useful thing an automated pass can do
    is say precisely WHICH pages need a human, rather than pretending the gaps
    are not there.

    One query per title returns the whole regional family, so the families are
    collapsed as they are discovered: ids already named by an earlier answer
    are not asked about again.
    """
    results = compat_export(args.compat_cache)
    todo = sorted(results)
    seen_ids, pages = set(), {}
    asked = failed = 0
    for title_id in todo:
        if title_id in seen_ids:
            continue
        rec, err = title_record(title_id)
        asked += 1
        if err or not rec:
            failed += 1
            seen_ids.add(title_id)
            continue
        # the answer named the whole family; take them all off the list
        family = rec.get("_family") or [title_id]
        for sibling in family:
            seen_ids.add(sibling)
        seen_ids.add(title_id)
        wiki_id = rec.get("wiki-id")
        title = rec.get("title")
        if not title:
            continue
        entry = pages.setdefault(wiki_id or ("t:" + title), {
            "title": title, "status": rec.get("status"), "wiki_id": wiki_id, "ids": []})
        entry["ids"].append(title_id)
        if asked % 250 == 0:
            print(f"  asked {asked}, {len(pages)} pages so far", flush=True)
        time.sleep(0.25)

    print(f"asked the api about {asked} ids ({failed} unanswered); {len(pages)} distinct wiki pages")

    missing = []
    for key, page in sorted(pages.items(), key=lambda kv: kv[1]["title"].lower()):
        name = page_name(page["title"])
        if wayback_url(name) is None:
            missing.append((page, name))
        time.sleep(0.3)

    with open(args.missing_list, "w") as f:
        f.write("RPCS3 wiki pages with no archived copy - these need a person with a browser\n")
        f.write("=" * 76 + "\n\n")
        f.write("Why this list exists: the recommended settings for each game live on\n")
        f.write("wiki.rpcs3.net, which answers 403 to every automated request (its API, its\n")
        f.write("Special:Export, even its robots.txt). The Internet Archive holds about half\n")
        f.write("the pages and we read those; the ones below it does not hold. A browser gets\n")
        f.write("through the challenge a script should not, so these want a human.\n\n")
        f.write("What to copy: the Configuration section only - its table has the columns\n")
        f.write("Setting | Option | Notes. Nothing else on the page is needed. If a page has\n")
        f.write("no Configuration section, write NONE beside it; that is a useful answer too\n")
        f.write("and saves the next person opening it.\n\n")
        f.write(f"{len(missing)} pages, of {len(pages)} in all.\n\n")
        for page, name in missing:
            url = (f"https://wiki.rpcs3.net/index.php?curid={page['wiki_id']}"
                   if page.get("wiki_id") else
                   f"https://wiki.rpcs3.net/index.php?title={urllib.parse.quote(name)}")
            f.write(f"{page['title']}\n")
            f.write(f"  {url}\n")
            f.write(f"  status: {page.get('status')}   title ids: {', '.join(page['ids'][:6])}\n\n")
    print(f"wrote {args.missing_list}: {len(missing)} pages need a human, of {len(pages)}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--wayback", action="store_true", help="read pages from the Internet Archive")
    src.add_argument("--dump", metavar="DIR", help="read pages from a local wiki export (preferred)")
    ap.add_argument("--sample", type=int, default=0, help="only this many title ids, chosen at random")
    ap.add_argument("--seed", type=int, default=1131, help="sample seed, so a run is repeatable")
    ap.add_argument("--out", metavar="FILE", help="write the table here")
    ap.add_argument("--map", default=DEFAULT_MAP, help="wiki name to core setting dictionary")
    ap.add_argument("--compat-cache", metavar="FILE", help="cache the 6.5 MB export here")
    ap.add_argument("--coverage", action="store_true", help="report what was found and write nothing")
    ap.add_argument("--missing-list", metavar="FILE",
                    help="walk EVERY title and write the wiki links whose pages no archive holds, "
                         "for people to fetch by hand (a browser gets through the challenge; we do not)")
    args = ap.parse_args()

    if args.missing_list:
        return write_missing_list(args)

    mapping = load_map(args.map)
    if not mapping and not args.coverage:
        sys.exit(f"no mapping at {args.map}: nothing could be translated, so nothing would be written")

    results = compat_export(args.compat_cache)
    ids = sorted(results)
    if args.sample:
        random.seed(args.seed)
        ids = random.sample(ids, min(args.sample, len(ids)))

    pages, api_failed = {}, 0
    for title_id in ids:
        rec, err = title_record(title_id)
        if err or not rec or not rec.get("title"):
            api_failed += 1
            continue
        pages.setdefault(rec.get("wiki-id"), {"title": rec["title"], "status": rec.get("status"), "ids": []})
        pages[rec["wiki-id"]]["ids"].append(title_id)
        time.sleep(0.3)

    table, stats, unknown_all = {}, {"archived": 0, "with_settings": 0, "no_page": 0}, {}
    for wiki_id, page in pages.items():
        name = page_name(page["title"])
        if args.dump:
            path = os.path.join(args.dump, name + ".html")
            if not os.path.exists(path):
                stats["no_page"] += 1
                continue
            page_html = open(path, encoding="utf-8", errors="replace").read()
        else:
            url = wayback_url(name)
            if not url:
                stats["no_page"] += 1
                continue
            try:
                page_html = fetch_text(url)
            except Exception:  # noqa: BLE001
                stats["no_page"] += 1
                continue
            time.sleep(0.4)
        stats["archived"] += 1

        rows = settings_from_html(page_html)
        if not rows:
            continue
        stats["with_settings"] += 1
        values, unknown = apply_map(rows, mapping)
        for u, _ in unknown:
            unknown_all[u] = unknown_all.get(u, 0) + 1
        if values:
            for title_id in page["ids"]:
                table[title_id] = {"title": page["title"], "values": values}

    print(f"title ids asked about : {len(ids)} ({api_failed} the api would not answer)")
    print(f"distinct wiki pages   : {len(pages)}")
    print(f"pages readable        : {stats['archived']}  (missing: {stats['no_page']})")
    print(f"pages with a settings table : {stats['with_settings']}")
    print(f"title ids with values : {len(table)}")
    if unknown_all:
        print("\nUNMAPPED - add these to the dictionary or say why they are ignored:")
        for name, count in sorted(unknown_all.items(), key=lambda kv: -kv[1]):
            print(f"  {count:4d}  {name}")

    if args.coverage:
        return 0
    if unknown_all:
        print("\nrefusing to write a table with unmapped names in it", file=sys.stderr)
        return 1
    if args.out:
        with open(args.out, "w") as f:
            json.dump({"source": "rpcs3 wiki via " + ("dump" if args.dump else "web.archive.org"),
                       "games": table}, f, indent=1, sort_keys=True)
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
