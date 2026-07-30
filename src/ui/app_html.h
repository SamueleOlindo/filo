#pragma once

namespace filo {

// The whole interface, embedded in the binary.
//
// Nothing to install next to the executable: the page is handed to WebView2
// with NavigateToString. The search does NOT happen here: the JavaScript sends
// the query to C++ and gets back finished results, because the 13 ms are the
// work of the in-memory index, not of the browser.
//
// Split into adjacent literals because MSVC caps a single string constant at
// 16380 characters (C2026). The compiler joins them back together; the seams
// are placed on structural boundaries so each piece is still readable on its
// own.
inline constexpr char kAppHtml[] = R"FILOHTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<!-- No connections, made structural rather than incidental.

     This page draws file names, folder paths and passages of text taken off
     the user's disk. All of it goes through escapeHtml, and the reason for
     this line is that "all of it" is a claim about code, and code changes. If
     a single interpolation is ever missed, a file named
     <img src=https://somewhere/?x=> would be enough to send the name of every
     other file in the list somewhere — from a program whose entire promise is
     that nothing leaves the machine.

     default-src 'none' blocks the lot: no images, no fonts, no fetch, no
     WebSocket, no form post, no frame, no plugin. The two 'unsafe-inline'
     grants are for our own <style> and <script>, which are the whole
     application and are compiled into the executable. Nothing here can be
     changed by anything on the disk being searched. -->
<meta http-equiv="Content-Security-Policy"
      content="default-src 'none';
               style-src 'unsafe-inline';
               script-src 'unsafe-inline';
               img-src data:;
               connect-src 'none';
               form-action 'none';
               base-uri 'none';">
<title>Filo</title>
<style>
  :root {
    --bg: #ffffff;
    --bg-soft: #f6f7f9;
    --bg-hover: #eef0f4;
    --bg-active: #e3e8f0;
    --fg: #14171c;
    --fg-dim: #6b7280;
    --fg-faint: #9aa1ac;
    /* One colour for the whole application, the brand one. The risk with red
       is that it reads as an error: avoided by using it SATURATED only on the
       mark and on pressed buttons, and as a light wash everywhere else. */
    --brand: #d63031;
    --accent: #d63031;
    --accent-soft: #fdecec;
    --border: #e3e6ea;
    --hit: #fde68a;
    --hit-fg: #3a2c00;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #14161a;
      --bg-soft: #1a1d22;
      --bg-hover: #22262d;
      --bg-active: #2b313a;
      --fg: #e8eaed;
      --fg-dim: #9aa1ac;
      --fg-faint: #6b7280;
      --brand: #ff6b6b;
      --accent: #ff6b6b;
      --accent-soft: #2e1f21;
      --border: #262a31;
      --hit: #6b5300;
      --hit-fg: #ffe8a3;
    }
  }

  * { box-sizing: border-box; }
  html, body {
    margin: 0; height: 100%; overflow: hidden;
    background: var(--bg); color: var(--fg);
    font: 14px/1.45 "Segoe UI Variable Text", "Segoe UI", system-ui, sans-serif;
    -webkit-font-smoothing: antialiased;
  }
  /* Sidebar plus one visible view. Filo does two different things now —
     finding files and freeing space — and a single screen with a search box on
     it could only ever be the first. */
  body { display: grid; grid-template-columns: 188px 1fr; }

  nav {
    display: flex; flex-direction: column; gap: 2px;
    padding: 12px 10px; background: var(--bg-soft);
    border-right: 1px solid var(--border); -webkit-app-region: drag;
  }
  nav .brand {
    display: flex; align-items: center; gap: 9px;
    padding: 6px 8px 16px; color: var(--brand); font-weight: 600; font-size: 15px;
  }
  nav .brand span { color: var(--fg); }
  nav button {
    display: flex; align-items: center; gap: 9px; width: 100%;
    padding: 8px 10px; border: 0; border-radius: 6px; cursor: pointer;
    background: transparent; color: var(--fg-dim); font: inherit; text-align: left;
    -webkit-app-region: no-drag;
  }
  nav button:hover { background: var(--bg-hover); color: var(--fg); }
  nav button.on { background: var(--accent-soft); color: var(--accent); font-weight: 600; }
  nav button svg { flex: none; }
  nav .sp { flex: 1; }
  nav .sidestat {
    padding: 8px; color: var(--fg-faint); font-size: 11px; line-height: 1.6;
    font-variant-numeric: tabular-nums; white-space: pre-line;
  }

  .view { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
  .view.off { display: none; }

  /* ------------------------------------------------------------ query bar */
  header {
    display: flex; align-items: center; gap: 12px;
    padding: 14px 18px; border-bottom: 1px solid var(--border);
    background: var(--bg); -webkit-app-region: drag;
  }
  .logo { flex: none; color: var(--brand); }
  #q {
    flex: 1; min-width: 0; border: 0; outline: 0; background: transparent;
    color: var(--fg); font: inherit; font-size: 17px; padding: 4px 0;
    -webkit-app-region: no-drag;
  }
  #q::placeholder { color: var(--fg-faint); }
  .count { flex: none; color: var(--fg-faint); font-size: 12px; font-variant-numeric: tabular-nums; }

  /* -------------------------------------------------------------- results */
  main { flex: 1; overflow-y: auto; overflow-x: hidden; }
  main::-webkit-scrollbar { width: 10px; }
  main::-webkit-scrollbar-thumb { background: var(--border); border-radius: 5px; }

  .row {
    padding: 7px 18px; cursor: default; border-left: 2px solid transparent;
  }
  .row:hover { background: var(--bg-hover); }
  /* A light wash, not a solid fill: the chosen row stands out without the red
     shouting. */
  .row.sel { background: var(--accent-soft); border-left-color: var(--accent); }
  .head { display: flex; align-items: baseline; gap: 10px; }
  .name { flex: none; max-width: 46%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .dir  { flex: 1; min-width: 0; color: var(--fg-dim); font-size: 12.5px;
          overflow: hidden; text-overflow: ellipsis; white-space: nowrap; direction: rtl; text-align: left; }
  mark { background: var(--hit); color: var(--hit-fg); border-radius: 2px; padding: 0 1px; }

  /* Preview of the passage found: this is what separates a match in the
     content from a match in the name, and it has to be readable without
     opening the file. */
  .snip {
    margin-top: 3px; font-size: 12px; line-height: 1.4; color: var(--fg-dim);
    overflow: hidden; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical;
  }
  .tag {
    flex: none; font-size: 10px; letter-spacing: .3px; text-transform: uppercase;
    padding: 1px 5px; border-radius: 3px; background: var(--bg-active); color: var(--fg-faint);
  }
  .tag.body { background: color-mix(in srgb, var(--accent) 18%, transparent); color: var(--accent); }

  .acts { flex: none; display: none; gap: 4px; }
  .row:hover .acts, .row.sel .acts { display: flex; }
  .acts button {
    font: inherit; font-size: 11px; padding: 2px 8px; cursor: pointer;
    border: 1px solid var(--border); border-radius: 4px;
    background: var(--bg); color: var(--fg-dim);
  }
  .acts button:hover { background: var(--accent); color: #fff; border-color: var(--accent); }

  /* ------------------------------------------------------- special states */
  .empty {
    height: 100%; display: flex; flex-direction: column;
    align-items: center; justify-content: center; gap: 14px;
    color: var(--fg-faint); text-align: center; padding: 0 40px;
  }
  .empty .big { color: var(--border); }
  .empty h1 { margin: 0; font-size: 15px; font-weight: 600; color: var(--fg-dim); }
  .empty p { margin: 0; font-size: 13px; max-width: 380px; }
  kbd {
    font: 11px/1 ui-monospace, Consolas, monospace; padding: 3px 6px;
    border: 1px solid var(--border); border-bottom-width: 2px;
    border-radius: 4px; background: var(--bg-soft); color: var(--fg-dim);
  }

  footer {
    flex: none; display: flex; align-items: center; gap: 16px;
    padding: 7px 18px; border-top: 1px solid var(--border);
    background: var(--bg-soft); color: var(--fg-faint); font-size: 11.5px;
    font-variant-numeric: tabular-nums;
  }
  footer .sp { flex: 1; }
  /* The sidebar took 188px off the width. The keyboard hints are the first
     thing that should go when there is not room for both. */
  footer > span:first-child { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  footer .keys { flex: none; white-space: nowrap; }
  @media (max-width: 760px) { footer .keys { display: none; } }

  /* ------------------------------------------------------ reclaiming space */
  .vhead {
    flex: none; display: flex; align-items: center; gap: 12px;
    padding: 16px 20px; border-bottom: 1px solid var(--border);
    -webkit-app-region: drag;
  }
  .vhead h2 { margin: 0; font-size: 16px; font-weight: 600; }
  .vhead .sp { flex: 1; }
  button.primary, button.danger {
    font: inherit; font-size: 13px; padding: 6px 14px; border-radius: 6px;
    cursor: pointer; border: 1px solid var(--border); background: var(--bg);
    color: var(--fg); -webkit-app-region: no-drag;
  }
  button.primary:hover { background: var(--bg-hover); }
  button.danger { border-color: var(--accent); color: #fff; background: var(--accent); }
  button.danger:hover { filter: brightness(1.08); }
  button:disabled { opacity: .45; cursor: default; filter: none; }

  #space { flex: 1; overflow-y: auto; padding: 6px 0 20px; }
  #space::-webkit-scrollbar { width: 10px; }
  #space::-webkit-scrollbar-thumb { background: var(--border); border-radius: 5px; }

  .summary {
    padding: 14px 20px; color: var(--fg-dim); font-size: 13px;
    border-bottom: 1px solid var(--border);
  }
  .summary b { color: var(--fg); font-size: 15px; }

  /* A category is the unit of judgement. Nobody can weigh forty thousand
     files; anyone can decide about "35 GB your tools rebuild". */
  .cat { border-bottom: 1px solid var(--border); }
  .cathead {
    display: flex; align-items: center; gap: 12px; width: 100%;
    padding: 13px 20px; border: 0; background: transparent; color: var(--fg);
    font: inherit; text-align: left; cursor: pointer;
  }
  .cathead:hover { background: var(--bg-hover); }
  .cathead .bar {
    flex: none; width: 4px; height: 30px; border-radius: 2px; background: var(--border);
  }
  .cat.can .cathead .bar { background: var(--accent); }
  .cathead .nm { flex: none; font-weight: 600; }
  .cathead .sz { flex: none; font-variant-numeric: tabular-nums; color: var(--fg-dim); }
  .cathead .sp { flex: 1; }
  .cathead .note { flex: none; font-size: 11.5px; color: var(--fg-faint); }
  .cathead .caret { flex: none; color: var(--fg-faint); transition: transform .15s; }
  .cat.open .cathead .caret { transform: rotate(90deg); }
  .catbody { display: none; padding: 0 20px 14px 36px; }
  .cat.open .catbody { display: block; }

  .item .row { display: flex; align-items: center; gap: 9px; flex: 1; min-width: 0; }
  .item { display: flex; align-items: center; gap: 9px; padding: 3px 6px;
          border-radius: 4px; font-size: 12.5px; }
  .item:hover { background: var(--bg-hover); }
  .item input { flex: none; accent-color: var(--accent); margin: 0; }
  .item .sz { flex: none; width: 74px; text-align: right; color: var(--fg-dim);
              font-variant-numeric: tabular-nums; }
  .item .p { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .item .why { flex: none; max-width: 34%; color: var(--fg-faint); font-size: 11.5px;
               overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .more { padding: 6px; color: var(--fg-faint); font-size: 11.5px; }

  /* What was just moved to the bin. A line above the list, not a screen
     instead of it: after deleting six files you want to carry on with the
     other four hundred, not scan the disk again to see them. */
  #done {
    flex: none; margin: 12px 18px 0; padding: 10px 14px; border-radius: 6px;
    background: var(--accent-soft); color: var(--fg-dim); font-size: 12.5px;
  }
  #done b { color: var(--accent); }

  /* Asking the model what a file is. Everything else on this screen is a rule
     Filo can defend; this is prose nothing checks, so it is marked as such and
     kept visually apart from the reason above it.

     NOT scoped to .item. The screen has two kinds of row — .item in the flat
     lists and .gfile inside a duplicate set — and scoping this to the first one
     left the button in the duplicate sets rendering as a bare browser button. */
  .ask {
    flex: none; width: 20px; height: 20px; border-radius: 4px; cursor: pointer;
    border: 1px solid var(--border); background: var(--bg); color: var(--fg-faint);
    font: inherit; font-size: 11px; line-height: 1; padding: 0;
  }
  .ask:hover { background: var(--accent); color: #fff; border-color: var(--accent); }
  .said {
    display: flex; gap: 8px; align-items: flex-start;
    margin: 2px 0 8px 32px; padding: 7px 10px; border-radius: 5px;
    background: var(--bg-soft); border-left: 2px solid var(--accent);
    font-size: 12px; color: var(--fg-dim);
  }
  .said .who {
    flex: none; font-size: 9.5px; letter-spacing: .4px; text-transform: uppercase;
    padding: 2px 5px; border-radius: 3px; margin-top: 1px;
    background: color-mix(in srgb, var(--accent) 16%, transparent); color: var(--accent);
  }

  .group { padding: 12px 20px; border-bottom: 1px solid var(--border); }
  .ghead {
    display: flex; align-items: baseline; gap: 10px;
    color: var(--fg-dim); font-size: 12.5px; margin-bottom: 7px;
  }
  .ghead b { color: var(--fg); font-size: 14px; }
  .ghead .sp { flex: 1; }
  .ghead button {
    font: inherit; font-size: 11px; padding: 2px 8px; cursor: pointer;
    border: 1px solid var(--border); border-radius: 4px;
    background: var(--bg); color: var(--fg-dim);
  }
  .ghead button:hover { background: var(--bg-hover); color: var(--fg); }

  .gfile {
    display: flex; align-items: center; gap: 9px;
    padding: 4px 6px; border-radius: 4px; font-size: 12.5px;
    overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  }
  .gfile:hover { background: var(--bg-hover); }
  .gfile input { flex: none; accent-color: var(--accent); margin: 0; }
  .gfile .p { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  /* The copy being kept is not an option and does not get a checkbox. Making it
     look selectable would invite the one mistake this whole screen is built to
     prevent. */
  .gfile.keep { color: var(--fg-dim); }
  .gfile.keep .badge {
    flex: none; font-size: 10px; letter-spacing: .3px; text-transform: uppercase;
    padding: 1px 6px; border-radius: 3px;
    background: color-mix(in srgb, var(--accent) 16%, transparent); color: var(--accent);
  }
  .spin {
    width: 14px; height: 14px; border: 2px solid var(--border);
    border-top-color: var(--accent); border-radius: 50%;
    animation: turn .7s linear infinite;
  }
  @keyframes turn { to { transform: rotate(360deg); } }
</style>
</head>)FILOHTML"
R"FILOHTML(
<body>

<nav>
  <div class="brand">
    <!-- Ariadne's thread: tangled into a spiral, then drawn straight to the point -->
    <svg width="22" height="22" viewBox="0 0 100 100" fill="none" stroke="currentColor"
         stroke-width="9" stroke-linecap="round" stroke-linejoin="round">
      <path d="M38 50L39.4 50.1L40.8 50.6L42.1 51.5L43.1 53L43.5 54.8L43.3 56.8L42.4 58.8L40.7 60.4L38.4 61.4L35.8 61.7L33 60.9L30.5 59.1L28.6 56.5L27.8 53.1L28.1 49.5L29.7 46L32.6 43.1L36.5 41.3L41.1 40.9L45.6 42.1L49.8 44.9L52.8 49.2L54.2 54.5L53.8 60.2L51.4 65.6L47.1 70.1L41.3 72.9L34.6 73.7L27.8 72.1L21.7 68.1L17.2 62.1L14.9 54.7L15.2 46.6L18.4 38.9L24.2 32.5L32 28.2L52 27.5L60 33L64 43L66 50L74 50"/>
      <circle cx="84" cy="50" r="7.5" fill="currentColor" stroke="none"/>
    </svg>
    <span>Filo</span>
  </div>

  <button class="nav on" data-view="search">
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor"
         stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/></svg>
    Search
  </button>
  <button class="nav" data-view="space">
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor"
         stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18M8 6V4h8v2M6 6l1 14h10l1-14"/></svg>
    Space
  </button>

  <span class="sp"></span>
  <div class="sidestat" id="sidestat">…</div>
</nav>

<section class="view" id="view-search">
  <header>
    <input id="q" type="text" placeholder="Search your files…" spellcheck="false" autocomplete="off">
    <span class="count" id="count"></span>
  </header>

  <main id="list"></main>

  <footer>
    <span id="stat">…</span>
    <span class="sp"></span>
    <span class="keys"><kbd>↑</kbd><kbd>↓</kbd> move · <kbd>Enter</kbd> open · <kbd>Ctrl+Enter</kbd> show in folder</span>
  </footer>
</section>

<section class="view off" id="view-space">
  <div class="vhead">
    <h2>Reclaim space</h2>
    <span class="sp"></span>
    <button class="primary" id="scan">Analyse the disk</button>
  </div>

  <!-- What just happened, above the list rather than instead of it. -->
  <div id="done" style="display:none"></div>

  <main id="space"></main>

  <footer id="spacebar" style="display:none">
    <span id="picked">Nothing selected</span>
    <span class="sp"></span>
    <button class="danger" id="recycle" disabled>Move to Recycle Bin</button>
  </footer>
</section>

<script>
const $q = document.getElementById('q');
const $list = document.getElementById('list');
const $count = document.getElementById('count');
const $stat = document.getElementById('stat');

let rows = [];
let selected = 0;
let lastQuery = '';
// What was actually searched for, once the query planner has lifted out the
// format, the folder and the date. Highlighting uses this and not lastQuery:
// "il pdf del contratto" searches for "contratto", and marking "il" and "del"
// in every snippet would make a right answer look like a wrong one.
let lastTerms = '';

const WELCOME = `<div class="empty">
  <svg class="big" width="118" height="118" viewBox="0 0 100 100" fill="none"
       stroke="currentColor" stroke-width="7" stroke-linecap="round" stroke-linejoin="round">
    <path d="M38 50L39.4 50.1L40.8 50.6L42.1 51.5L43.1 53L43.5 54.8L43.3 56.8L42.4 58.8L40.7 60.4L38.4 61.4L35.8 61.7L33 60.9L30.5 59.1L28.6 56.5L27.8 53.1L28.1 49.5L29.7 46L32.6 43.1L36.5 41.3L41.1 40.9L45.6 42.1L49.8 44.9L52.8 49.2L54.2 54.5L53.8 60.2L51.4 65.6L47.1 70.1L41.3 72.9L34.6 73.7L27.8 72.1L21.7 68.1L17.2 62.1L14.9 54.7L15.2 46.6L18.4 38.9L24.2 32.5L32 28.2L52 27.5L60 33L64 43L66 50L74 50"/>
    <circle cx="84" cy="50" r="6" fill="currentColor" stroke="none"/>
  </svg>
  <h1>Type to search</h1>
  <p>Filo keeps the whole disk index in memory: results appear as you type.</p>
</div>`;

function escapeHtml(text) {
  return text.replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
}

// Highlights where the query occurs in the name. The comparison is
// case-insensitive, but the text shown stays the original one.
function highlight(text, needle) {
  const safe = escapeHtml(text);
  if (!needle) return safe;
  const at = text.toLowerCase().indexOf(needle.toLowerCase());
  if (at < 0) return safe;
  return escapeHtml(text.slice(0, at)) +
         '<mark>' + escapeHtml(text.slice(at, at + needle.length)) + '</mark>' +
         escapeHtml(text.slice(at + needle.length));
}

// In the preview a term can appear several times and in different places:
// highlight all of them, not only the first.
function highlightAll(text, query) {
  const terms = query.split(/[\s"]+/).filter(t => t.length >= 2);
  if (!terms.length) return escapeHtml(text);

  const lower = text.toLowerCase();
  const spans = [];
  for (const term of terms) {
    const needle = term.toLowerCase();
    let from = 0;
    for (;;) {
      const at = lower.indexOf(needle, from);
      if (at < 0) break;
      spans.push([at, at + needle.length]);
      from = at + needle.length;
    }
  }
  if (!spans.length) return escapeHtml(text);
  spans.sort((a, b) => a[0] - b[0]);

  let out = '', cursor = 0;
  for (const [from, to] of spans) {
    if (from < cursor) continue;   // overlaps: keep the first one
    out += escapeHtml(text.slice(cursor, from)) +
           '<mark>' + escapeHtml(text.slice(from, to)) + '</mark>';
    cursor = to;
  }
  return out + escapeHtml(text.slice(cursor));
}

function render() {
  if (!rows.length) {
    $list.innerHTML = lastQuery
      ? '<div class="empty"><h1>No results</h1><p>Nothing matches "' +
        escapeHtml(lastQuery) + '".</p></div>'
      : WELCOME;
    return;
  }
  const html = rows.map((row, i) => {
    let tag = '';
    if (row.inBody && !row.inName) {
      tag = '<span class="tag body">text' +
            (row.passages > 1 ? ' ' + row.passages + '×' : '') + '</span>';
    } else if (row.inBody && row.inName) {
      tag = '<span class="tag body">name + text</span>';
    }
    const snip = row.snippet
      ? '<div class="snip">' + highlightAll(row.snippet, lastTerms) + '</div>'
      : '';
    // The actions show up on hover: without them the only way to open a file
    // would be the double click, which nobody tries on a list of results.
    const actions =
      '<span class="acts">' +
        '<button data-action="open" title="Open this file">open</button>' +
        '<button data-action="reveal" title="Show in folder">folder</button>' +
      '</span>';
    return '<div class="row' + (i === selected ? ' sel' : '') + '" data-i="' + i + '">' +
             '<div class="head">' +
               '<span class="name">' + highlight(row.name, lastTerms) + '</span>' +
               '<span class="dir">' + escapeHtml(row.dir) + '</span>' + tag + actions +
             '</div>' + snip +
           '</div>';
  }).join('');
  $list.innerHTML = html;
}

function scrollToSelected() {
  const el = $list.querySelector('.row.sel');
  if (el) el.scrollIntoView({ block: 'nearest' });
}

function send(message) {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(JSON.stringify(message));
  }
}

// Moves the selection WITHOUT rebuilding the list.
//
// This used to call render(), which rewrites the whole HTML: the element under
// the pointer was destroyed halfway through the click, and the double click
// never arrived. That was both the reason opening a file did not work and one
// of the causes of the slowdown while typing.
function updateSelection(next) {
  if (!rows.length) return;
  const clamped = Math.max(0, Math.min(next, rows.length - 1));
  if (clamped === selected) return;
  const before = $list.querySelector('.row.sel');
  if (before) before.classList.remove('sel');
  selected = clamped;
  const after = $list.querySelector('.row[data-i="' + selected + '"]');
  if (after) { after.classList.add('sel'); after.scrollIntoView({ block: 'nearest' }); }
}

let timer = null;
$q.addEventListener('input', () => {
  clearTimeout(timer);
  // A full search costs between 30 and 100 ms across names, content and
  // previews. With too short a wait the requests pile up while you type and
  // the bar feels like it snags: wait for the hand to stop instead.
  timer = setTimeout(() => send({ type: 'search', q: $q.value }), 140);
});

$q.addEventListener('keydown', event => {
  if (event.key === 'ArrowDown') {
    updateSelection(selected + 1);
    event.preventDefault();
  } else if (event.key === 'ArrowUp') {
    updateSelection(selected - 1);
    event.preventDefault();
  } else if (event.key === 'Enter' && rows[selected]) {
    send({ type: event.ctrlKey ? 'reveal' : 'open', path: rows[selected].path });
    event.preventDefault();
  } else if (event.key === 'Escape') {
    if ($q.value) { $q.value = ''; send({ type: 'search', q: '' }); }
    else send({ type: 'hide' });
  }
});

$list.addEventListener('click', event => {
  const row = event.target.closest('.row');
  if (!row) return;
  updateSelection(Number(row.dataset.i));
  // A click on either action opens straight away, with no double click.
  const action = event.target.closest('[data-action]');
  if (action) {
    send({ type: action.dataset.action, path: rows[selected].path });
    event.preventDefault();
  }
});

$list.addEventListener('dblclick', event => {
  const row = event.target.closest('.row');
  if (row) send({ type: 'open', path: rows[Number(row.dataset.i)].path });
});

window.chrome.webview.addEventListener('message', event => {
  const message = typeof event.data === 'string' ? JSON.parse(event.data) : event.data;
  if (message.type === 'results') {
    rows = message.items;
    lastQuery = message.q;
    lastTerms = message.terms || message.q;
    selected = 0;
    if (message.total) {
      const parts = [];
      if (message.nameHits) parts.push(message.nameHits.toLocaleString('en-US') + ' in names');
      if (message.contentHits) parts.push(message.contentHits.toLocaleString('en-US') + ' in text');
      if (message.meaningHits) parts.push(message.meaningHits.toLocaleString('en-US') + ' by meaning');
      $count.textContent = parts.join(' · ') + ' · ' + message.ms.toFixed(0) + ' ms';
    } else {
      $count.textContent = '';
    }
    render();
  } else if (message.type === 'status') {
    $stat.textContent = message.text;
    document.getElementById('sidestat').textContent = message.text.replace(/ · /g, '\n');
  } else if (message.type === 'prefill') {
    $q.value = message.q;
    send({ type: 'search', q: message.q });
  }
});

render();
$q.focus();
send({ type: 'ready' });
</script>)FILOHTML"
R"FILOHTML(
<script>
// ------------------------------------------------------------- reclaim space
//
// A second listener rather than another branch in the first: the two screens
// share nothing but the bridge, and keeping them apart means a change to one
// cannot break the other.

const $space = document.getElementById('space');
const $spacebar = document.getElementById('spacebar');
const $picked = document.getElementById('picked');
const $scan = document.getElementById('scan');
const $recycle = document.getElementById('recycle');
const $done = document.getElementById('done');

let categories = [];
let picks = new Set();
let sizeById = new Map();
let scanning = false;
// Which scan the ids on screen belong to. Sent back with anything that names
// one, because a scan rebuilds the list and position 12 stops meaning the file
// it meant a minute ago. The host refuses a mismatch rather than guessing.
let scanGen = 0;
// What the scan said could be freed, and how much of it has been since. Kept so
// the headline can come down as rows leave, instead of the whole screen being
// replaced by a total and another scan being needed to see what is left.
let offerableTotal = 0;
let freedTotal = 0;

function gigabytes(bytes) {
  if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(2) + ' GB';
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
  return (bytes / 1024).toFixed(0) + ' KB';
}

function esc(text) {
  return text.replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
}

// Views ---------------------------------------------------------------------
function showView(name) {
  for (const other of document.querySelectorAll('nav button.nav')) {
    other.classList.toggle('on', other.dataset.view === name);
  }
  document.getElementById('view-search').classList.toggle('off', name !== 'search');
  document.getElementById('view-space').classList.toggle('off', name !== 'space');
  if (name === 'search') $q.focus();
}
for (const button of document.querySelectorAll('nav button.nav')) {
  button.addEventListener('click', () => showView(button.dataset.view));
}
// Lets the automated capture reach the second screen. WebView2 cannot be driven
// with synthetic clicks from outside, so without a hook like this only the
// first view would ever be verified.
window.filoShowView = showView;

const SPACE_IDLE = `<div class="empty">
  <svg class="big" width="90" height="90" viewBox="0 0 24 24" fill="none" stroke="currentColor"
       stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">
    <path d="M3 6h18M8 6V4h8v2M6 6l1 14h10l1-14"/></svg>
  <h1>See where your disk went</h1>
  <p>Filo sorts the biggest files by what they actually are — output your tools
     rebuild, installers you already ran, copies you have twice. Nothing is deleted:
     what you pick goes to the Recycle Bin, and only after you say so.</p>
</div>`;

function renderIdle() {
  $space.innerHTML = SPACE_IDLE;
  $spacebar.style.display = 'none';
}
renderIdle();

// Results -------------------------------------------------------------------
//
// Categories first, files second. The disk is 375 GB across 1.87 million
// files; what a person can actually decide about is "35 GB your tools rebuild"
// and "8 GB downloaded and never opened again".

const CARET = `<svg class="caret" width="12" height="12" viewBox="0 0 24 24" fill="none"
  stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M9 5l7 7-7 7"/></svg>`;

function renderCategories(message) {
  categories = message.categories || [];
  scanGen = message.gen || 0;
  offerableTotal = message.offerable || 0;
  freedTotal = 0;
  picks = new Set();
  sizeById = new Map();
  $done.style.display = 'none';

  if (!categories.length) {
    $space.innerHTML = `<div class="empty"><h1>Nothing worth removing</h1>
      <p>Filo looked at ${message.examined.toLocaleString('en-US')} files.</p></div>`;
    $spacebar.style.display = 'none';
    return;
  }

  let html = `<div class="summary"><b>${gigabytes(message.offerable)}</b> can be freed,
    out of ${gigabytes(message.total)} across
    ${message.examined.toLocaleString('en-US')} files. The categories below are ordered
    by how sure Filo is; the last few are shown so the numbers add up, and cannot be
    selected.</div>`;

  categories.forEach((cat, c) => {
    html += `<div class="cat ${cat.offerable ? 'can' : ''}" data-cat="${c}"
                  data-bytes="${cat.bytes}" data-files="${cat.files}">
      <button class="cathead">
        <span class="bar"></span>
        <span class="nm">${esc(cat.name)}</span>
        <span class="sz">${gigabytes(cat.bytes)}</span>
        <span class="sp"></span>
        <span class="note">${cat.files.toLocaleString('en-US')} ${cat.groups ? 'sets' : 'files'}${
          cat.offerable ? '' : ' · shown, not offered'}</span>
        ${CARET}
      </button>
      <div class="catbody">`;

    if (cat.groups) {
      // Identical copies keep the group shape: seeing which one is kept is most
      // of what makes the choice easy.
      cat.groups.forEach((group, g) => {
        html += `<div class="group" style="padding:8px 0;border:0">
          <div class="ghead"><b>${gigabytes(group.reclaimable)}</b>
            <span>${group.files.length} copies · ${gigabytes(group.each)} each</span>
            <span class="sp"></span>
            <button data-all="${c}:${g}">select the other ${group.files.length - 1}</button>
          </div>`;
        group.files.forEach((file, f) => {
          // The keeper can be asked about like anything else; it just cannot be
          // ticked. An id is a name, not permission.
          const ask = `<button class="ask" data-ask="${file.id}"
                       title="Ask the model what this is">?</button>`;
          if (!file.pick) {
            html += `<div class="gfile keep"><span class="badge">keep</span>
                     <span class="p">${esc(file.path)}</span>${ask}</div>`;
          } else {
            sizeById.set(file.id, group.each);
            html += `<div class="gfile"><label class="row">
                       <input type="checkbox" data-pick="${file.id}">
                       <span class="p">${esc(file.path)}</span></label>${ask}</div>`;
          }
        });
        html += `</div>`;
      });
    } else {
      if (cat.offerable) {
        html += `<div class="ghead"><span class="sp"></span>
          <button data-allcat="${c}">select all ${cat.items.length}</button></div>`;
      }
      for (const item of cat.items) {
        if (item.pick) sizeById.set(item.id, item.size);
        const box = item.pick
          ? `<input type="checkbox" data-pick="${item.id}">` : '';
        // The button sits outside the label on purpose: inside one, clicking it
        // would also tick the checkbox, and asking a question about a file is
        // not the same as choosing to delete it.
        //
        // Every row gets one, deletable or not. It used to be tied to the
        // checkbox, which meant the model only ever explained files the rules
        // had already explained and said nothing about the large unknown ones
        // that were the entire reason for asking it.
        const ask = item.id >= 0
          ? `<button class="ask" data-ask="${item.id}" title="Ask the model what this is">?</button>`
          : '';
        // Both columns get cut off at this width, and the reason is the part
        // that decides. The full text is one hover away rather than lost.
        html += `<div class="item">
          <label class="row" title="${esc(item.path)}\n${esc(item.why || '')}">${box}
            <span class="sz">${gigabytes(item.size)}</span>
            <span class="p">${esc(item.path)}</span>
            <span class="why">${esc(item.why || '')}</span></label>${ask}</div>`;
      }
      if (cat.files > cat.items.length) {
        html += `<div class="more">and ${(cat.files - cat.items.length).toLocaleString('en-US')}
                 more, not listed</div>`;
      }
    }
    html += `</div></div>`;
  });
)FILOHTML"
R"FILOHTML(
  $space.innerHTML = html;
  // The automated capture cannot click, so it asks for a category to be open.
  // Without this the screenshot only ever shows the collapsed headers and the
  // half of this screen that lists actual files goes unverified.
  if (window.filoExpandFirst) {
    // Open the first category that has a file to ask about, not simply the
    // first: if duplicates come out on top the capture shows groups and the
    // question button never appears in it.
    const ask = $space.querySelector('.ask');
    const open = ask ? ask.closest('.cat') : $space.querySelector('.cat');
    if (open) open.classList.add('open');
    if (window.filoAskFirst) {
      if (ask) ask.click(); else send({ type: 'probeCapture' });
    }
  }
  $spacebar.style.display = 'flex';
  updatePicked();
}

// Takes the rows that really went to the bin off the screen, and puts the
// numbers back in agreement with what is left.
//
// Everything here is bookkeeping the page already had: the size of each row, the
// category it sits in, and the header totals. What it did NOT have was which
// rows went, which is why it used to give up and ask for another scan.
function removeRows(ids) {
  for (const id of ids) {
    const box = $space.querySelector(`[data-pick="${id}"], [data-ask="${id}"]`);
    if (!box) continue;
    const row = box.closest('.item, .gfile');
    if (!row) continue;

    const size = sizeById.get(id) || 0;
    const cat = row.closest('.cat');
    if (cat) {
      // Header first, while the row is still there to be counted.
      const bytes = Number(cat.dataset.bytes || 0) - size;
      const files = Number(cat.dataset.files || 0) - 1;
      cat.dataset.bytes = Math.max(0, bytes);
      cat.dataset.files = Math.max(0, files);
      const sz = cat.querySelector('.cathead .sz');
      const note = cat.querySelector('.cathead .note');
      if (sz) sz.textContent = gigabytes(Math.max(0, bytes));
      if (note) {
        note.textContent = `${Math.max(0, files).toLocaleString('en-US')} left`;
      }
    }

    // The model's answer about a file that no longer exists goes with it.
    const said = row.nextElementSibling;
    if (said && said.classList.contains('said')) said.remove();
    row.remove();

    picks.delete(id);
    sizeById.delete(id);
    freedTotal += size;
  }

  // The headline figure is what can still be freed, so it comes down by what
  // just was.
  const summary = $space.querySelector('.summary b');
  if (summary) summary.textContent = gigabytes(Math.max(0, offerableTotal - freedTotal));
}

function updatePicked() {
  let bytes = 0;
  for (const id of picks) bytes += sizeById.get(id) || 0;
  $picked.textContent = picks.size
    ? `${picks.size} selected · ${gigabytes(bytes)}`
    : 'Nothing selected';
  $recycle.disabled = picks.size === 0;
}

$space.addEventListener('change', event => {
  const box = event.target.closest('input[type=checkbox]');
  if (!box) return;
  const id = Number(box.dataset.pick);
  if (box.checked) picks.add(id); else picks.delete(id);
  updatePicked();
});

function syncBoxes() {
  for (const box of $space.querySelectorAll('input[type=checkbox]')) {
    box.checked = picks.has(Number(box.dataset.pick));
  }
  updatePicked();
}

$space.addEventListener('click', event => {
  // Open or close a category.
  const head = event.target.closest('.cathead');
  if (head && !event.target.closest('button[data-all],button[data-allcat]')) {
    head.parentElement.classList.toggle('open');
    return;
  }

  const one = event.target.closest('button[data-all]');
  if (one) {
    const [c, g] = one.dataset.all.split(':').map(Number);
    for (const file of categories[c].groups[g].files.slice(1)) picks.add(file.id);
    syncBoxes();
    return;
  }

  // Ask what a file is. Clicking again takes the answer away, so a screen full
  // of questions can be cleared one at a time.
  const ask = event.target.closest('button[data-ask]');
  if (ask) {
    // Both kinds of row. Looking only for .item found nothing inside a
    // duplicate set, so this threw on the next line and the click did nothing
    // at all — a dead button rather than a visibly broken one.
    const row = ask.closest('.item, .gfile');
    if (!row) return;
    const open = row.nextElementSibling;
    if (open && open.classList.contains('said')) { open.remove(); return; }
    const said = document.createElement('div');
    said.className = 'said';
    said.dataset.for = ask.dataset.ask;
    said.innerHTML = `<span class="who">model</span><span class="txt">thinking…</span>`;
    row.after(said);
    send({ type: 'describe', id: Number(ask.dataset.ask), gen: scanGen });
    return;
  }

  const all = event.target.closest('button[data-allcat]');
  if (all) {
    for (const item of categories[Number(all.dataset.allcat)].items) {
      if (item.pick) picks.add(item.id);
    }
    syncBoxes();
  }
});

// Actions -------------------------------------------------------------------
$scan.addEventListener('click', () => {
  if (scanning) return;
  scanning = true;
  $scan.disabled = true;
  $spacebar.style.display = 'none';
  $space.innerHTML = `<div class="empty"><div class="spin"></div>
    <h1 id="phase">Measuring the disk…</h1>
    <p id="phasedetail">This reads the size of every file. About a minute.</p></div>`;
  send({ type: 'reclaimScan' });
});

$recycle.addEventListener('click', () => {
  if (!picks.size) return;
  $recycle.disabled = true;
  $picked.textContent = 'Moving to the Recycle Bin…';
  send({ type: 'reclaimRecycle', picks: Array.from(picks), gen: scanGen });
});

window.chrome.webview.addEventListener('message', event => {
  const message = typeof event.data === 'string' ? JSON.parse(event.data) : event.data;

  if (message.type === 'reclaimProgress') {
    const phase = document.getElementById('phase');
    const detail = document.getElementById('phasedetail');
    if (phase) phase.textContent = message.phase;
    if (detail) detail.textContent = message.detail;

  } else if (message.type === 'reclaimResults') {
    scanning = false;
    $scan.disabled = false;
    renderCategories(message);

  } else if (message.type === 'described') {
    const said = $space.querySelector(`.said[data-for="${message.id}"] .txt`);
    if (said) said.textContent = message.text;
    // A capture asks a SECOND time, on a different row. The first question
    // worked even when the host was reading the id wrongly and every question
    // was secretly about file 0 — the reply came back labelled 0 too, so it
    // landed on the first row and looked right. Only a second row exposed it.
    if (window.filoAskFirst && !window.filoAskedTwice) {
      window.filoAskedTwice = 1;
      const next = $space.querySelectorAll('.ask')[1];
      if (next) { next.click(); return; }
    }

    // Then exercise the part that cannot be checked by looking at the C++:
    // that recycling takes rows OFF the list instead of throwing the list away.
    // Deleting a file to test this would be absurd, so the capture feeds the
    // handler the message the host would have sent, using a row that is really
    // on screen, and photographs what is left.
    if (window.filoAskFirst && !window.filoTestedRemoval) {
      window.filoTestedRemoval = 1;
      // The duplicate sets use a different row element from the flat lists, and
      // the question button was styled and wired for the flat one only: inside
      // a set it rendered as a bare browser button and did nothing when
      // clicked. Nothing noticed, because the capture only ever clicked rows in
      // the flat lists. This is that check.
      const inSet = $space.querySelector('.gfile .ask');
      let setResult = 'no duplicate sets on this disk';
      if (inSet) {
        // Opened first: inside a collapsed category the element inherits
        // display:none, and getComputedStyle would report a width that says
        // nothing about whether the rule matched.
        const cat = inSet.closest('.cat');
        if (cat) cat.classList.add('open');
        inSet.click();
        const answered = inSet.closest('.gfile').nextElementSibling;
        const styled = getComputedStyle(inSet).width === '20px';
        setResult = (answered && answered.classList.contains('said') ? 'wired' : 'DEAD') +
                    ', ' + (styled ? 'styled' : 'UNSTYLED');
      }

      const first = $space.querySelector('[data-pick]');
      const id = first ? Number(first.dataset.pick) : -1;
      const before = $space.querySelectorAll('.item, .gfile').length;
      const headBefore = ($space.querySelector('.summary b') || {}).textContent;

      if (id >= 0) {
        const size = sizeById.get(id) || 0;
        removeRows([id]);
        $done.innerHTML = `<b>${gigabytes(size)} freed.</b>
          1 in the Recycle Bin, where you can still get it back.`;
        $done.style.display = 'block';
        updatePicked();
      }

      const after = $space.querySelectorAll('.item, .gfile').length;
      const headAfter = ($space.querySelector('.summary b') || {}).textContent;
      window.filoRemoval = `rows ${before}->${after} head ${headBefore}->${headAfter}` +
                           ` | ask in duplicate set: ${setResult}`;
      send({ type: 'probeCapture' });
      return;
    }

  } else if (message.type === 'reclaimDone') {
    scanning = false;
    $scan.disabled = false;
    if (message.error) {
      $space.innerHTML = `<div class="empty"><h1>Nothing was moved</h1>
        <p>${esc(message.error)}</p></div>`;
    } else {
      // Each reason said in its own words. They used to be summed and reported
      // as "changed since Filo compared them", which was true of one of them.
      // The one that matters most is too-big: it means Filo refused on purpose,
      // because Windows destroys a file that will not fit rather than
      // recycling it, and the user should hear that as a decision.
      const notes = [];
      if (message.tooBig) {
        notes.push(`${message.tooBig} ${message.tooBig === 1 ? 'was' : 'were'} left alone:
          too large for the Recycle Bin, so deleting would have been permanent.`);
      }
      if (message.changed) {
        notes.push(`${message.changed} changed since Filo measured them.`);
      }
      if (message.missing) {
        notes.push(`${message.missing} were already gone.`);
      }
      if (message.failed) {
        notes.push(`${message.failed} could not be moved.`);
      }

      // The list STAYS. It used to be replaced by a "N GB freed" panel, which
      // meant deleting six files cost another full scan of the disk — about a
      // minute — before you could see the rest of the list you were working
      // through. The host says exactly which rows went; taking those out and
      // fixing up the totals is arithmetic, not a rescan.
      removeRows(message.recycled || []);
      $done.innerHTML = `<b>${gigabytes(message.bytes)} freed.</b>
        ${message.binned.toLocaleString('en-US')} in the Recycle Bin, where you can
        still get them back. ${esc(notes.join(' '))}`;
      $done.style.display = 'block';
      updatePicked();
    }
  }
});
</script>
</body>
</html>)FILOHTML";

}  // namespace filo
