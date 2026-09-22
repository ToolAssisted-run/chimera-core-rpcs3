// Read the RPCS3 wiki's per-game Configuration tables, from inside your own browser.
//
// WHY THIS IS A BROWSER SNIPPET AND NOT A SCRIPT ON THE BUILD MACHINE
// -------------------------------------------------------------------
// wiki.rpcs3.net sits behind a Cloudflare managed challenge. Every path is 403
// to a command-line client - api.php, Special:Export, index.php, even
// robots.txt - and no set of headers changes that, because the gate checks the
// TLS fingerprint and wants JavaScript run to mint a cf_clearance cookie.
// Defeating that (an impersonating TLS build, a headless browser, a solver) is
// circumventing an access decision the site made on purpose, and we are not
// going to do it to a project we are about to ask a favour of.
//
// A browser passes the challenge BY BEING A BROWSER. That is the intended path,
// not a bypass. So this runs in yours: same-origin fetches inherit the cookie
// your session already has, and it reads the same public pages you can read by
// clicking. It is slow on purpose.
//
// HOW TO RUN IT (Chrome)
// ----------------------
//  1. Open https://wiki.rpcs3.net/ and let the "Just a moment" check finish, so
//     the tab holds a valid cf_clearance.
//  2. F12 for DevTools, then the Console tab.
//  3. Chrome will refuse a pasted script the first time: it prints a warning and
//     asks you to type  allow pasting  and press Enter. Do that once.
//  4. Paste this whole file, press Enter. It starts immediately and prints a
//     line every 25 pages.
//  5. Leave the tab open and in the FOREGROUND. Chrome throttles timers in
//     background tabs, which will not break it, only slow it down.
//  6. When it finishes it downloads rpcs3-wiki-settings.json. Send me that file.
//
// It saves progress in localStorage after every page, so if the tab closes, or
// you stop it, just paste it again: it resumes where it left off. To start over
// from nothing, run  wikiHarvest.reset()  first.
// To stop early and keep what you have, run  wikiHarvest.stop()  - it will
// download what it has so far.
//
// Roughly 2200 pages at ~1.2 s each is about three quarters of an hour.

