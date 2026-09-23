#!/usr/bin/env python3
"""Every PS3 title id in the compatibility list, linked to its wiki page.

usage: wiki-resolve-ids.py <compat-export.json> [--out tools/wiki-ids.json]
                           [--cache tools/wiki-ids.cache.jsonl]

The compatibility API answers a question about one title id with its whole
REGIONAL FAMILY - BLUS30007 comes back with BLES00048 and BLJM60032, all naming
wiki page 1067 - so one question per family is enough. Every member of the
answer is recorded against the page. The first version of this struck the
siblings off its to-do list and then recorded only the id it had asked about,
which left 3,213 of 6,754 ids linked to nothing - Oblivion USA among them, whose
page plainly exists. That is the bug this file exists to not repeat.

Every raw answer is appended to the cache as it arrives, so an interrupted run
resumes and a finished one never has to be run again.
"""
import argparse
import json
import os
import sys
import time
import urllib.request

UA = "chimera-core-rpcs3/1.0 (per-game settings table; contact via the chimera repository)"
ONE = "https://rpcs3.net/compatibility?api=v1&g={}"
HERE = os.path.dirname(os.path.abspath(__file__))


def ask(tid, tries=5):
    delay = 5.0
    for _ in range(tries):
        try:
            req = urllib.request.Request(ONE.format(tid), headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=45) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code != 429 and e.code < 500:
                raise
        except (urllib.error.URLError, TimeoutError, OSError):
            pass
        time.sleep(delay)
        delay *= 2
    raise RuntimeError(f"no answer for {tid} after {tries} tries")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export")
    ap.add_argument("--out", default=os.path.join(HERE, "wiki-ids.json"))
    ap.add_argument("--cache", default=os.path.join(HERE, "wiki-ids.cache.jsonl"))
    a = ap.parse_args()

    ex = json.load(open(a.export))
    ex = ex.get("results", ex)
    ids = {}
    if os.path.exists(a.cache):
        for line in open(a.cache):
            ans = json.loads(line)
            for tid, rec in (ans.get("results") or {}).items():
                ids[tid] = rec
            if ans.get("_asked") and ans["_asked"] not in ids:
                ids[ans["_asked"]] = None     # asked, and the api knew nothing
    todo = [t for t in sorted(ex) if t not in ids]
    print(f"{len(ex)} ids in the export, {len(ids)} already answered, {len(todo)} to ask", flush=True)
    asked = 0
    with open(a.cache, "a") as cache:
        for tid in todo:
            if tid in ids:
                continue          # a sibling's answer already covered it
            ans = ask(tid)
            ans["_asked"] = tid
            cache.write(json.dumps(ans) + "\n")
            cache.flush()
            for sib, rec in (ans.get("results") or {}).items():
                ids[sib] = rec
            ids.setdefault(tid, None)
            asked += 1
            if asked % 200 == 0:
                print(f"  asked {asked}; {len(ids)} of {len(ex)} ids answered", flush=True)
            time.sleep(0.25)

    # when the compatibility list was read: its PROVENANCE date, which the
    # importer stamps on every status and must not guess
    out = {"_fetched": time.strftime("%Y-%m-%d", time.gmtime())}
    for tid in sorted(ex):
        rec = ids.get(tid) or {}
        out[tid] = {"wiki_id": rec.get("wiki-id"), "title": rec.get("title"),
                    "status": (rec.get("status") or ex[tid].get("status"))}
    with open(a.out, "w") as f:
        json.dump(out, f, indent=0, sort_keys=True)
    linked = sum(1 for k, v in out.items() if not k.startswith("_") and v["wiki_id"])
    print(f"done: asked {asked} more; {len(out)} ids, {linked} linked to a wiki page")
    return 0


if __name__ == "__main__":
    sys.exit(main())