(() => {
  'use strict';

  const PAGES_KEY = 'chimeraWikiPages';
  const DONE_KEY = 'chimeraWikiDone';
  const DELAY_MS = 1200;     // be a guest, not a load test
  const REPORT_EVERY = 25;

  if (!location.hostname.endsWith('rpcs3.net')) {
    console.error('Run this on a wiki.rpcs3.net tab: the fetches must be same-origin ' +
                  'so they carry the cookie that got you past the challenge.');
    return;
  }

  // The page list comes from the compatibility API (title id -> wiki-id), which
  // IS open to scripts. Paste it in as wikiHarvestPages before running, or let
  // this fall back to the wiki's own all-pages list.
  const pages = window.wikiHarvestPages || JSON.parse(localStorage.getItem(PAGES_KEY) || 'null');
  if (!pages) {
    console.error('No page list. Set window.wikiHarvestPages = [...] first ' +
                  '(the JSON I sent you), then paste this again.');
    return;
  }
  localStorage.setItem(PAGES_KEY, JSON.stringify(pages));

  const done = JSON.parse(localStorage.getItem(DONE_KEY) || '{}');
  let stopping = false;

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  function settingsFrom(doc) {
    // Every table in the page whose header is Setting | Option - no walking
    // between headings. MediaWiki has changed how it wraps headings more than
    // once (a bare h2, then an h2 inside div.mw-heading), and a sibling walk
    // that assumes one shape returns nothing on the other while looking like a
    // page with no recommendations. There is no honest way for the person
    // running this to notice that, so the fragile version is not worth its
    // precision: a Setting | Option table anywhere on a game page is the
    // configuration table.
    //
    // The Notes column is deliberately NOT kept. The wiki is CC BY-SA 4.0, and
    // a note is somebody's prose; a setting and its value are facts about a
    // game. We keep the facts, name them in our own vocabulary, and credit the
    // wiki for where they came from, rather than copying the writing.
    const hasSection = !!doc.querySelector('#Configuration, [id="Configuration"]');
    const out = [];
    for (const t of doc.querySelectorAll('table')) {
      const rows = [...t.querySelectorAll('tr')];
      if (!rows.length) continue;
      const hdr = [...rows[0].querySelectorAll('th')].map((c) => c.textContent.trim());
      if (hdr[0] !== 'Setting' || hdr[1] !== 'Option') continue;
      for (const r of rows.slice(1)) {
        const cells = [...r.querySelectorAll('th,td')].map((c) => c.textContent.trim());
        if (cells.length >= 2 && cells[0] && cells[1]) {
          out.push({ setting: cells[0], option: cells[1] });
        }
      }
    }
    if (out.length) return out;        // recommendations found
    return hasSection ? [] : null;     // [] section but nothing in it; null no section
  }

  function download(obj, name) {
    const blob = new Blob([JSON.stringify(obj, null, 1)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  }

  function finish(all, reason) {
    const withSettings = Object.values(all).filter((v) => v.settings && v.settings.length).length;
    const none = Object.values(all).filter((v) => v.settings && !v.settings.length).length;
    const noPage = Object.values(all).filter((v) => v.noPage).length;
    const noSection = Object.values(all).filter((v) => v.settings === null && !v.noPage).length;
    console.log(`%c${reason}: ${Object.keys(all).length} pages read, ` +
                `${withSettings} with settings, ${none} with an empty table, ` +
                `${noSection} with no Configuration section, ` +
                `${noPage} with no wiki page at all`,
                'font-weight:bold');
    download({ source: 'wiki.rpcs3.net, read in a browser session',
               licence: 'page content CC BY-SA 4.0; only setting names and values are kept',
               harvested: new Date().toISOString(),
               pages: all }, 'rpcs3-wiki-settings.json');
  }

  async function run() {
    const todo = pages.filter((p) => !done[p.curid || p.title]);
    console.log(`%cRPCS3 wiki harvest: ${todo.length} pages to read, ` +
                `${Object.keys(done).length} already done. About ` +
                `${Math.round((todo.length * DELAY_MS) / 60000)} minutes. ` +
                `wikiHarvest.stop() to end early.`, 'font-weight:bold');

    let n = 0;
    for (const page of todo) {
      if (stopping) break;
      const key = page.curid || page.title;
      const url = page.curid
        ? `/index.php?curid=${page.curid}`
        : `/index.php?title=${encodeURIComponent(page.title.replace(/ /g, '_'))}`;
      try {
        const res = await fetch(url, { credentials: 'same-origin' });
        if (res.status === 404) {
          // A TITLE WITH NO PAGE IS A FACT, NOT A FAILURE. The page list comes
          // from the compatibility API, which knows games the wiki has never
          // had an article for, so a 404 is simply "nobody wrote one". Record
          // it and carry on.
          //
          // This used to `break` along with every other non-ok status, so one
          // missing article stopped the whole run and the message blamed an
          // expired clearance - which sent the reader to reload a tab that was
          // working perfectly. It cost a 2,178-page harvest 826 pages.
          done[key] = { title: page.title, ids: page.ids || [],
                        status: page.status || null, settings: null, noPage: true };
          localStorage.setItem(DONE_KEY, JSON.stringify(done));
          continue;
        }
        if (!res.ok) {
          // 403 is the clearance expiring, 429 is asking too fast. Both mean
          // stop: reload the page in this tab, let the check pass, and paste
          // the snippet again rather than hammering a gate saying no.
          console.error(`stopped at ${page.title}: HTTP ${res.status}. ` +
                        `Reload the tab, let the challenge pass, and paste this again - ` +
                        `it resumes from here.`);
          break;
        }
        const doc = new DOMParser().parseFromString(await res.text(), 'text/html');
        done[key] = { title: page.title, ids: page.ids || [],
                      status: page.status || null, settings: settingsFrom(doc) };
      } catch (err) {
        console.error(`stopped at ${page.title}: ${err}`);
        break;
      }
      localStorage.setItem(DONE_KEY, JSON.stringify(done));
      if (++n % REPORT_EVERY === 0) {
        const hits = Object.values(done).filter((v) => v.settings && v.settings.length).length;
        console.log(`  ${n}/${todo.length} read, ${hits} with settings so far`);
      }
      await sleep(DELAY_MS);
    }
    finish(done, stopping ? 'stopped' : 'finished');
  }

  window.wikiHarvest = {
    stop() { stopping = true; console.log('stopping after this page...'); },
    reset() { localStorage.removeItem(DONE_KEY); console.log('progress cleared'); },
    done,
  };

  run();
})();
