/* Klingelbox web UI -- "Die Klingel, lokal und ohne Cloud."
 *
 * The product name is German, and so is one of the two interface languages:
 * English and German, toggled from the header. The ENGLISH STRING IS THE KEY —
 * see the i18n block below, and README.md for how to add a third.
 *
 * Vanilla ES5-ish JavaScript, no framework, no build step, no external assets:
 * this file is flashed into a SPIFFS image on a microcontroller, so every byte
 * is paid for once in flash and again on every page load over a softAP.
 *
 * Four ideas run through the whole file.
 *
 * 0. THE GRAPH IS THE PRODUCT. There are five screens -- Dashboard, Activity,
 *    Settings, Diagnostics, Handbook -- and the Dashboard is the node graph and
 *    nothing else. The live feed has its own tab because a feed under the graph
 *    pushes the graph off a 360 px screen; the written manual has its own tab
 *    because this box can be reached over its own captive portal with no
 *    internet, so the documentation has to ship inside it.
 *
 *    There is still no Signals screen and no Learn screen: a signal is heard,
 *    synthesized, inspected and rebound from inside the node that uses it. The
 *    store itself outlives the graph (deleting a node never deletes a
 *    recording), so the one destructive list lives under Settings.
 *
 * 1. MOBILE FIRST, LIST FIRST. The layout is authored for a 360 px phone. The
 *    node graph in particular is a LIST of cards whose links are tappable
 *    chips; the SVG canvas at >= 900 px is a second view of the same data and
 *    never the only way to do anything. Multi-step flows are bottom sheets.
 *
 * 2. THE API IS THE CONTRACT. Every request in here appears in docs/API.md.
 *    Nothing is invented, and every failure arrives as {"error": "..."} with a
 *    real status, which api() turns into an Error carrying .status.
 *
 * 3. A MISSING FEATURE HIDES, IT DOES NOT BREAK. 404 means "this firmware does
 *    not have that" and 503 means "the hardware is not there". Both disable or
 *    remove the affected control and say why, rather than throwing into a dead
 *    page. A box with an unplugged CC1101 must still be fully navigable --
 *    that is exactly when a user needs the Diagnostics screen.
 *
 * Two readings of the API had to be pinned down; both are noted in README.md:
 * `ts_s` / `last_seen_s` are treated as device-uptime seconds (not epoch --
 * `created_at` is the epoch field), and Wi-Fi passphrases are written to
 * /api/config as `sta.networks[].password`, matching /api/wifi.
 */
(function () {
"use strict";

/* ======================================================================
   Helpers
   ====================================================================== */

function $(sel, root) { return (root || document).querySelector(sel); }
function $$(sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); }

function el(tag, cls, text) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined && text !== null) e.textContent = text;
  return e;
}
function svgEl(tag, cls) {
  var e = document.createElementNS("http://www.w3.org/2000/svg", tag);
  if (cls) e.setAttribute("class", cls);
  return e;
}
function clear(node) { while (node && node.firstChild) node.removeChild(node.firstChild); return node; }
function add(parent) {
  for (var i = 1; i < arguments.length; i++) if (arguments[i]) parent.appendChild(arguments[i]);
  return parent;
}

/* fetch wrapper: resolves with the parsed body, rejects with an Error whose
   .message is the server's `error` string and whose .status is the code. */
function api(path, opts) {
  return fetch(path, opts).then(function (res) {
    return res.json().catch(function () { return {}; }).then(function (body) {
      if (!res.ok) {
        var err = new Error((body && body.error) ? body.error : ("HTTP " + res.status));
        err.status = res.status;
        /* Keep the whole envelope. Some 4xx bodies carry machine-readable
           context next to the sentence (conflict_signal_id, for one), and a
           caller that can act on it should not have to re-parse the prose. */
        err.body = body || {};
        throw err;
      }
      return body || {};
    });
  }, function () {
    var err = new Error(t("No answer from the box (connection lost)."));
    err.status = 0;
    throw err;
  });
}
function postJSON(path, data) {
  return api(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(data || {})
  });
}
function delJSON(path, data) {
  return api(path, {
    method: "DELETE",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(data || {})
  });
}

function setMsg(node, text, kind) {
  if (!node) return;
  node.textContent = text || "";
  node.className = "formmsg" + (kind ? " " + kind : "");
}

function numOr(v, dflt) { return (typeof v === "number" && isFinite(v)) ? v : dflt; }
function intOf(input, dflt) { var v = parseInt(input.value, 10); return isFinite(v) ? v : dflt; }
function trimOf(input) { return (input.value || "").trim(); }

function durText(s) {
  s = Math.max(0, Math.round(s));
  if (s < 60) return s + "s";
  if (s < 3600) return Math.floor(s / 60) + "m " + (s % 60) + "s";
  if (s < 86400) return Math.floor(s / 3600) + "h " + Math.floor((s % 3600) / 60) + "m";
  return Math.floor(s / 86400) + "d " + Math.floor((s % 86400) / 3600) + "h";
}
function shortDur(s) {
  s = Math.max(0, Math.round(s));
  if (s < 60) return s + "s";
  if (s < 3600) return Math.round(s / 60) + "m";
  if (s < 86400) return Math.round(s / 3600) + "h";
  return Math.round(s / 86400) + "d";
}
function fmtHz(hz) {
  if (typeof hz !== "number") return "-";
  if (hz >= 1e6) return (hz / 1e6).toFixed(5).replace(/0+$/, "").replace(/\.$/, "") + " MHz";
  if (hz >= 1e3) return (hz / 1e3).toFixed(1) + " kHz";
  return hz + " Hz";
}
function fmtEpoch(s) {
  if (typeof s !== "number" || s < 1000000000) return null;
  try { return new Date(s * 1000).toLocaleString(); } catch (e) { return null; }
}

/* ======================================================================
   i18n — THE ENGLISH STRING IS THE KEY

   t("Add node") returns the German sentence, or "Add node" itself when the
   dictionary has no entry for it. There is deliberately no `nav.add_node`
   style identifier anywhere, and PLEASE DO NOT "IMPROVE" IT INTO ONE.

   The reasoning, so it does not have to be rediscovered:

   * This is a retrofit onto ~350 KB of finished UI code. Opaque ids would mean
     authoring a second dictionary (id -> English) and keeping it in sync with
     this file forever; English-as-key means there is exactly one dictionary,
     lang-de.js, and it is the only thing a translator ever opens.
   * A missing or misspelt key degrades to correct English. With opaque ids it
     degrades to `nav.add_node` leaking into the interface, which is the worse
     failure by a wide margin on a device whose UI ships in flash and cannot be
     hot-fixed from a server.
   * The source stays readable: el("button", null, t("Add node")) says what
     appears on screen, so a reviewer can see the copy without a lookup.

   The cost is real and accepted: changing an English string orphans its German
   entry (it falls back to the new English, visibly), and two English strings
   that happen to be identical cannot take different German translations. Both
   have been fine in practice; the second has not come up at all.

   INTERPOLATION. Never concatenate a translated fragment with a value —
   German word order differs, so t("Signal ") + id cannot be translated
   properly. Use one key with a {placeholder}:

       t("Signal {id} is still in use", { id: sig.id })

   WHAT IS NOT TRANSLATED, and must never be: node type wire names
   ("logic.switch"), MQTT topics, API field names, GPIO numbers, anything the
   firmware sends back in an {"error": "..."} body (it is authored in C, in
   English, and is closer to log output than to interface copy), and the
   product tagline.

   NEW LANGUAGE: see firmware/webui/README.md — it is one more file and one
   more entry in LANG_DICTS.
   ====================================================================== */

/* Dictionaries are plain objects hung off `window` by their own file, so
   lang-<code>.js stays separately editable and a translation fix is a diff a
   non-programmer can read. English needs no dictionary: it is the keys. */
var LANG_DICTS = { en: null, de: "KLINGELBOX_DE" };
var LANG_NAMES = { en: "English", de: "Deutsch" };
var langCur = "en";

function langDict() {
  var g = LANG_DICTS[langCur];
  return (g && window[g]) || null;
}

function t(s, vars) {
  var d = langDict();
  var out = (d && Object.prototype.hasOwnProperty.call(d, s) && d[s]) || s;
  if (vars) {
    out = out.replace(/\{(\w+)\}/g, function (m, k) {
      return Object.prototype.hasOwnProperty.call(vars, k) ? String(vars[k]) : m;
    });
  }
  return out;
}

/* ======================================================================
   Where updates come from

   THE ONE PLACE A FORK CHANGES ON THIS SIDE. The firmware has its own single
   copy (DB_UPDATE_REPO_SLUG in update_check.h) and serves the resulting URLs as
   config.ota.default_url / default_webui_url, which the UI prefers when it is
   there. These constants are the fallback for a box running a firmware older
   than that field — without them the manual update fields would be empty on
   exactly the boxes most in need of updating.

   `releases/latest/download/<asset>` is a GitHub redirect to the newest
   release's asset, so it never needs a version typed into it.
   ====================================================================== */

var GH_REPO_SLUG = "MarvAmBass/klingelbox";
var GH_RELEASE_ASSETS = "https://github.com/" + GH_REPO_SLUG + "/releases/latest/download/";
var OTA_DEFAULT_APP_URL = GH_RELEASE_ASSETS + "klingelbox.bin";
var OTA_DEFAULT_WEBUI_URL = GH_RELEASE_ASSETS + "storage.bin";

/* ======================================================================
   State
   ====================================================================== */

var S = {
  sys: null,
  radio: null,
  config: null,
  ap: null,
  signals: null,          /* null = not loaded yet */
  signalsErr: null,       /* last /api/signals failure, shown where signals are listed */
  graph: null,
  gpio: null,
  raw: null,              /* last GET /api/raw; also the "have probed" flag */
  diag: null,
  /* Monitor telemetry from GET /api/monitor: { now_s, at, by: { id -> node } }.
     null = never fetched, which is also the "have not probed yet" state. */
  monitor: null,

  events: [],
  serial: -1,             /* /api/events serial; -1 = nothing fetched yet */

  tab: "dashboard",
  recovery: false,
  graphView: "list",
  built: {},              /* tab -> true once its static frame is in the DOM */

  /* feature availability, driven purely by response codes */
  has: { gpio: true, diagnostics: true, radioCfg: true, ap: true, config: true,
         monitor: true, raw: true },
  /* Why transmitting is unavailable. `txBlock` holds the reason as its ENGLISH
     text and is translated on the way out by txBlockText(); a reason authored by
     the firmware is not a dictionary key and so passes through untouched, which
     is exactly what we want for server prose. `txBlockKind` is what the code
     tests against — sniffing the prose ("does it start with 'No CC1101'?")
     silently stopped working the moment that sentence could be German. */
  txBlock: null,
  txBlockKind: null,      /* "radio" = no CC1101, "api" = the box said 409/503 */

  upBase: null,           /* uptime_s at the last /api/system */
  upAt: 0                 /* Date.now() at the last /api/system */
};

/* Smoothed uptime clock: /api/system is polled every 10 s but "12s ago" labels
   need to tick every second, so interpolate locally between samples. */
function uptimeNow() {
  if (S.upBase === null) return null;
  return S.upBase + (Date.now() - S.upAt) / 1000;
}
/* `ts_s` / `last_seen_s` are device-uptime seconds (see the file header). */
function agoText(ts) {
  if (typeof ts !== "number") return "";
  var abs = fmtEpoch(ts);
  if (abs) return abs;
  var up = uptimeNow();
  if (up === null) return "+" + Math.round(ts) + "s";
  return t("{d} ago", { d: shortDur(up - ts) });
}

/* ======================================================================
   Theme toggle: Auto / Light / Dark
   Only "light" or "dark" is ever stored; an absent key means Auto, so the OS
   media query decides. The click cycle starts with the OPPOSITE of what the
   system currently shows, so the very first tap always visibly does something.
   ====================================================================== */
var themeRelabel = null;   /* set below; re-titles the button after a language change */
(function themeToggle() {
  var KEY = "doorbell433-theme";
  var root = document.documentElement;
  var btn = $("#theme-toggle");
  var mql = window.matchMedia ? window.matchMedia("(prefers-color-scheme: dark)") : null;
  function systemDark() { return !!(mql && mql.matches); }
  function stored() {
    var v = null;
    try { v = localStorage.getItem(KEY); } catch (e) { /* private mode */ }
    return (v === "light" || v === "dark") ? v : null;
  }
  function apply(mode) {
    if (mode === "light" || mode === "dark") root.setAttribute("data-theme", mode);
    else root.removeAttribute("data-theme");
    if (!btn) return;
    if (mode === "light") { btn.textContent = "☀️"; btn.title = t("Theme: Light (tap for Dark)"); }
    else if (mode === "dark") { btn.textContent = "🌙"; btn.title = t("Theme: Dark (tap for Auto)"); }
    else { btn.textContent = "🖥"; btn.title = t("Theme: Auto (tap for Light/Dark)"); }
  }
  /* The language toggle initialises after this block and calls this to re-title
     the button, both at boot and on every later switch. */
  themeRelabel = function () { apply(stored()); };
  apply(stored());
  if (btn) btn.addEventListener("click", function () {
    var cur = stored();
    var first = systemDark() ? "light" : "dark";
    var second = systemDark() ? "dark" : "light";
    var next = (cur === null) ? first : (cur === first ? second : null);
    try {
      if (next) localStorage.setItem(KEY, next); else localStorage.removeItem(KEY);
    } catch (e) { /* ignore */ }
    apply(next);
  });
})();

/* ======================================================================
   Language toggle: English / German

   Two languages is a TOGGLE, not a dropdown, and it sits next to the theme
   toggle wearing the same size and shape because it is the same class of
   control: one tap, one tiny persistent preference, no consequences.

   THE BUTTON SHOWS THE LANGUAGE YOU WOULD GET, not the one you have. Reading
   "DE" while looking at an English page is unambiguous; reading "EN" while
   looking at an English page could mean either, and every user would have to
   tap it once to find out.

   First visit with nothing stored follows navigator.language, so a browser set
   to German opens in German — this box is called a Klingelbox.
   ====================================================================== */

/* Static chrome (the tab strips, the footer, aria-labels) lives in index.html
   rather than being built by JS. Marked-up elements carry data-i18n; the
   English original is stashed on first pass so re-translating is not a
   translate-the-translation. */
function translateStatic() {
  $$("[data-i18n]").forEach(function (n) {
    if (n.dataset.i18nEn === undefined) n.dataset.i18nEn = n.textContent;
    n.textContent = t(n.dataset.i18nEn);
  });
  $$("[data-i18n-aria]").forEach(function (n) {
    if (n.dataset.i18nAriaEn === undefined) n.dataset.i18nAriaEn = n.getAttribute("aria-label") || "";
    n.setAttribute("aria-label", t(n.dataset.i18nAriaEn));
  });
  $$("[data-i18n-title]").forEach(function (n) {
    if (n.dataset.i18nTitleEn === undefined) n.dataset.i18nTitleEn = n.getAttribute("title") || "";
    n.setAttribute("title", t(n.dataset.i18nTitleEn));
  });
}

/* Re-render everything the language actually reaches, WITHOUT a page reload.
   What must survive, and how:
     * open sheets — untouched. A sheet is a live flow (a listening session has
       the radio armed); tearing it down mid-flow to relabel it would be a far
       worse bug than a dialog finishing in the language it started in.
     * polling timers — stopTabPolls() then onTabEnter() is the same teardown
       and restart a tab switch performs, so no timer is duplicated or orphaned.
     * the node graph — S.graph, S.signals and the rest of the cache are NOT
       cleared, so the map redraws from the data already in hand: no refetch,
       no flash of an empty screen, and the scroll position is all that moves.
   Only S.built is dropped, which is precisely "the DOM for this tab must be
   built again". */
function rerenderForLang() {
  translateStatic();
  if (themeRelabel) themeRelabel();
  S.built = {};
  $$(".tabpane").forEach(function (p) { if (p.id !== "tab-recovery") clear(p); });
  renderHeader();
  if (S.recovery) { if (S.sys) buildRecovery(S.sys); return; }
  stopTabPolls();
  onTabEnter(S.tab, true);
}

(function langToggle() {
  var KEY = "klingelbox-lang";
  var btn = $("#lang-toggle");

  function stored() {
    var v = null;
    try { v = localStorage.getItem(KEY); } catch (e) { /* private mode */ }
    return Object.prototype.hasOwnProperty.call(LANG_DICTS, v) ? v : null;
  }
  /* navigator.language is "de", "de-DE", "de-AT", "de-CH"… — the primary
     subtag is the only part that decides, and anything that is not German
     lands on English rather than on a language we do not have. */
  function fromBrowser() {
    var l = (navigator.language || (navigator.languages || [])[0] || "en").toLowerCase();
    var primary = l.split("-")[0];
    return Object.prototype.hasOwnProperty.call(LANG_DICTS, primary) ? primary : "en";
  }
  function apply(code, rerender) {
    langCur = code;
    /* <html lang> is not decoration: it is what a screen reader switches voice
       on and what the browser hyphenates by. */
    document.documentElement.setAttribute("lang", code);
    var other = (code === "en") ? "de" : "en";
    if (btn) {
      btn.textContent = other.toUpperCase();
      btn.title = t("Language: {current} (tap for {other})",
                    { current: LANG_NAMES[code], other: LANG_NAMES[other] });
      btn.setAttribute("aria-label", btn.title);
    }
    if (rerender) rerenderForLang();
  }

  apply(stored() || fromBrowser(), false);
  translateStatic();
  if (themeRelabel) themeRelabel();

  if (btn) btn.addEventListener("click", function () {
    var next = (langCur === "en") ? "de" : "en";
    try { localStorage.setItem(KEY, next); } catch (e) { /* ignore */ }
    apply(next, true);
  });
})();

/* ======================================================================
   Polling
   One named timer per job. Timers are torn down on tab change so a phone on
   battery is never polling a screen nobody is looking at, and every tick is
   skipped while the document is hidden (backgrounded tab / locked phone).
   ====================================================================== */
var timers = {};
function poll(name, ms, fn) {
  stopPoll(name);
  fn();
  timers[name] = setInterval(function () { if (!document.hidden) fn(); }, ms);
}
function stopPoll(name) {
  if (timers[name]) { clearInterval(timers[name]); delete timers[name]; }
}
function stopTabPolls() {
  Object.keys(timers).forEach(function (k) { if (k !== "system") stopPoll(k); });
}
document.addEventListener("visibilitychange", function () {
  if (!document.hidden) { loadSystem(); onTabEnter(S.tab, true); }
});

/* ======================================================================
   Sheets (bottom sheet on a phone, centred dialog from 600 px -- CSS decides)
   ====================================================================== */

/* `onClose` runs however the sheet goes away -- the ✕, Escape, or a tap on the
   scrim -- which is what lets a multi-step flow (learn, virtual signal) tear
   down its poll and disarm the box when the user backs out. */
function openSheet(title, sub, onClose) {
  var scrim = el("div", "scrim");
  var sheet = el("div", "sheet");
  sheet.setAttribute("role", "dialog");
  sheet.setAttribute("aria-modal", "true");
  add(sheet, el("div", "sheet-grip"));
  var head = el("div", "sheet-head");
  add(head, el("h3", null, title));
  var x = el("button", "sheet-close", "✕");
  x.type = "button"; x.setAttribute("aria-label", t("Close"));
  add(head, x);
  add(sheet, head);
  if (sub) add(sheet, el("p", "sheet-sub", sub));
  var body = el("div", "sheet-body");
  add(sheet, body);
  add(scrim, sheet);
  document.body.appendChild(scrim);

  var closed = false;
  function close() {
    if (closed) return;
    closed = true;
    scrim.remove();
    document.removeEventListener("keydown", onKey);
    if (onClose) onClose();
  }
  function onKey(e) { if (e.key === "Escape") close(); }
  document.addEventListener("keydown", onKey);
  x.addEventListener("click", close);
  scrim.addEventListener("click", function (e) { if (e.target === scrim) close(); });
  return { scrim: scrim, sheet: sheet, body: body, close: close };
}

function confirmSheet(title, lines, confirmLabel, danger) {
  return new Promise(function (resolve) {
    var sh = openSheet(title);
    (lines || []).forEach(function (line) { add(sh.body, el("p", "small muted", line)); });
    var foot = el("div", "formfoot");
    var yes = el("button", "btn " + (danger ? "danger" : "primary"), confirmLabel || t("Confirm"));
    yes.type = "button";
    var no = el("button", "btn", t("Cancel")); no.type = "button";
    add(foot, yes, no);
    add(sh.body, foot);
    var done = false;
    function finish(v) { if (done) return; done = true; sh.close(); resolve(v); }
    yes.addEventListener("click", function () { finish(true); });
    no.addEventListener("click", function () { finish(false); });
    sh.scrim.addEventListener("click", function (e) { if (e.target === sh.scrim) finish(false); });
    setTimeout(function () { yes.focus(); }, 30);
  });
}

/* A picker is the touch replacement for drag-and-drop and for any long select:
   a full-height list of big tap targets. */
function pickerSheet(title, sub, items, onPick) {
  var sh = openSheet(title, sub);
  if (!items.length) {
    add(sh.body, el("div", "empty", t("Nothing to choose from.")));
    return sh;
  }
  var ul = el("ul", "list");
  items.forEach(function (it) {
    var li = el("li");
    var b = el("button", "listitem"); b.type = "button";
    if (it.icon) add(b, el("span", "li-ico", it.icon));
    var main = el("div", "li-main");
    add(main, el("div", "li-title", it.label));
    if (it.sub) add(main, el("div", "li-sub", it.sub));
    add(b, main);
    if (it.meta) add(b, el("div", "li-meta", it.meta));
    if (it.disabled) b.disabled = true;
    else b.addEventListener("click", function () { sh.close(); onPick(it.value, it); });
    add(li, b);
    add(ul, li);
  });
  add(sh.body, ul);
  return sh;
}

/* ======================================================================
   Form field builders
   ====================================================================== */

function inputEl(type, value, attrs) {
  var i = el("input");
  i.type = type;
  if (value !== undefined && value !== null) i.value = value;
  i.autocomplete = "off";
  i.spellcheck = false;
  if (attrs) Object.keys(attrs).forEach(function (k) { i.setAttribute(k, attrs[k]); });
  return i;
}
function selectEl(options, value) {
  var s = el("select");
  options.forEach(function (o) {
    var op = el("option", null, o.label);
    op.value = String(o.value);
    if (String(o.value) === String(value)) op.selected = true;
    if (o.disabled) op.disabled = true;
    add(s, op);
  });
  return s;
}
function field(labelText, control, hint, cls) {
  var l = el("label", "field" + (cls ? " " + cls : ""));
  add(l, el("span", null, labelText), control);
  if (hint) add(l, el("span", "hint", hint));
  return l;
}
function checkField(labelText, checked, hint) {
  var l = el("label", "field check");
  var i = el("input"); i.type = "checkbox"; i.checked = !!checked;
  add(l, i, el("span", null, labelText));
  var wrap = el("div");
  add(wrap, l);
  if (hint) add(wrap, el("div", "hint", hint));
  wrap.input = i;
  return wrap;
}

/* ======================================================================
   Navigation -- both navigations drive the same panes
   ====================================================================== */

var navButtons = $$(".tab").concat($$(".navbtn"));
navButtons.forEach(function (b) {
  b.addEventListener("click", function () { selectTab(b.dataset.tab); });
});

function selectTab(name) {
  if (S.recovery) return;
  S.tab = name;
  navButtons.forEach(function (b) {
    var on = b.dataset.tab === name;
    b.classList.toggle("active", on);
    b.setAttribute("aria-selected", on ? "true" : "false");
  });
  $$(".tabpane").forEach(function (p) {
    p.classList.toggle("active", p.id === "tab-" + name);
  });
  /* keep the active bottom-nav cell in view on very narrow screens */
  var cell = $('.navbtn[data-tab="' + name + '"]');
  if (cell && cell.scrollIntoView) {
    try { cell.scrollIntoView({ block: "nearest", inline: "nearest" }); } catch (e) { /* old browser */ }
  }
  window.scrollTo(0, 0);
  stopTabPolls();
  onTabEnter(name, false);
}

function onTabEnter(name, resumed) {
  if (S.recovery) return;
  switch (name) {
    case "dashboard":
      buildDashboard();
      loadGraph();
      if (!S.signals || !resumed) loadSignals();
      if (S.has.gpio && !S.gpio) loadGpio();
      if (!S.config) loadConfig();
      /* One probe per page load, before any Monitor node necessarily exists:
         it is what tells a 404 (this firmware has no Monitor node — hide the
         palette entry too) apart from "you have not added one yet". After that
         syncMonitorPoll() decides whether to keep asking. */
      if (S.has.monitor && !S.monitor) loadMonitor();
      /* No `events` timer here any more. The feed lives on its own tab and
         polls from there; the Dashboard is the map and nothing else, so a box
         showing the map costs one /api/graph and the status chips. */
      break;
    case "activity":
      buildActivity();
      /* The feed's own poll, scoped exactly like every other tab poll: started
         on entry, torn down by stopTabPolls() on the way out, and each tick
         skipped while the document is hidden. */
      poll("events", 2000, loadEvents);
      /* Ages are relabelled in place at 1 Hz — no re-render, so it never
         fights the search box for focus. */
      poll("feedclock", 1000, tickFeedClock);
      break;
    case "settings":
      buildSettings();
      /* The release status is the one thing on this (build-once) tab that can
         change on its own; null when the firmware has no update check. */
      if (updateRefresh) updateRefresh();
      /* The stored-signals maintenance list needs both: the store itself, and
         the graph to say which nodes still use each entry. */
      loadSignals();
      if (!S.graph) loadGraph();
      break;
    case "diagnostics":
      buildDiagnostics();
      poll("diag", 5000, loadDiagnostics);
      /* One probe, exactly like the Monitor node's: it is what tells a 404
         (this firmware has no listening sessions — take the button away) apart from
         "no session has been run yet". Not polled: a session that is running
         has its own sheet open, and this panel is only a doorway. */
      if (S.has.raw) loadRaw().then(renderRawPanel).catch(renderRawPanel);
      /* The suggested squelch floor is derived from the live band level, so make
         sure /api/radio has been read at least once before anyone opens it. */
      if (!S.radio) api("/api/radio").then(function (r) { S.radio = r; })
                       .catch(function () { /* the flow falls back to a fixed default */ });
      break;
    case "handbook":
      /* Static prose — nothing to poll. The only live thing on it is the MQTT
         base topic, so fetch the config once if some other tab has not already,
         and rebuild when it lands rather than printing the placeholder. */
      buildHandbook();
      if (!S.config && S.has.config) loadConfig().then(rebuildHandbook);
      break;
  }
}

/* ======================================================================
   Loaders
   ====================================================================== */

function loadSystem() {
  return api("/api/system").then(function (sys) {
    S.sys = sys;
    S.upBase = numOr(sys.uptime_s, null);
    S.upAt = Date.now();
    if (sys.wifi_mode === "recovery" && !S.recovery) { enterRecovery(sys); return sys; }
    renderHeader();
    if (sys.radio && sys.radio.present === false) {
      S.txBlock = "No CC1101 radio detected. Transmitting is impossible until the module answers on SPI -- see Diagnostics.";
      S.txBlockKind = "radio";
    } else if (S.txBlockKind === "radio") {
      S.txBlock = null;
      S.txBlockKind = null;
    }
    return sys;
  }).catch(function (e) {
    renderHeader(e);
    throw e;
  });
}

function loadEvents() {
  return api("/api/events?since=" + (S.serial < 0 ? 0 : S.serial)).then(function (res) {
    var serial = numOr(res.serial, 0);
    var evs = res.events || [];
    /* The serial guard: an unchanged serial means nothing happened, so the
       feed is left alone entirely -- no DOM churn every 2 s. */
    if (serial === S.serial && !evs.length) { return; }
    if (evs.length) {
      S.events = evs.concat(S.events).slice(0, 60);
      /* A press changes seen_count/last_seen on a signal; refresh the store,
         but only when something actually happened. */
      if (evs.some(function (v) { return v.kind === "button_press" || v.kind === "rf_unmatched"; })) {
        loadSignals();
      }
    }
    S.serial = serial;
    renderFeed();
  }).catch(function () { /* transient; the next tick retries */ });
}

function loadSignals() {
  return api("/api/signals").then(function (res) {
    S.signals = res.signals || [];
    S.signalsErr = null;
    onSignalsChanged();
    return S.signals;
  }).catch(function (e) {
    if (S.signals === null) S.signals = [];
    S.signalsErr = e;
    onSignalsChanged();
    return S.signals;
  });
}

function loadGraph() {
  return api("/api/graph").then(function (res) {
    S.graph = { nodes: res.nodes || [], links: res.links || [] };
    renderGraph();
    syncMonitorPoll();
    return S.graph;
  }).catch(function (e) {
    renderGraph(e);
  });
}

/* GET /api/monitor — every Monitor node's recent hits, in ONE call however many
   there are. `now_s` and the hits share the device's uptime clock, so ages come
   out of a subtraction and nothing here needs the box to know the date.

   `at` is the wall-clock instant the sample arrived, used only to advance now_s
   locally between polls: with a hold as short as 1 s, a lamp that only moved on
   the poll edge would visibly stutter or stay lit a beat too long. */
function loadMonitor() {
  return api("/api/monitor").then(function (res) {
    var by = {};
    (res.nodes || []).forEach(function (m) { if (m && m.id) by[m.id] = m; });
    S.monitor = { now_s: numOr(res.now_s, 0), at: Date.now(), by: by };
    refreshMonitors();
    return S.monitor;
  }).catch(function (e) {
    /* 404 = this firmware predates the Monitor node. Hide the feature whole:
       no palette entry, no lamps, no timelines — the same contract every other
       optional endpoint here follows. */
    if ((e.status === 404 || e.status === 501) && S.has.monitor) {
      S.has.monitor = false;
      S.monitor = null;
      stopPoll("monitor");
      renderGraph();
    }
    return null;
  });
}

function monitorNodes() {
  return ((S.graph && S.graph.nodes) || []).filter(function (n) {
    return n.type === "sink.monitor";
  });
}

/* Poll only while the Dashboard is up AND at least one Monitor node exists.
   A box with none must cost nothing, and the graph is what decides that — so
   this is re-evaluated after every loadGraph() rather than set once. poll()
   fires immediately and resets the interval, so an already-running timer is
   left alone. */
function syncMonitorPoll() {
  if (S.tab !== "dashboard" || S.recovery || !S.has.monitor || !monitorNodes().length) {
    stopPoll("monitor");
    return;
  }
  /* 1 s. The hold can be set as low as 1 s, so anything slower could miss a
     lit window completely; the response is a few integers per node; and the
     Dashboard already ticks at 1 Hz for its status chips, so this adds no new
     class of wake-up to a phone that is showing the page anyway. */
  if (!timers.monitor) poll("monitor", 1000, loadMonitor);
  else refreshMonitors();
}

function loadGpio() {
  return api("/api/gpio/available").then(function (res) {
    S.gpio = res;
    return res;
  }).catch(function (e) {
    /* 404 = this firmware has no wired-button support: drop the node type. */
    if (e.status === 404 || e.status === 501) S.has.gpio = false;
    S.gpio = null;
    renderGraph();
  });
}

function loadConfig() {
  return api("/api/config").then(function (cfg) { S.config = cfg; return cfg; })
    .catch(function (e) { if (e.status === 404) S.has.config = false; return null; });
}

/* /api/raw is polled only for as long as a listening sheet is open, from
   inside openListenFlow(). There is no /api/learn any more: registering a
   button and recording raw frames were two shapes of the same job, and the
   narrower one made a whole class of transmitter invisible. */

function loadDiagnostics() {
  return api("/api/diagnostics").then(function (res) {
    S.diag = res;
    renderDiagnostics();
  }).catch(function (e) {
    if (e.status === 404) S.has.diagnostics = false;
    S.diag = null;
    renderDiagnostics(e);
  });
}

/* ======================================================================
   Header
   ====================================================================== */

function renderHeader(err) {
  var badge = $("#status-badge");
  var dot = $("#brand-dot");
  var ver = $("#brand-ver");
  var foot = $("#foot-ver");
  if (!badge) return;

  if (S.recovery) {
    badge.textContent = t("setup mode");
    badge.className = "status-badge warn";
    dot.className = "brand-dot";
    renderStatusChips(err);
    return;
  }
  if (err || !S.sys) {
    badge.textContent = t("offline");
    badge.className = "status-badge bad";
    dot.className = "brand-dot bad";
    renderStatusChips(err);
    return;
  }
  var sys = S.sys;
  if (ver && sys.version) ver.textContent = "v" + sys.version;
  if (foot && sys.version) foot.textContent = " · v" + sys.version;

  var radioOk = !(sys.radio && sys.radio.present === false);
  if (!radioOk) {
    badge.textContent = t("no radio");
    badge.className = "status-badge bad";
    dot.className = "brand-dot bad";
  } else if (!sys.sta_connected) {
    badge.textContent = sys.ap_ssid ? t("AP only") : t("no Wi-Fi");
    badge.className = "status-badge warn";
    dot.className = "brand-dot";
  } else {
    badge.textContent = t("ready");
    badge.className = "status-badge ok";
    dot.className = "brand-dot ok";
  }
  badge.title = (sys.sta_connected ? t("Wi-Fi: {ssid} @ {ip}",
                                       { ssid: (sys.sta_ssid || "?"), ip: (sys.sta_ip || "?") })
                                   : t("Not on a home network"))
    + (radioOk ? "" : t(" -- CC1101 not detected"));
  /* Below 600 px the badge is hidden and this dot IS the badge — same three
     states, same class names — so it carries the same explanation. */
  if (dot) dot.title = t("{state} — {detail}", { state: badge.textContent, detail: badge.title });

  /* Both live off the same /api/system reading, so both are refreshed by the
     one call that produced it — the header chips everywhere, and the two
     blocking warnings if the Dashboard happens to be built. */
  renderStatusChips();
  renderDashNotes();
}

/* ======================================================================
   Transmit -- one path, one place where 409/503 is turned into a reason
   ====================================================================== */

function txAvailable() { return !S.txBlock; }

/* The stored reason, in the current language. Our own reasons are English
   dictionary keys and come back translated; a firmware-authored reason is not a
   key, so t() returns it verbatim. Reading it here rather than translating at
   assignment is what makes the note change language on the toggle instead of at
   the next /api/system, ten seconds later. */
function txBlockText() { return S.txBlock ? t(S.txBlock) : ""; }

/* `opts` is optional and exists so the pairing burst (a longer repeat train
   with its own wording) still goes through THIS function rather than growing a
   second transmit path that would have to re-learn what a 409/503 means:
     .body    extra transmit parameters, e.g. { repeats: 20, gap_us: 8000 }
     .sending / .sent / .ok   wording for a non-default use */
function transmit(signalId, btn, msgNode, opts) {
  opts = opts || {};
  if (!txAvailable()) { if (msgNode) setMsg(msgNode, txBlockText(), "err"); return Promise.resolve(); }
  var old = btn ? btn.textContent : null;
  if (btn) { btn.disabled = true; btn.textContent = opts.sending || t("Sending…"); }
  if (msgNode) setMsg(msgNode, "");
  return postJSON("/api/signals/" + signalId + "/transmit", opts.body || {}).then(function () {
    if (btn) { btn.textContent = opts.sent || t("Sent ✓"); setTimeout(function () { btn.textContent = old; btn.disabled = false; }, 1400); }
    if (msgNode) setMsg(msgNode, opts.ok || t("Transmitted. This only confirms the pulses left the radio -- it cannot know a receiver reacted."), "ok");
  }).catch(function (e) {
    if (btn) { btn.textContent = old; btn.disabled = false; }
    if (e.status === 503 || e.status === 409) {
      S.txBlock = e.message || "The radio is unavailable, so nothing can be transmitted.";
      S.txBlockKind = "api";
      renderTxNote();
    }
    if (msgNode) setMsg(msgNode, e.message, "err");
  });
}

function txBlockNote() {
  if (txAvailable()) return null;
  return el("div", "note bad", txBlockText());
}

/* ======================================================================
   Waveform plot -- durations_us drawn as a HIGH/LOW square wave
   ====================================================================== */

/* `sel` is optional: { from, to } as zero-based pulse indices, `from` inclusive
   and `to` exclusive (Array.slice semantics, which is also what the raw-capture
   API takes). When present the whole frame is still drawn -- you cannot judge a
   trim without seeing what you are cutting away -- and the selected span is
   shaded, with the caption describing the SELECTION rather than the frame. */
function waveform(durations, firstLevel, sel) {
  if (!durations || !durations.length) return null;
  var MAXP = 800;
  var n = Math.min(durations.length, MAXP);
  var total = 0, i;
  for (i = 0; i < n; i++) total += Math.max(0, durations[i] || 0);
  if (!total) return null;

  var W = 1000, HI = 7, LO = 33, H = 40;
  var lvl = firstLevel ? 1 : 0;
  var x = 0;
  var xs = [0];
  var d = "M0," + (lvl ? HI : LO);
  for (i = 0; i < n; i++) {
    x += (durations[i] || 0) * W / total;
    xs.push(x);
    d += "H" + x.toFixed(2);
    lvl = lvl ? 0 : 1;
    d += "V" + (lvl ? HI : LO);
  }

  var wrap = el("div", "wave-wrap");
  var svg = svgEl("svg", "wave");
  svg.setAttribute("viewBox", "0 0 " + W + " " + H);
  svg.setAttribute("preserveAspectRatio", "none");
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", t("{n} pulses over {ms} milliseconds",
                                   { n: n, ms: Math.round(total / 1000) }));
  var mid = svgEl("path", "mid");
  mid.setAttribute("d", "M0,20 H" + W);
  add(svg, mid);

  var from = 0, to = n, selUs = 0;
  if (sel) {
    from = Math.max(0, Math.min(n, numOr(sel.from, 0)));
    to = Math.max(from, Math.min(n, numOr(sel.to, n)));
    for (i = from; i < to; i++) selUs += Math.max(0, durations[i] || 0);
    var band = svgEl("rect", "wave-sel");
    band.setAttribute("x", xs[from].toFixed(2));
    band.setAttribute("y", "0");
    band.setAttribute("width", Math.max(0, xs[to] - xs[from]).toFixed(2));
    band.setAttribute("height", String(H));
    add(svg, band);
  }

  var p = svgEl("path");
  p.setAttribute("d", d);
  add(svg, p);
  /* Long frames get a real pixel width and scroll horizontally instead of
     being squeezed into an unreadable comb on a phone. */
  if (n > 110) svg.style.width = Math.round(n * 7) + "px";
  add(wrap, svg);

  var box = el("div");
  add(box, wrap);
  add(box, el("div", "wave-cap", sel
    ? t("{sel} of {n} pulses selected · {ms} ms · starts {lvl}",
        { sel: (to - from), n: n, ms: (selUs / 1000).toFixed(1),
          lvl: ((firstLevel ^ (from & 1)) ? "HIGH" : "LOW") })
    : t("{n} pulses · {ms} ms · starts {lvl}",
        { n: (durations.length > n ? t("{n} of {total}", { n: n, total: durations.length }) : n),
          ms: (total / 1000).toFixed(1), lvl: (firstLevel ? "HIGH" : "LOW") })));
  box.svg = svg;
  return box;
}

/* ======================================================================
   SIGNALS -- reached THROUGH nodes, never beside them

   There is no Signals screen and no Learn screen any more. A signal is not a
   thing you keep a list of; it is what a node listens for or transmits, so
   everything a signal needs happens where the signal is used:

     * a node that needs one is CREATED through the choice of that signal --
       hear it off the air, pick one already stored, or synthesize a virtual
       one -- in a single flow with a single confirmation,
     * a node that HAS one shows it inline in its editor: decode, waveform,
       pairing, transmit-to-test, rename, listen again, swap.

   THE STORE IS NOT THE GRAPH. A learned signal is a recording of a physical
   remote; the graph is only wiring. Deleting a node, unlinking it or rebinding
   it to a different signal therefore NEVER touches the store -- the waveform
   stays, under its name, and can be picked again tomorrow. Rewiring must never
   cost a walk to the front door with a remote in hand. Removing a signal for
   good is a separate, deliberate act and lives in exactly one place:
   Settings -> Stored signals (sectionSignals). Nothing in this file deletes a
   signal as a side effect of graph editing.
   ====================================================================== */

function signalById(id) {
  var list = S.signals || [];
  for (var i = 0; i < list.length; i++) if (list[i].id === id) return list[i];
  return null;
}
function signalLabel(sig) {
  if (!sig) return t("signal");
  return sig.name || t("Signal {id}", { id: sig.id });
}
/* One line of identity: the decoder's sentence when there is one, otherwise the
   honest "raw pulses" wording -- an undecoded signal is a supported state. */
function signalIdent(sig) {
  if (sig && sig.decoded && sig.decoded.text) return sig.decoded.text;
  return t("raw waveform, {count} pulses", { count: numOr(sig && sig.pulse_count, 0) });
}
/* Which nodes point at a signal. Drives both the "used by" marks in the picker
   and the delete guard: a signal another node still needs is not deletable. */
function nodesUsingSignal(id, exceptNodeId) {
  var out = [];
  ((S.graph && S.graph.nodes) || []).forEach(function (n) {
    if (exceptNodeId && n.id === exceptNodeId) return;
    if (numOr(n.signal_id, 0) === id) out.push(n);
  });
  return out;
}
function usedByText(users) {
  if (!users.length) return t("Not used by any node");
  return t("Used by {nodes}", { nodes: users.map(function (n) { return nodeName(n.id); }).join(", ") });
}

/* Anything that re-reads /api/signals lands here: node summaries carry signal
   names, an open picker is a live view of the same store, and so is the
   maintenance list under Settings. */
function onSignalsChanged() {
  if (S.built.dashboard && S.graph) renderGraph();
  if (pickerRefresh) pickerRefresh();
  if (settingsSigRender) settingsSigRender();
}

/*
 * The pairing panel of a SYNTHESIZED signal.
 *
 * Written after a real report of "the virtual signal is broken". It was not:
 * a synthesized signal is a brand-new address, so no receiver on the band
 * answers to it until one has been TAUGHT it, and the user's chime was paired
 * to a different address entirely. The explanation existed only on the
 * creation form, which is the one screen you are no longer looking at by the
 * time you tap Transmit and nothing happens. It now sits in the node editor,
 * which is exactly where someone goes when the chime stays silent.
 *
 * The order of the steps is the whole feature: the receiver has to be listening
 * BEFORE the code goes out, and almost everyone tries it the other way round.
 */
var PAIR_REPEATS = 20;    /* ~20 x 36 ms ≈ 0.7 s: long enough that a receiver
                             that just entered learning mode cannot miss it,
                             short enough to stay an inline request like every
                             other transmit. */
var PAIR_GAP_US = 8000;

function pairPanel(sig) {
  var box = el("div", "note");
  add(box, el("b", null, t("Pair with a receiver")));

  var steps = el("ol");
  steps.style.margin = ".45rem 0 .55rem";
  steps.style.paddingLeft = "1.25rem";

  var s1 = el("li");
  add(s1, el("span", null, t("Put your receiver into ")), el("b", null, t("learning mode")),
      el("span", null, t(" — usually hold its button until it beeps or its LED blinks.")));
  var s2 = el("li");
  add(s2, el("span", null, t("Tap ")), el("b", null, t("Pair now")),
      el("span", null, t(" below within a few seconds.")));
  var s3 = el("li", null, t("The receiver stores this code and rings for it from then on."));
  add(steps, s1, s2, s3);
  add(box, steps);

  add(box, el("div", "small muted",
    t("This code is new — nothing responds to it until a receiver has learned it. " +
      "That is expected.")));

  var msg = el("div", "formmsg");
  var row = el("div", "formfoot");
  var b = el("button", "btn primary", t("🔗 Pair now"));
  b.type = "button";
  b.disabled = !txAvailable();
  if (!txAvailable()) b.title = txBlockText();
  b.addEventListener("click", function () {
    transmit(sig.id, b, msg, {
      body: { repeats: PAIR_REPEATS, gap_us: PAIR_GAP_US },
      sending: t("Pairing…"),
      sent: t("Sent ✓"),
      ok: t("Code sent {count} times. If the receiver was in learning mode it " +
            "has stored it — test it with Transmit below. If not, put it back into learning " +
            "mode and tap Pair now again.", { count: PAIR_REPEATS })
    });
  });
  add(row, b, msg);
  add(box, row);
  return box;
}

/* ----------------------------------------------------------------------
   signalBlock -- the whole of the old Signals detail view, as a fragment
   dropped inline into the editor of the node that uses the signal.

   There is deliberately no delete here: this block is reached while editing
   wiring, and wiring must not be able to destroy a recording. See the section
   header, and Settings -> Stored signals for the one place that can.

   opts:
     .node       the node this signal belongs to, when there is one
     .onChanged  fn(what, value) -- "renamed"
   ---------------------------------------------------------------------- */
function signalBlock(sig, opts) {
  opts = opts || {};
  var box = el("div", "sigblock");

  /* identity */
  var chips = el("div", "chiprow");
  if (sig.decoded && sig.decoded.text) add(chips, el("span", "chip accent mono", sig.decoded.text));
  else add(chips, el("span", "chip warn", t("Unknown protocol — stored as raw pulses")));
  if (sig.origin) add(chips, el("span", "chip", sig.origin));
  if (typeof sig.seen_count === "number") add(chips, el("span", "chip", t("seen {count}x", { count: sig.seen_count })));
  add(box, chips);

  if (!sig.decoded) {
    add(box, el("div", "note",
      t("No decoder claimed this waveform. That is a fully supported state: the exact pulse " +
        "timings are stored and replay works normally. Only the human-readable identity is missing.")));
  }

  /* Above every other control, because it is the answer to the question a
     user has already formed by the time they get here.

     It does NOT fork on the node's direction, and deliberately so. Pairing
     teaches one of the user's own receivers this code, and it does that through
     POST /api/signals/{id}/transmit -- the SIGNAL's own transmit, not the
     node's. So it is just as applicable in a Signal receiver's editor ("make my
     chime answer the same code my doorbell sends") as in a sender's, and an
     earlier version that hid it on listen-only nodes was hiding a thing that
     worked. */
  if (sig.origin === "synthesized") add(box, pairPanel(sig));

  /* confidence meter */
  if (typeof sig.confidence === "number") {
    var cwrap = el("div");
    cwrap.style.margin = ".7rem 0";
    var crow = el("div", "row");
    crow.style.justifyContent = "space-between";
    add(crow, el("span", "small muted", t("Decode confidence")));
    add(crow, el("span", "small mono", sig.confidence + "%"));
    add(cwrap, crow);
    var m = el("div", "meter " + (sig.confidence >= 65 ? "ok" : sig.confidence >= 45 ? "warn" : "bad"));
    var fill = el("i");
    fill.style.width = Math.max(2, Math.min(100, sig.confidence)) + "%";
    add(m, fill);
    add(cwrap, m);
    add(cwrap, el("div", "hint",
      t("Measured on this bench: real presses score 67-92%, band noise 24-48%.")));
    add(box, cwrap);
  }

  /* facts */
  var kv = el("dl", "kv");
  function kvAdd(k, v) { if (v === null || v === undefined || v === "") return; add(kv, el("dt", null, k)); add(kv, el("dd", "mono", String(v))); }
  kvAdd(t("Id"), sig.id);
  kvAdd(t("Fingerprint"), sig.fingerprint);
  kvAdd(t("Base pulse"), typeof sig.base_us === "number" ? sig.base_us + " us" : null);
  kvAdd(t("Pulses"), sig.pulse_count);
  kvAdd("RSSI", typeof sig.rssi_dbm === "number" ? sig.rssi_dbm + " dBm" : null);
  kvAdd(t("Seen"), typeof sig.seen_count === "number" ? t("{count} times", { count: sig.seen_count }) : null);
  kvAdd(t("Last seen"), typeof sig.last_seen_s === "number" ? agoText(sig.last_seen_s) : null);
  kvAdd(t("Created"), fmtEpoch(sig.created_at) || null);
  if (sig.decoded) {
    kvAdd(t("Protocol"), sig.decoded.protocol);
    kvAdd(t("Address"), typeof sig.decoded.id === "number"
      ? sig.decoded.id + " (0x" + sig.decoded.id.toString(16).toUpperCase() + ")" : null);
    kvAdd(t("Button"), sig.decoded.button);
  }
  add(box, kv);

  /* waveform */
  var wf = waveform(sig.durations_us, sig.first_level);
  if (wf) {
    var wh = el("div", "field");
    add(wh, el("span", null, t("Pulse train")));
    add(wh, wf);
    add(wh, el("span", "hint",
      t("High = carrier on, low = carrier off. This is what gets replayed, verbatim, on transmit.")));
    /* Why a virtual signal does not look like a captured one. Read as a bug
       otherwise -- and it has been. */
    if (sig.origin === "synthesized") {
      add(wh, el("span", "hint",
        t("A synthesized frame ends with the ~9 ms sync gap the protocol puts between words, " +
          "so it has one pulse more than the same code captured off the air (50 vs 49). A " +
          "capture can never contain that gap: it is longer than the 8 ms idle threshold, so it " +
          "is exactly what ENDS the recording. Repeated on air the two are the same waveform.")));
    }
    add(box, wh);
  }

  /* rename */
  var nameIn = inputEl("text", sig.name || "", { maxlength: "40" });
  add(box, field(t("Signal name"), nameIn,
    t("Shown in the activity feed, on the node card and in the picker.")));

  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");

  var txb = el("button", "btn primary", t("📡 Transmit"));
  txb.type = "button";
  txb.disabled = !txAvailable();
  if (!txAvailable()) txb.title = txBlockText();
  txb.addEventListener("click", function () { transmit(sig.id, txb, msg); });

  /* Export just this one signal.
     Deliberately the SAME bundle shape as a full backup, with one signal and an
     empty graph -- so it imports through the identical path with no special
     case. A one-signal file is simply a small backup, which is also why it can
     be merged into a box that already has automations without touching them. */
  var exp = el("button", "btn", t("\u2b07 Export"));
  exp.type = "button";
  exp.title = t("Download this one signal as a .json you can import on another Klingelbox");
  exp.addEventListener("click", function () {
    exp.disabled = true;
    setMsg(msg, t("Reading the waveform\u2026"));
    Promise.all([api("/api/system"), api("/api/signals/" + sig.id)])
      .then(function (r) {
        var sysm = r[0], full = r[1];
        var bundle = {
          kind: BACKUP_KIND,
          version: BACKUP_VERSION,
          exported_at: Math.floor(Date.now() / 1000),
          device: { hostname: sysm.hostname || "", version: sysm.version || "", idf: sysm.idf || "" },
          signals: [{
            id: full.id, name: full.name, origin: full.origin,
            first_level: full.first_level, durations_us: full.durations_us || []
          }],
          graph: { nodes: [], links: [] }
        };
        var slug = String(full.name || ("signal-" + full.id))
                     .toLowerCase().replace(/[^a-z0-9-]+/g, "-").replace(/^-|-$/g, "");
        var bytes = downloadBundle(bundle, "klingelbox-signal-" + (slug || full.id) + ".json");
        exp.disabled = false;
        setMsg(msg, t("Exported {count} pulses ({kb} KB). Import it on another box from " +
                      "Settings \u2192 Backup.",
                    { count: (full.durations_us || []).length, kb: Math.round(bytes / 1024) }), "ok");
      })
      .catch(function (e) { exp.disabled = false; setMsg(msg, e.message, "err"); });
  });

  var save = el("button", "btn", t("Save name"));
  save.type = "button";
  save.addEventListener("click", function () {
    var n = trimOf(nameIn);
    if (!n) { setMsg(msg, t("A name cannot be empty."), "err"); return; }
    save.disabled = true;
    setMsg(msg, t("Saving…"));
    postJSON("/api/signals/" + sig.id, { name: n }).then(function () {
      save.disabled = false;
      sig.name = n;
      setMsg(msg, t("Renamed."), "ok");
      loadSignals();
      if (S.graph) loadGraph();
      if (opts.onChanged) opts.onChanged("renamed", n);
    }).catch(function (e) { save.disabled = false; setMsg(msg, e.message, "err"); });
  });

  add(foot, txb, save, exp, msg);
  add(box, foot);

  /* Suppressed in Settings -> Stored signals, where the note would point at the
     very sheet you are reading and Delete is right underneath it anyway. */
  if (opts.storeNote !== false) {
    var others = nodesUsingSignal(sig.id, opts.node ? opts.node.id : 0);
    add(box, el("div", "note",
      t("This recording belongs to the box, not to this node. Deleting the node, unlinking it or " +
        "pointing it at a different signal leaves it in the store under its name — you never have " +
        "to walk to the door and register a button twice.") +
      (others.length ? " " + t("{used} as well.", { used: usedByText(others) }) : "") +
      " " + t("To remove it for good: Settings → Stored signals.")));
  }
  return box;
}

/* ----------------------------------------------------------------------
   The signal picker -- choose which stored waveform a node uses.

   Purely constructive: it picks, and it offers the two ways of getting a
   signal that is not in the list yet. It cannot delete anything, because it
   is reached while wiring and wiring must never destroy a recording (see the
   section header). "Used by" is shown only because it helps you choose.

   opts:
     .title        wording for the calling context
     .onPick       fn(signal)
     .node         the node being configured
     .onListen     optional fn() -- offered as "listen for one instead"
     .onVirtual    optional fn() -- offered as "create one instead"
   ---------------------------------------------------------------------- */
var pickerRefresh = null;   /* set while a picker sheet is open */

function openSignalPicker(opts) {
  opts = opts || {};
  var sh = openSheet(opts.title || t("Choose a signal"),
    t("Everything this box has stored. Signals already used by a node are marked — you can " +
      "still pick one, two nodes may share a signal."),
    function () { pickerRefresh = null; });

  var listWrap = el("div");
  add(sh.body, listWrap);

  function alts(where) {
    if (!opts.onListen && !opts.onVirtual) return;
    var row = el("div", "btnrow");
    row.style.marginTop = ".7rem";
    if (opts.onListen) {
      var lb = el("button", "btn", t("🎧 Listen for a new button"));
      lb.type = "button";
      lb.addEventListener("click", function () { sh.close(); opts.onListen(); });
      add(row, lb);
    }
    if (opts.onVirtual) {
      var vb = el("button", "btn", t("✨ Configure by hand"));
      vb.type = "button";
      vb.addEventListener("click", function () { sh.close(); opts.onVirtual(); });
      add(row, vb);
    }
    add(where, row);
  }

  function render() {
    clear(listWrap);
    if (S.signalsErr) {
      add(listWrap, el("div", "note bad",
        t("Could not read the signal store: {error}", { error: S.signalsErr.message })));
    }

    var list = S.signals || [];
    if (!list.length) {
      add(listWrap, el("div", "empty", t("No signals stored yet.")));
      alts(listWrap);
      return;
    }

    var ul = el("ul", "list");
    list.forEach(function (sig) {
      var users = nodesUsingSignal(sig.id, 0);
      var li = el("li");
      var b = el("button", "listitem"); b.type = "button";
      add(b, el("span", "li-ico", sig.origin === "synthesized" ? "✨" : "📥"));
      var main = el("div", "li-main");
      add(main, el("div", "li-title", signalLabel(sig)));
      var subParts = [signalIdent(sig)];
      if (typeof sig.base_us === "number") subParts.push(t("{base} us base", { base: sig.base_us }));
      add(main, el("div", "li-sub", subParts.join("  ·  ")));
      add(main, el("div", "li-sub", users.length ? usedByText(users) : t("Not used by any node")));
      add(b, main);
      var meta = el("div", "li-meta");
      if (typeof sig.seen_count === "number") add(meta, el("div", null, sig.seen_count + "x"));
      if (typeof sig.last_seen_s === "number") add(meta, el("div", null, agoText(sig.last_seen_s)));
      add(b, meta);
      b.addEventListener("click", function () { sh.close(); opts.onPick(sig); });
      add(li, b);
      add(ul, li);
    });
    add(listWrap, ul);
    alts(listWrap);
  }

  pickerRefresh = render;
  render();
  /* Cheap and worth it: the store may have changed since this tab was entered. */
  loadSignals();
  return sh;
}

/* ----------------------------------------------------------------------
   Creating a virtual signal, as a flow. Same copy as the old Signals screen
   carried, because it is the copy that explains why the thing exists at all.
   Resolves with the created signal.
   ---------------------------------------------------------------------- */
/* An EV1527 address is 20 bits and is SHOWN as hex everywhere in this UI
   ("EV1527 id=0xA685A btn=0x8"), so it must be accepted as hex too. Rules:
     "0xA685A" / "A685A"  -> hex   (any string containing a-f is unambiguous)
     "681562"             -> decimal (plain digits read as the human wrote them)
     ""                   -> blank; the firmware draws a random address
   Anything else is rejected with a sentence rather than silently clamped -- a
   quietly truncated address produces a signal that simply never matches. */
var ID20_MAX = 0xFFFFF;

function parseId20(raw) {
  var txt = (raw || "").trim().replace(/\s+/g, "");
  if (!txt) return { ok: true, blank: true, value: 0 };
  var v = NaN;
  if (/^0x[0-9a-f]+$/i.test(txt)) v = parseInt(txt.slice(2), 16);
  else if (/^[0-9]+$/.test(txt)) v = parseInt(txt, 10);
  else if (/^[0-9a-f]+$/i.test(txt)) v = parseInt(txt, 16);
  else return { ok: false, msg: t("Use hex (0xA685A or A685A) or plain digits — nothing else.") };
  if (!isFinite(v) || v < 0) return { ok: false, msg: t("That is not a number.") };
  if (v > ID20_MAX) {
    return { ok: false, msg: t("An EV1527 address is 20 bits: 0 to 0xFFFFF (1048575). " +
      "0x{hex} is too large.", { hex: v.toString(16).toUpperCase() }) };
  }
  /* 0 is the API's "pick one for me" sentinel, so it can never be a real
     address -- say so instead of creating something that decodes as random. */
  return { ok: true, blank: v === 0, value: v };
}
function hex20(v) { return "0x" + v.toString(16).toUpperCase(); }
function ev1527Text(id20, button) { return "EV1527 id=" + hex20(id20) + " btn=" + hex20(button); }

/* ----------------------------------------------------------------------
   Hand-crafting a signal, as a flow. Reached as the third option ("Configure
   by hand") both from a Signal node's editor and from the stored-signals list.

   The copy used to fork on direction, because a source node could only listen
   and a transmit sink could only send, and the same made-up code therefore
   meant two different things depending on which end you were standing at. The
   node type now says which end it is, so this sheet no longer has to guess:
   it explains what a hand-entered code IS, and the node's own editor says what
   that node will do with it. The sentence that had to survive from the old
   source copy is the one about nothing happening on its own -- without it a
   user invents a code, hears silence, and concludes the box is broken.

   opts.mode: "signal" (a code for a graph node, either direction) | "sink" (the stored
   signals list, where there is no node and pairing is the whole point).
   Default "sink". Resolves with the created signal.
   ---------------------------------------------------------------------- */
function openVirtualFlow(opts) {
  opts = opts || {};
  var isNode = opts.mode === "signal";
  return new Promise(function (resolve) {
    var done = false;
    var sh = openSheet(t("Enter a code"),
      isNode
        ? t("A code for this node to use.")
        : t("A brand-new code, so you can pair your own receivers to this box."),
      function () { if (!done) { done = true; resolve(null); } });

    if (isNode) {
      add(sh.body, el("p", "small",
        t("Use this when you already KNOW the code — from another Klingelbox, from a remote you " +
          "decoded elsewhere — or when you are inventing a fresh one for a receiver of your own. " +
          "If the remote is in your hand, Learn is easier and cannot get a digit wrong.")));
      add(sh.body, el("div", "note warn",
        t("A code on its own does nothing. A Signal sender puts it on air when something " +
          "triggers it — which is also how you pair a chime, relay or socket to it. A Signal " +
          "receiver stays quiet until that code is actually heard on air.")));
    } else {
      add(sh.body, el("p", "small",
        t("A virtual signal is a brand-new EV1527 code that no remote in the world is using yet. " +
          "It exists so you can pair YOUR OWN receivers to this box: put a plug-in chime, a relay " +
          "or a socket into its learning mode, then transmit this signal. From then on the receiver " +
          "obeys this box, and any node can ring it.")));
    }

    var grid = el("div", "formgrid");
    var vName = inputEl("text", isNode ? t("Hand-entered code") : t("Virtual chime 1"),
      { maxlength: "40", placeholder: isNode ? t("Hand-entered code") : t("Virtual chime 1") });
    var vBtn = selectEl([1, 2, 4, 8].map(function (b) { return { value: b, label: t("Button {n}", { n: b }) }; }), 8);
    var vBase = inputEl("number", "350", { min: "100", max: "1500", step: "10", inputmode: "numeric" });
    /* text, not number: "0xA685A" is the form this UI displays everywhere. */
    var vId = inputEl("text", "", { maxlength: "12", placeholder: t("0xA685A or blank"),
      autocapitalize: "off", autocorrect: "off" });

    var idRow = el("div", "row");
    idRow.style.gap = ".4rem";
    vId.style.flex = "1 1 8rem";
    var rnd = el("button", "btn small", t("🎲 Randomize"));
    rnd.type = "button";
    rnd.style.minHeight = "2.75rem";
    rnd.style.flex = "0 0 auto";
    add(idRow, vId, rnd);

    var idField = field(t("20-bit address"), idRow,
      t("Hex (0xA685A or A685A) or plain digits. Leave it blank — or tap Randomize — and the box " +
        "picks a free address itself."), "full");

    /* The preview is what makes hand-entry checkable: it is formatted exactly
       like the decoded identity shown on every signal elsewhere in the UI. */
    var preview = el("div", "hint mono");
    add(idField, preview);

    add(grid,
      field(t("Name"), vName, null, "full"),
      field(t("Button code"), vBtn, t("The 4-bit button nibble sent with the address.")),
      field(t("Base pulse (us)"), vBase, t("350 us suits most EV1527 receivers.")),
      idField);
    add(sh.body, grid);

    var msg = el("div", "formmsg");

    function syncPreview() {
      var r = parseId20(vId.value);
      var btn = intOf(vBtn, 8);
      if (!r.ok) {
        preview.textContent = r.msg;
        preview.className = "hint mono bad-text";
        return r;
      }
      preview.className = "hint mono";
      preview.textContent = r.blank
        ? t("EV1527 id=0x????? btn={btn}   — a random address, chosen by the box", { btn: hex20(btn) })
        : ev1527Text(r.value, btn);
      return r;
    }
    vId.addEventListener("input", syncPreview);
    vBtn.addEventListener("change", syncPreview);
    rnd.addEventListener("click", function () {
      /* 1..0xFFFFF: never 0, which the API reads as "choose for me". */
      vId.value = hex20(1 + Math.floor(Math.random() * ID20_MAX));
      syncPreview();
      setMsg(msg, "");
    });
    syncPreview();

    var foot = el("div", "formfoot");
    var save = el("button", "btn primary", isNode ? t("Use this code") : t("Create virtual signal"));
    save.type = "button";
    /* The duplicate-address override.
       The API refuses a code another signal already carries, because the two
       cannot be told apart when one is RECEIVED. That is worth a stop, not a
       wall: re-creating a code you already captured is a legitimate thing to
       want — it is how you check that the box can GENERATE a code it can
       currently only REPLAY. So the refusal turns into a second button rather
       than a dead end, and the button stays hidden until there has actually
       been a refusal: nobody should be offered "create a duplicate" before
       there is one to duplicate. */
    var anyway = el("button", "btn", t("Create it anyway"));
    anyway.type = "button";
    anyway.style.display = "none";
    var cancel = el("button", "btn", t("Cancel"));
    cancel.type = "button";
    cancel.addEventListener("click", sh.close);
    add(foot, save, anyway, cancel, msg);
    add(sh.body, foot);

    add(sh.body, el("div", "note", isNode
      ? t("The node is created carrying exactly this code. It gets a “Pair with a receiver” " +
          "panel for teaching your chime the code, and anything sending that code over the air " +
          "shows up on the Activity tab.")
      : t("Next: the signal you are creating gets a “Pair with a receiver” panel. Put your receiver " +
          "into pairing mode and tap Pair now there — the receiver stores this code and answers to " +
          "it from then on.")));

    function submit(allowDuplicate) {
      var r = syncPreview();
      if (!r.ok) { setMsg(msg, r.msg, "err"); vId.focus(); return; }
      var body = {
        name: trimOf(vName) || (isNode ? t("Hand-entered code") : t("Virtual signal")),
        button: intOf(vBtn, 8),
        base_us: intOf(vBase, 350),
        id20: r.blank ? 0 : r.value
      };
      if (allowDuplicate) body.allow_duplicate = true;
      save.disabled = true;
      anyway.disabled = true;
      setMsg(msg, t("Creating…"));
      postJSON("/api/signals/virtual", body).then(function (created) {
        return loadSignals().then(function (list) {
          var made = (created && created.id) ? (signalById(created.id) || created)
                                             : list[list.length - 1];
          done = true;
          sh.close();
          resolve(made || null);
        });
      }).catch(function (e) {
        save.disabled = false;
        anyway.disabled = false;
        setMsg(msg, e.message, "err");
        if (e.status === 409 && e.body && e.body.conflict_signal_id)
          anyway.style.display = "";
      });
    }

    save.addEventListener("click", function () { submit(false); });
    anyway.addEventListener("click", function () { submit(true); });
  });
}

/* ======================================================================
   LISTENING -- the one way a button is registered

   THERE USED TO BE TWO OF THESE, AND THAT WAS THE BUG. "Learn mode" armed the
   receiver and waited for a burst that repeated at least twice AND normalised
   to at least 65 % confidence; raw capture recorded everything and let you cut
   a signal out by hand. Both thresholds were measured on EV1527 remotes, so
   learn mode was, in effect, a mode for one protocol family. A transmitter of
   any other shape never produced a candidate at all -- the countdown ran, the
   screen stayed empty, and the box said nothing about why. Meanwhile raw
   capture, which was offered as the escape hatch for exactly that case, picked
   the same remote up without trouble.

   So the gate is gone and the two flows are one. Detection is now permissive
   and protocol-agnostic: every frame the radio hands up is kept. What the box
   does instead is RANK, using evidence that needs no protocol knowledge --

     * a real remote repeats itself, and noise does not, so "seen N times" is
       the strongest signal of authenticity available and it dominates the
       order;
     * a decoded protocol and a clean base-width estimate raise a candidate
       further, but only ever as a tie-break. An undecoded candidate is
       first-class, fully usable, and can sit at the top of the list.

   Every row says WHY it is where it is ("seen 5 times, decoded ev1527, 92 %
   confidence") rather than showing a score, because that is a claim the user
   can check against what they just did with their thumb.

   AND IT UN-CHOPS TRANSMISSIONS. The frame boundary ends a recording after a
   fixed silence; set it shorter than a remote's own inter-word gap and ONE
   press arrives as several dissimilar pieces, leaving the user to transmit
   scraps one at a time hunting for the one that rings the bell. The box detects
   that shape (see rf_group.h), says so in words, offers a one-tap retry at a
   longer gap, and -- where the pieces fit back together -- offers the rejoined
   whole as a candidate in its own right, stitched with the silence that was
   actually measured between them rather than an invented one.

   WHERE IT LIVES. A flow (a sheet), not a tab: it is both a diagnostic and a
   way to make a signal, and it is reached from wherever someone realises they
   need it -- a node that needs a signal, the signal picker, and Diagnostics,
   where "the box cannot hear my remote" is already investigated.
   ====================================================================== */

/* Probe once per page load. 404 = this firmware predates listening sessions;
   hide the feature entirely rather than offering a button that cannot work. */
function loadRaw() {
  return api("/api/raw").then(function (res) {
    S.raw = res;
    S.has.raw = true;
    return res;
  }).catch(function (e) {
    if (e.status === 404 || e.status === 501) { S.has.raw = false; S.raw = null; }
    throw e;
  });
}

/* The squelch floor to START with.
 *
 * No fixed number is right: the AGC noise floor depends on the antenna, the
 * room and what else is on the band, and it was measured 10 dB hotter on the
 * author's box than on the bench the firmware default came from. GET /api/radio
 * already reports the live band level, so use it: sit a few dB above whatever
 * the band is doing right now, clamped so this can never be stricter than the
 * normal receiver nor so loose that the buffer fills with hash. */
function rawSuggestedFloor() {
  var live = S.radio && numOr(S.radio.rssi_dbm, null);
  if (live === null) return -80;
  var v = Math.round((live + 6) / 5) * 5;
  return Math.max(-100, Math.min(-75, v));
}

function rawFloorOptions(sel, offValue) {
  var out = [{ value: offValue, label: t("Off — record every burst") }];
  [-100, -95, -90, -85, -80, -75].forEach(function (v) {
    out.push({
      value: v,
      label: v === -75 ? t("{v} dBm (the normal receiver's squelch)", { v: v })
           : v === sel ? t("{v} dBm (suggested here)", { v: v })
           : v + " dBm"
    });
  });
  return out;
}

/* The honest verdict. The whole point of this screen is that "nothing was
   received" and "something was received but did not fit our assumptions" are
   different problems with different fixes, so this never says "no results". */
function rawVerdict(st) {
  var d = (st && st.dropped) || {};
  var r = (st && st.radio) || {};
  var set = (st && st.settings) || {};
  var heard = numOr(r.heard, 0);
  var floor = numOr(d.below_floor, 0);
  var tooLong = numOr(d.too_long, 0);
  var tooShort = numOr(d.too_short, 0);
  var count = numOr(st && st.count, 0);
  var cands = (st && st.candidates) || [];

  if (r.present === false) {
    return { kind: "bad", text: t("No CC1101 detected, so nothing can be recorded. See the states below.") };
  }
  if (count > 0) {
    /* Filled to the brim almost immediately, with nothing strong in it: that is
       the band's own noise, not a remote, and the fix is one number. */
    var strongest = null;
    (st.frames || []).forEach(function (f) {
      var v = numOr(f.rssi_dbm, null);
      if (v !== null && (strongest === null || v > strongest)) strongest = v;
    });
    if (st.stop_reason === "full" && numOr(st.elapsed_s, 99) * 3 <= numOr(set.seconds, 30) &&
        strongest !== null && strongest <= numOr(set.rssi_floor_dbm, -80) + 8) {
      return {
        kind: "warn",
        suggest: Math.min(-75, Math.round((strongest + 5) / 5) * 5),
        text: t("All {count} slots filled in {secs} s and the loudest thing in them was "
          + "{dbm} dBm. That is the receiver's own amplifier noise, not a transmitter. "
          + "Raise the squelch floor and run it again.",
          { count: count, secs: numOr(st.elapsed_s, 0), dbm: strongest })
      };
    }
    /* Something repeated: say so, because that is the evidence the ranking runs
       on and it is what tells the user the top row is worth trying first. */
    var frameTxt = count === 1 ? t("{n} frame", { n: count })
                               : t("{n} frames", { n: count });
    var best = cands.length ? numOr(cands[0].seen, 1) : 1;
    if (best > 1) {
      return {
        kind: "ok",
        text: t("{cands} from {frames}. The top one was heard {best} times, which is "
          + "what a real remote does and noise does not — try that one first.",
          { cands: cands.length === 1 ? t("{n} candidate", { n: cands.length })
                                      : t("{n} candidates", { n: cands.length }),
            frames: frameTxt, best: best })
      };
    }
    return {
      kind: "ok",
      text: t(strongest !== null
        ? "{frames} recorded, loudest {dbm} dBm, but nothing repeated. Press the button "
          + "several times in one session so the box can tell your remote apart from the "
          + "band — or just try the candidates below."
        : "{frames} recorded, but nothing repeated. Press the button several times in one "
          + "session so the box can tell your remote apart from the band — or just try "
          + "the candidates below.",
        { frames: frameTxt, dbm: strongest })
    };
  }
  if (st && st.running) return null;   /* still listening; not a verdict yet */
  if (!st || !st.held) return null;    /* no session has run yet */

  if (!heard && !floor && !r.carrier_seen) {
    return {
      kind: "bad",
      text: t("The radio heard NOTHING — not one burst, not even noise, and carrier sense "
        + "never asserted. That is a radio problem, not a threshold problem: these "
        + "settings cannot help. Check the antenna is fitted, and that the frequency and "
        + "modulation under Settings → Radio match what your remote actually sends.")
    };
  }
  if (floor > 0) {
    return {
      kind: "warn",
      suggest: numOr(set.rssi_floor_dbm, -80) - 10,
      text: t(floor === 1
        ? "{n} burst was received and thrown away for being quieter than {floor} dBm"
          + "{peak}. Lower the floor, or press the button closer to the box."
        : "{n} bursts were received and thrown away for being quieter than {floor} dBm"
          + "{peak}. Lower the floor, or press the button closer to the box.",
        { n: floor, floor: numOr(set.rssi_floor_dbm, -80),
          peak: typeof r.peak_rssi_dbm === "number"
            ? t(" (the band peaked at {dbm} dBm)", { dbm: r.peak_rssi_dbm }) : "" })
    };
  }
  if (tooLong > 0) {
    return {
      kind: "warn",
      text: t(tooLong === 1
        ? "{n} frame was longer than the 512-pulse limit and were discarded before they "
          + "reached us — a truncated recording would replay as a different waveform. "
          + "Lower the frame boundary so each transmission ends sooner instead of "
          + "running into the next one."
        : "{n} frames were longer than the 512-pulse limit and were discarded before they "
          + "reached us — a truncated recording would replay as a different waveform. "
          + "Lower the frame boundary so each transmission ends sooner instead of "
          + "running into the next one.", { n: tooLong })
    };
  }
  if (tooShort > 0) {
    return {
      kind: "warn",
      text: t(tooShort === 1
        ? "{n} burst was shorter than {min} pulses. Lower the minimum — but this is "
          + "usually the AGC hash rather than a remote."
        : "{n} bursts were shorter than {min} pulses. Lower the minimum — but this is "
          + "usually the AGC hash rather than a remote.",
        { n: tooShort, min: numOr(set.min_pulses, 4) })
    };
  }
  if (r.carrier_seen) {
    return {
      kind: "warn",
      text: t("Carrier energy was sensed but no pulse stream ever emerged from it. That "
        + "points at the radio settings rather than these thresholds: the frequency is "
        + "close but not right, the bandwidth is too narrow, or the transmitter is not "
        + "using OOK at all. Settings → Radio.")
    };
  }
  return {
    kind: "warn",
    text: t("The session ended with nothing recorded. The radio was alive and the band was "
      + "quiet — press the button while the countdown is running, and within a few metres.")
  };
}

/*
 * The listening session. Resolves with a signal if one was saved (so a caller
 * can bind it to a node in the same breath), otherwise null.
 */
function openListenFlow(opts) {
  opts = opts || {};
  return new Promise(function (resolve) {
    var done = false;
    var made = null;
    var running = false;

    var sh = openSheet(opts.title || t("Listen for a button"),
      opts.sub || t("Press the button on your remote several times. Everything the radio "
      + "hears is kept — the box ranks what it heard instead of deciding in advance what "
      + "a real signal is allowed to look like."),
      function () {
        stopPoll("raw");
        if (done) return;
        done = true;
        /* Leaving the box recording with relaxed thresholds behind your back is
           not something to do silently. The FRAMES are kept (stopping is not
           discarding), so reopening this shows them again. */
        if (running) postJSON("/api/raw/stop", {}).catch(function () { /* ignore */ });
        resolve(made);
      });

    var count = el("div", "countdown idle", "—");
    var state = el("div", "listening");
    add(sh.body, count, state);

    var fragWrap = el("div");
    var verdict = el("div");
    var candWrap = el("div");
    add(sh.body, fragWrap, verdict, candWrap);

    var msg = el("div", "formmsg");
    var ctl = el("div", "btnrow");
    var startBtn = el("button", "btn primary", t("Start listening"));
    startBtn.type = "button";
    var stopBtn = el("button", "btn hidden", t("Stop"));
    stopBtn.type = "button";
    add(ctl, startBtn, stopBtn);
    add(sh.body, ctl, msg);

    /* Every frame, unranked — the fallback for the case the ranking got wrong,
       collapsed because it is not the answer, it is the raw material. */
    var allSec = section(t("Every frame recorded"), null, false);
    var allWrap = el("div");
    add(allSec.bodyEl, allWrap);
    add(sh.body, allSec);

    /* ---- the relaxed filters, out of the way but not hidden ---- */
    var lim = (S.raw && S.raw.limits) || {};
    var offValue = numOr(lim.rssi_off_dbm, -120);
    var sug = rawSuggestedFloor();

    var setSec = section(t("Advanced: what the receiver is allowed to hear"), null, false);

    var secSel = selectEl([
      { value: 15, label: t("15 seconds") },
      { value: 30, label: t("30 seconds") },
      { value: 60, label: t("1 minute") },
      { value: 120, label: t("2 minutes") }
    ], 30);
    add(setSec.bodyEl, field(t("Listen for"), secSel,
      t("A session also stops as soon as its 32 slots are full, and it always stops on "
      + "its own — the box never keeps recording behind your back.")));

    var idleIn = inputEl("number", 8000, {
      min: String(numOr(lim.idle_us_min, 1000)),
      max: String(numOr(lim.idle_us_max, 32000)), step: "500"
    });
    add(setSec.bodyEl, field(t("Frame boundary (µs of silence)"), idleIn,
      t("How much quiet ends one recording and starts the next. Set it too LOW and one "
      + "transmission is chopped into pieces — the box detects that and says so — too "
      + "HIGH and several presses are glued into one frame, and anything over {max} "
      + "pulses is thrown away entirely.", { max: numOr(lim.max_pulses, 512) })));

    var minIn = inputEl("number", 4, {
      min: String(numOr(lim.min_pulses_min, 2)),
      max: String(numOr(lim.min_pulses_max, 64)), step: "1"
    });
    add(setSec.bodyEl, field(t("Minimum pulses"), minIn,
      t("Shorter bursts are treated as noise and dropped. The normal receiver uses 32, "
      + "which is exactly why a protocol with a short frame never shows up at all. "
      + "Low values also let the amplifier's own hash through.")));

    var floorSel = selectEl(rawFloorOptions(sug, offValue), sug);
    add(setSec.bodyEl, field(t("Squelch floor"), floorSel,
      t("Bursts quieter than this are counted but not kept. Pre-set a few dB above "
      + "whatever the band is doing right now, which is the only value that is correct "
      + "on both a quiet bench and a busy flat. The normal receiver squelches at {dbm} "
      + "dBm; anything below that is more permissive than the box has ever been.",
      { dbm: numOr(lim.normal_squelch_dbm, -75) })));

    add(sh.body, setSec);

    var ep = section(t("What a listening session actually does"), null, false);
    [
      t("The receiver is ALWAYS listening. It has to be: a button you already registered "
      + "must ring the moment it is pressed, so there is no on-demand receive mode to "
      + "switch on. What a session changes is what happens to a signal the box does NOT "
      + "recognise — normally dropped with one line in the activity feed, here kept."),
      t("The normal receive path applies four filters before you would ever see a signal, "
      + "and every one of them is a correct guess about the cheap remotes this box was "
      + "built around — and a possible lie about anything else. A session drops the "
      + "minimum frame length from 32 pulses to whatever you set, makes the frame "
      + "boundary an adjustable number instead of a fixed 8 ms, lowers (or removes) the "
      + "signal-strength squelch, and keeps every repeat separately instead of merging "
      + "them."),
      t("Nothing is admitted or rejected on how it looks. Candidates are RANKED: a "
      + "waveform heard several times outranks one heard once, and a recognised protocol "
      + "only breaks ties. A candidate no decoder understands is completely usable — the "
      + "exact timings are stored and replay works; only the human-readable name is "
      + "missing."),
      t("Nothing is written to flash and nothing reaches your node graph: while a session "
      + "runs, the doorbell keeps working exactly as it did, because only signals that "
      + "would have passed the NORMAL thresholds are still routed. Noise cannot ring "
      + "anything."),
      t("The frames live in RAM only, and they are handed back when you start another "
      + "session, when you close this and come back much later, or when the box reboots.")
    ].forEach(function (line) { add(ep.bodyEl, el("p", "small", line)); });
    add(sh.body, ep);

    /* ---- polling ---- */

    function tick() {
      return api("/api/raw").then(function (res) {
        S.raw = res;
        render(res, null);
      }).catch(function (e) {
        render(null, e);
      });
    }

    function render(st, err) {
      if (err) {
        count.textContent = "—";
        count.className = "countdown idle";
        clear(state);
        add(state, el("span", null, t("Listening is not available: {msg}", { msg: err.message })));
        startBtn.disabled = true;
        stopPoll("raw");
        return;
      }
      running = !!st.running;
      startBtn.classList.toggle("hidden", running);
      stopBtn.classList.toggle("hidden", !running);
      setSec.classList.toggle("hidden", running);

      if (running) {
        var rem = numOr(st.remaining_s, 0);
        count.textContent = Math.floor(rem / 60) + ":" + ("0" + (rem % 60)).slice(-2);
        count.className = "countdown";
        clear(state);
        add(state, el("span", "pulse"), el("span", null,
          t("Listening — press your button now, several times. {used} of {cap} slots used.",
            { used: numOr(st.count, 0), cap: numOr(st.capacity, 32) })));
      } else {
        count.textContent = "—";
        count.className = "countdown idle";
        clear(state);
        add(state, el("span", null, st.held
          ? t("Stopped ({reason}) after {secs} s.", {
              reason: st.stop_reason === "full" ? t("all slots used")
                : st.stop_reason === "time" ? t("time up") : t("you stopped it"),
              secs: numOr(st.elapsed_s, 0) })
          : t("Not listening. Nothing is being stored and the box behaves normally.")));
      }

      renderFragmentation(st);
      renderVerdict(st);
      renderCandidates(st);
      renderAll(st);
    }

    /*
     * THE THING THE USER ACTUALLY HIT. One press arriving as five rows is not a
     * mystery to be solved by trying all five: it is a threshold that fired too
     * early, and the box can both say so and offer the fix. Where the pieces fit
     * back together the rejoined whole is already in the candidate list above,
     * marked, so the retry is an improvement rather than the only way forward.
     */
    function renderFragmentation(st) {
      clear(fragWrap);
      var fr = st && st.fragmentation;
      if (!fr || !fr.detected || st.running) return;

      var runs = numOr(fr.runs, 0);
      var frames = numOr(fr.frames, 0);
      var rejoined = numOr(fr.rejoined, 0);
      var suggest = numOr(fr.suggest_idle_us, 0);
      var idle = numOr((st.settings || {}).idle_us, 8000);

      var note = el("div", "note warn");
      add(note, el("div", null,
        t(runs === 1
          ? "{runs} transmission was cut into {pieces} pieces: the frame boundary "
            + "({idle} µs of silence) fired while your remote was still sending. That is "
            + "why one press turns into several rows."
          : "{runs} transmissions were cut into {pieces} pieces: the frame boundary "
            + "({idle} µs of silence) fired while your remote was still sending. That is "
            + "why one press turns into several rows.",
          { runs: runs, pieces: frames, idle: idle })));
      if (rejoined > 0) {
        add(note, el("div", "small",
          t(rejoined === 1
            ? "{n} of them has been stitched back together from the pieces, using the "
              + "silence actually measured between them. Those are marked 🧩 in the list "
              + "and are the ones to try first."
            : "{n} of them have been stitched back together from the pieces, using the "
              + "silence actually measured between them. Those are marked 🧩 in the list "
              + "and are the ones to try first.", { n: rejoined })));
      }
      add(fragWrap, note);

      if (suggest) {
        var again = el("button", "btn");
        again.type = "button";
        again.textContent = t("Try again with a {us} µs gap", { us: suggest });
        again.style.marginTop = ".5rem";
        again.addEventListener("click", function () {
          idleIn.value = String(suggest);
          startBtn.click();
        });
        add(fragWrap, again);
        add(fragWrap, el("div", "hint",
          t("This raises the frame boundary so each press is recorded whole. Nothing you "
          + "already have is lost until the new session starts.")));
      }
    }

    function renderVerdict(st) {
      clear(verdict);
      var v = rawVerdict(st);
      if (!v) return;
      var note = el("div", "note " + v.kind, v.text);
      add(verdict, note);
      if (typeof v.suggest === "number" && !st.running) {
        var again = el("button", "btn", t("Try again at {dbm} dBm", { dbm: v.suggest }));
        again.type = "button";
        again.style.marginTop = ".5rem";
        again.addEventListener("click", function () {
          floorSel.value = String(v.suggest);
          if (floorSel.value !== String(v.suggest)) {
            /* Not one of the offered steps — add it so the select can hold it. */
            var op = el("option", null, v.suggest + " dBm");
            op.value = String(v.suggest);
            add(floorSel, op);
            floorSel.value = String(v.suggest);
          }
          startBtn.click();
        });
        add(verdict, again);
      }
    }

    /*
     * The ranked list. Most likely first, and each row says why it is where it
     * is: an undecoded waveform heard five times legitimately sits above a
     * pristine decoded one heard twice, and the user has to be able to see that
     * rather than take it on faith.
     */
    function renderCandidates(st) {
      clear(candWrap);
      var list = (st && st.candidates) || [];
      if (!list.length) return;

      add(candWrap, el("div", "lg-label", t("Most likely first")));
      add(candWrap, el("div", "hint",
        t("Ranked by how often each waveform was heard — a real remote repeats itself and "
        + "noise does not. A recognised protocol only breaks ties: an undecoded candidate "
        + "is fully usable and can be top of the list. Tap one to see it, trim it, and "
        + "send it back out.")));

      var ul = el("ul", "list");
      list.forEach(function (c, i) {
        var li = el("li");
        var b = el("button", "listitem"); b.type = "button";
        add(b, el("span", "li-ico", c.merged ? "🧩" : (c.decoded ? "🔎" : "〰️")));

        var main = el("div", "li-main");
        add(main, el("div", "li-title", (c.decoded && c.decoded.text)
          ? c.decoded.text
          : t("Unknown protocol — still fully replayable")));

        var why = el("div", "li-sub");
        if (i === 0) add(why, el("span", "chip ok", t("most likely")));
        add(why, el("span", null, (i === 0 ? " " : "") + (c.why || "")));
        add(main, why);

        var det = t("{pulses} pulses · {ms} ms · base {base} µs", {
          pulses: numOr(c.pulse_count, 0),
          ms: (numOr(c.airtime_us, 0) / 1000).toFixed(1),
          base: numOr(c.base_us, 0)
        });
        if (c.merged && c.frames && c.frames.length) {
          det += t(" · rejoined from frames {list}", { list: c.frames.join(" + ") });
        }
        if (c.truncated) det += t(" · TRUNCATED at the 512-pulse limit");
        add(main, el("div", "li-sub", det));
        add(b, main);

        var meta = el("div", "li-meta");
        add(meta, el("div", null, numOr(c.seen, 1) + "×"));
        add(meta, el("div", "mono", numOr(c.rssi_dbm, 0) + " dBm"));
        add(b, meta);

        b.addEventListener("click", function () {
          openWaveform("candidate", c.id, function (sig) {
            made = sig || made;
            if (sig) { loadSignals(); if (S.graph) loadGraph(); }
          });
        });
        add(li, b);
        add(ul, li);
      });
      add(candWrap, ul);

      /* GET /api/raw embeds only the top few so a full session's response can
         still be built on a box that is already holding 43 KB of it. Say so
         rather than letting a truncated list look like the whole list. */
      var total = numOr(st.candidates_total, list.length);
      if (total > list.length) {
        add(candWrap, el("div", "hint",
          t("Showing the top {n} of {total} candidates. The rest were heard fewer times "
          + "still — if none of these is yours, the frame list below has everything.",
          { n: list.length, total: total })));
      }
    }

    /* The unranked truth underneath, for when the ranking is not what you want.
       Grouping is a convenience; it never hides anything. */
    function renderAll(st) {
      clear(allWrap);
      var frames = (st && st.frames) || [];
      allSec.classList.toggle("hidden", !frames.length);
      if (!frames.length) return;

      var head = $("summary", allSec);
      if (head) head.textContent = t("Every frame recorded ({n})", { n: frames.length });
      add(allWrap, el("div", "hint",
        t("Every frame exactly as it arrived, in order, with nothing grouped or ranked. "
        + "The list above is built from these — this is here so grouping can never hide "
        + "something from you.")));

      var ul = el("ul", "list");
      frames.forEach(function (f) {
        var li = el("li");
        var b = el("button", "listitem"); b.type = "button";
        add(b, el("span", "li-ico", f.decoded ? "🔎" : "〰️"));
        var main = el("div", "li-main");
        add(main, el("div", "li-title", t("Frame {i} · {n} pulses",
          { i: f.index, n: numOr(f.pulse_count, 0) })));
        add(main, el("div", "li-sub", (f.decoded && f.decoded.text)
          ? f.decoded.text
          : t("No decoder claimed this — still fully replayable")));
        add(main, el("div", "li-sub",
          t("{ms} ms · base {base} µs · {conf}% confidence{trunc}", {
            ms: (numOr(f.airtime_us, 0) / 1000).toFixed(1),
            base: numOr(f.base_us, 0),
            conf: numOr(f.confidence, 0),
            trunc: f.truncated ? t(" · TRUNCATED at the 512-pulse limit") : ""
          })));
        add(b, main);
        var meta = el("div", "li-meta");
        add(meta, el("div", "mono", numOr(f.rssi_dbm, 0) + " dBm"));
        add(b, meta);
        b.addEventListener("click", function () {
          openWaveform("frame", f.index, function (sig) {
            made = sig || made;
            if (sig) { loadSignals(); if (S.graph) loadGraph(); }
          });
        });
        add(li, b);
        add(ul, li);
      });
      add(allWrap, ul);
    }

    startBtn.addEventListener("click", function () {
      startBtn.disabled = true;
      setMsg(msg, t("Starting…"));
      postJSON("/api/raw/start", {
        seconds: intOf(secSel, 30),
        idle_us: intOf(idleIn, 8000),
        min_pulses: intOf(minIn, 4),
        rssi_floor_dbm: intOf(floorSel, -80)
      }).then(function (st) {
        S.raw = st;
        setMsg(msg, t("Listening. Press your remote button now — several times."), "ok");
        render(st, null);
        poll("raw", 1000, tick);
      }).catch(function (e) {
        setMsg(msg, e.message, "err");
      }).then(function () { startBtn.disabled = false; });
    });

    stopBtn.addEventListener("click", function () {
      stopBtn.disabled = true;
      postJSON("/api/raw/stop", {}).then(function (st) {
        S.raw = st;
        setMsg(msg, "");
        render(st, null);
      }).catch(function (e) { setMsg(msg, e.message, "err"); })
        .then(function () { stopBtn.disabled = false; });
    });

    poll("raw", 1000, tick);
    /* Straight into listening: someone who opened this has already said they
       want to register a button and should not have to say it twice. */
    if (opts.autoStart !== false) setTimeout(function () { startBtn.click(); }, 60);
  });
}

/*
 * One waveform, with trim handles. `kind` is "candidate" (a ranked, possibly
 * rejoined candidate) or "frame" (one recording exactly as it arrived); the two
 * differ only in which URL the waveform comes from, and deliberately in nothing
 * else -- what you inspect must be what you transmit and what you save.
 *
 * TRIMMING IS THE POINT, NOT A GARNISH. A relaxed frame boundary deliberately
 * captures more than one transmission at a time, so what comes back is often
 * "the thing I want, with something either side of it". `from`/`to` are pulse
 * indices into the same durations_us the plot is drawn from, and they are sent
 * verbatim to the box — so what you see selected is exactly what is transmitted
 * and exactly what is saved. There is no second interpretation anywhere.
 *
 * Both a pair of sliders (drag) and a pair of number boxes (type) drive the same
 * two values, because on a phone you want to drag and at a keyboard you want to
 * nudge by one.
 */
function openWaveform(kind, id, onSaved) {
  var base = (kind === "candidate") ? "/api/raw/candidates/" : "/api/raw/";
  var sh = openSheet((kind === "candidate") ? t("Candidate {id}", { id: id })
                                            : t("Frame {id}", { id: id }), null);
  add(sh.body, el("div", "empty", t("Loading…")));

  api(base + id).then(function (f) {
    clear(sh.body);
    var durs = f.durations_us || [];
    var n = durs.length;
    var from = 0, to = n;

    var chips = el("div", "chiprow");
    if (typeof f.seen === "number") {
      add(chips, el("span", "chip ok", t("seen {n}×", { n: f.seen })));
    }
    if (f.merged) add(chips, el("span", "chip accent", t("🧩 rejoined from {n} pieces", { n: (f.frames || []).length })));
    if (f.decoded && f.decoded.text) add(chips, el("span", "chip accent mono", f.decoded.text));
    else add(chips, el("span", "chip warn", t("Unknown protocol — raw pulses only")));
    add(chips, el("span", "chip mono", numOr(f.rssi_dbm, 0) + " dBm"));
    add(chips, el("span", "chip mono", numOr(f.confidence, 0) + "%"));
    if (f.truncated) add(chips, el("span", "chip bad", t("truncated at 512 pulses")));
    add(sh.body, chips);

    if (f.why) add(sh.body, el("div", "hint", t("Ranked here because it was {why}.", { why: f.why })));
    if (f.merged) {
      add(sh.body, el("div", "note",
        t("This is one transmission that the frame boundary cut into {n} pieces (frames "
        + "{frames}), put back together with the silence actually measured between them "
        + "— {gaps} µs. It is a reconstruction, so test it before you keep it; the "
        + "individual pieces are still listed on their own.", {
          n: (f.frames || []).length,
          frames: (f.frames || []).join(" + "),
          gaps: (f.gaps_us || []).join(" µs, ")
        })));
    }

    var plot = el("div");
    add(sh.body, plot);

    var fromRange = inputEl("range", 0, { min: "0", max: String(Math.max(0, n - 1)), step: "1" });
    var toRange = inputEl("range", n, { min: "1", max: String(n), step: "1" });
    var fromNum = inputEl("number", 0, { min: "0", max: String(Math.max(0, n - 1)), step: "1" });
    var toNum = inputEl("number", n, { min: "1", max: String(n), step: "1" });

    var trim = el("div", "trim");
    add(trim, field(t("Start at pulse"), fromRange));
    add(trim, field(t("End before pulse"), toRange));
    var nums = el("div", "row");
    add(nums, field(t("from"), fromNum), field(t("to"), toNum));
    add(trim, nums);
    add(sh.body, trim);
    add(sh.body, el("div", "hint",
      t("Indices count from 0, and `to` is the first pulse NOT included — so 0 to {n} "
      + "is the whole thing. Levels alternate, so a selection starting on an odd "
      + "index starts on the opposite level; the box accounts for that when it sends "
      + "or saves, and the caption above says which.", { n: n })));

    function redraw() {
      clear(plot);
      var w = waveform(durs, f.first_level, { from: from, to: to });
      if (w) add(plot, w);
    }
    function sync(src) {
      if (from > n - 1) from = Math.max(0, n - 1);
      if (from < 0) from = 0;
      if (to > n) to = n;
      if (to <= from) to = Math.min(n, from + 1);
      if (src !== "fromRange") fromRange.value = String(from);
      if (src !== "toRange") toRange.value = String(to);
      if (src !== "fromNum") fromNum.value = String(from);
      if (src !== "toNum") toNum.value = String(to);
      redraw();
    }
    function bind(input, isFrom, name) {
      input.addEventListener("input", function () {
        var v = parseInt(input.value, 10);
        if (!isFinite(v)) return;
        if (isFrom) from = v; else to = v;
        sync(name);
      });
    }
    bind(fromRange, true, "fromRange");
    bind(toRange, false, "toRange");
    bind(fromNum, true, "fromNum");
    bind(toNum, false, "toNum");
    sync(null);

    add(sh.body, el("div", "divider"));

    /* --- transmit the selection, without saving anything --- */
    var tmsg = el("div", "formmsg");
    var trow = el("div", "btnrow");
    var txBtn = el("button", "btn", t("\u{1F4E1} Transmit selection"));
    txBtn.type = "button";
    add(trow, txBtn);
    add(sh.body, el("div", "lg-label", t("Try it")));
    add(sh.body, el("div", "hint",
      t("Sends the selected pulses straight out, without storing anything. This is the "
      + "loop: send, listen at the bell, adjust the trim, send again.")));
    add(sh.body, trow, tmsg);
    var block = txBlockNote();
    if (block) add(sh.body, block);
    if (!txAvailable()) txBtn.disabled = true;

    txBtn.addEventListener("click", function () {
      var old = txBtn.textContent;
      txBtn.disabled = true;
      txBtn.textContent = t("Sending…");
      setMsg(tmsg, "");
      postJSON(base + id + "/transmit", { from: from, to: to })
        .then(function (res) {
          txBtn.textContent = t("Sent ✓");
          setTimeout(function () { txBtn.textContent = old; txBtn.disabled = false; }, 1400);
          setMsg(tmsg, t("{pulses} pulses × {repeats}. This only confirms the pulses left "
            + "the radio — it cannot know whether a receiver reacted.",
            { pulses: numOr(res.pulse_count, to - from), repeats: numOr(res.repeats, 0) }), "ok");
        }).catch(function (e) {
          txBtn.textContent = old;
          txBtn.disabled = false;
          if (e.status === 503) { S.txBlock = e.message; S.txBlockKind = "api"; renderTxNote(); }
          setMsg(tmsg, e.message, "err");
        });
    });

    /* --- promote it to a stored signal --- */
    add(sh.body, el("div", "divider"));
    add(sh.body, el("div", "lg-label", t("Keep it")));
    add(sh.body, el("div", "hint",
      t("Saves the SELECTION as an ordinary stored signal: it can then be bound to a node, "
      + "renamed, transmitted and matched against like any other — decoded or not.")));
    var nameIn = inputEl("text", "", { maxlength: "31", placeholder: t("e.g. Front door") });
    add(sh.body, field(t("Name"), nameIn));
    var smsg = el("div", "formmsg");
    var foot = el("div", "formfoot");
    var saveBtn = el("button", "btn primary", t("Save as signal"));
    saveBtn.type = "button";
    saveBtn.addEventListener("click", function () {
      var nm = trimOf(nameIn);
      if (!nm) { setMsg(smsg, t("Give it a name first."), "err"); nameIn.focus(); return; }
      saveBtn.disabled = true;
      setMsg(smsg, t("Saving…"));
      postJSON(base + id + "/save", { name: nm, from: from, to: to })
        .then(function (sig) {
          setMsg(smsg, t("Saved as “{name}”.", { name: sig.name }), "ok");
          if (onSaved) onSaved(sig);
          setTimeout(function () { sh.close(); }, 700);
        }).catch(function (e) {
          saveBtn.disabled = false;
          setMsg(smsg, e.message, "err");
        });
    });
    add(foot, saveBtn, smsg);
    add(sh.body, foot);
  }).catch(function (e) {
    clear(sh.body);
    add(sh.body, el("div", "note bad", t("Could not load this waveform: {msg}", { msg: e.message })));
  });
}

/* The one entry-point button, so its wording cannot drift between the places it
   appears. */
function listenButton(label, onDone) {
  var b = el("button", "btn", label || t("\u{1F3A7} Listen for a button"));
  b.type = "button";
  b.addEventListener("click", function () {
    openListenFlow({}).then(function (sig) { if (onDone) onDone(sig); });
  });
  return b;
}

/* ======================================================================
   DASHBOARD -- the node graph IS the product, and now it is ALL of it

   One screen, three things, in this order:
     1. the two warnings that would make the whole screen a lie (no radio, no
        home network) and the box-status chips,
     2. the view switch (Map / List) and "Add node",
     3. the graph itself.

   The live feed and the recipes used to sit under the graph. They were both
   moved out — to Activity and to the Handbook — because between them they cost
   about two screenfuls above the fold's worth of scrolling on a phone, and the
   thing you opened the app to look at was underneath them. A feed you have to
   scroll past is worse than one tap away, and a recipe list is reference
   material, not a control.
   ====================================================================== */

/* `g` is the node's GROUP, and the group is what decides its ports:

     source  -- output only
     logic   -- both
     sink    -- input only

   Three groups, and every node type fits one of them honestly. There was
   briefly a fourth, `signal`, for a single node that stood for a stored code
   and carried a port at BOTH ends -- and that is exactly what went wrong with
   it: what such a node did depended on how the event had arrived, which is
   invisible on screen. One code now gets up to two nodes instead. A Signal
   receiver is a source in every sense (it starts chains, it has no input), a
   Signal sender is a sink in every sense (it acts, it has no output), and the
   two dots on the map finally mean what they look like. */
var NODE_TYPES = [
  /* The pair. Same signal_id pool, one direction each -- wire an rx to a tx and
     you have "when I hear this, send that", written down where you can read it. */
  { t: "signal.rx", g: "source", label: "Signal receiver", ico: "📶",
    help: "Fires when one stored 433 MHz code is heard on air. Listens only — it never " +
          "sends anything." },
  { t: "source.gpio", g: "source", label: "Wired button", ico: "🔌",
    help: "Fires when a button wired to a GPIO pin is pressed. Optional." },
  /* "Virtual trigger" until 0.5, which is why it is still `source.virtual` on
     the wire and on flash. Nobody searching for "a button Home Assistant can
     press" ever found it under that name — the node existed the whole time and
     got asked for as a missing feature. The LABEL is what people read, so the
     label says what it is for. */
  { t: "source.virtual", g: "source", label: "MQTT button", ico: "✨",
    help: "A button Home Assistant can press. Any message on its topic fires it — and so do " +
          "the ▶ here, the REST API and any other MQTT client." },
  { t: "source.any_rf", g: "source", label: "Any RF signal", ico: "📻",
    help: "Wildcard: fires on EVERY burst the receiver hears, registered or not." },
  /* Two genuinely different jobs under one type, so the help describes them
     separately. The old one line ("ANY or ALL of its inputs inside a time
     window") implied the window applied to both; it does not apply to ANY at
     all, which made the useful half of the node the invisible half. */
  { t: "logic.group", g: "logic", label: "Group", ico: "🔗",
    help: "ANY: a merge point — everything that arrives passes straight through, and the " +
          "window is not used. ALL: passes on only once every inbound link has carried an " +
          "event inside the window, then re-arms." },
  { t: "logic.throttle", g: "logic", label: "Rate limit", ico: "⏱",
    help: "Passes the first press, then ignores everything for a cooldown you set. " +
          "Someone can lean on the button — the bell still rings once." },
  /* The "loop" node the user asked for, under a name that does not collide with
     the graph's own meaning of a loop (a cycle in the wiring, which the engine
     warns about and refuses to walk). */
  { t: "logic.repeat", g: "logic", label: "Repeat", ico: "🔁",
    help: "Rings straight away, then again a few more times at an interval you set. " +
          "One press, several chimes — for when the first one gets missed." },
  /* The node that is a switch in the wire. Everything else here is a source, a
     transform or a sink; this one sits IN a link and decides whether it
     conducts, which is what makes "turn the inside bell off tonight" a toggle
     instead of deleting a link and remembering to put it back. */
  { t: "logic.switch", g: "logic", label: "Switch", ico: "🎚",
    help: "A switch in the wire: while it is ON events pass straight through, while it is " +
          "OFF nothing gets past it. Give it an MQTT topic and Home Assistant gets a real " +
          "toggle for it." },
  /* The other half of the pair. 📡 is the same glyph the Transmit buttons wear,
     so the node that sends and the button that sends read as one idea. */
  { t: "signal.tx", g: "sink", label: "Signal sender", ico: "📡",
    help: "Transmits one stored 433 MHz code whenever something linked into it fires. " +
          "Sends only — it never listens." },
  { t: "sink.mqtt", g: "sink", label: "MQTT publish", ico: "📨",
    help: "Publishes to your broker / fires a Home Assistant device trigger." },
  /* The one node that DOES nothing. It exists to be looked at: a lamp that
     lights whenever the chain reaches it, and a rolling ten-minute timeline of
     when it did. Drop one next to any sink to prove a chain fires without
     ringing anything. */
  { t: "sink.monitor", g: "sink", label: "Monitor", ico: "💡",
    help: "Only watches — it changes nothing. No signal goes out, nothing is published. " +
          "Its lamp lights whenever the chain reaches it, and its timeline shows the last " +
          "10 minutes of hits." }
];
/* NODE_TYPES above holds ENGLISH text because that text is the translation key
   (see the i18n block at the top). It is a top-level `var`, evaluated once when
   the file loads, so it must never be translated in place — the language can
   change afterwards. Translation happens HERE, on every lookup, which is what
   makes a language switch redraw the palette, the cards and the handbook in the
   new language without a reload.

   `.t` (the wire name, "logic.switch") and `.g` (the group) are identifiers,
   not copy, and stay exactly as they are. */
function nodeType(type) {
  for (var i = 0; i < NODE_TYPES.length; i++) {
    if (NODE_TYPES[i].t === type) {
      var d = NODE_TYPES[i];
      return { t: d.t, g: d.g, ico: d.ico, label: t(d.label), help: t(d.help) };
    }
  }
  return { t: type, g: "logic", label: type || t("unknown"), ico: "⚙", help: "" };
}
function nodeById(id) {
  var ns = (S.graph && S.graph.nodes) || [];
  for (var i = 0; i < ns.length; i++) if (ns[i].id === id) return ns[i];
  return null;
}
function nodeName(id) {
  var n = nodeById(id);
  return n ? (n.name || nodeType(n.type).label) : t("node {id}", { id: id });
}
function signalName(id) {
  var list = S.signals || [];
  for (var i = 0; i < list.length; i++) if (list[i].id === id) return list[i].name || t("Signal {id}", { id: id });
  return null;
}
/* Ports follow from the group, with no exceptions left: a source has no input,
   a sink has no output, and logic has both. */
function hasInput(n) { return nodeType(n.type).g !== "source"; }
function hasOutput(n) { return nodeType(n.type).g !== "sink"; }

/* The two halves of one stored code. Asked often enough -- and in enough
   places -- that the type strings are written down once here rather than
   compared inline wherever a signal section, a ▶ or a summary is built. */
function isSignalRx(n) { return !!n && n.type === "signal.rx"; }
function isSignalTx(n) { return !!n && n.type === "signal.tx"; }
function isSignalNode(n) { return isSignalRx(n) || isSignalTx(n); }

/* ------------------------------------------------------------- switches --

   A Switch node's POSITION is its `enabled` flag -- the same field, not a
   second one beside it. The firmware skips a disabled node when it walks the
   graph, and "the walk does not enter it" is exactly what "the wire does not
   conduct" means, so the two ideas are one. That is also why a Switch card
   never shows the generic "disabled" chip: on this type that word is wrong,
   the honest word is OFF.

   Everywhere else in the UI `enabled === false` still means "switched off by
   hand and out of service", which reads the same way on the canvas: a node the
   events do not reach, and links that do not carry. */
/* "Outside bell" -> "outside_bell". Mirrors the firmware's slugify(), so the
   topic previewed here is the topic the box will actually answer on. */
function topicSlug(s) {
  return String(s || "").toLowerCase()
           .replace(/[^a-z0-9]+/g, "_").replace(/^_+|_+$/g, "");
}

/* MQTT TOPIC LIMIT. The field holds 48 bytes including the terminator, so 47
   characters is what actually fits. The inputs used to advertise 48 and let the
   user type one character the box would then refuse. */
var TOPIC_MAX = 47;

/*
 * A DELIBERATE MIRROR of db_mqtt_topic_valid() in main/mqtt_topic.c — same rule,
 * same order, same words. Returns "" when the topic is fine, or the complete
 * sentence to show when it is not.
 *
 * WHY IT IS DUPLICATED AT ALL. The firmware is the authority and validates every
 * write regardless; this copy exists so the rule reads as you type instead of
 * arriving as a server error after you have pressed Save. Keeping the wording
 * identical is what stops the two from being experienced as two different rules.
 * If you change one, change the other — and host-test/test_node_graph.c pins the
 * C side down.
 *
 * The one place they can differ is the length of a non-ASCII string: C counts
 * bytes, JavaScript counts UTF-16 units. Both still REFUSE it — non-ASCII is
 * rejected outright — so only the sentence can differ, never the verdict.
 *
 * '#' and '+' are MQTT wildcards: a broker refuses a PUBLISH to a topic
 * containing one, so a single typo here can take the whole bridge down. The
 * slash rules are not MQTT requirements — a leading, trailing or doubled '/' is
 * legal and produces an empty topic level — but here they are always a mistake,
 * so they are refused rather than silently made into a topic nobody can read.
 */
function topicError(value, fieldName, maxLen) {
  var val = String(value === null || value === undefined ? "" : value);
  var f = fieldName || t("topic");
  var max = maxLen || TOPIC_MAX;

  /* Empty is the caller's business, not a syntax error: on a node it means "no
     MQTT", in Settings it means "use the default". */
  if (val.length === 0) return "";

  if (val.length > max)
    return t("\"{field}\" is too long: {len} characters, the limit is {max}.",
             { field: f, len: val.length, max: max });

  if (val.charAt(0) === "/")
    return t("\"{field}\" must not start with '/' \u2014 a leading slash makes an " +
             "empty first topic level. The box adds the separators itself.", { field: f });

  for (var i = 0; i < val.length; i++) {
    var ch = val.charAt(i);
    var c = val.charCodeAt(i);

    if (ch === "#" || ch === "+")
      return t("\"{field}\" contains '{ch}', which is an MQTT wildcard. A " +
               "message cannot be published to a topic containing '#' or '+' \u2014 " +
               "the broker refuses it.", { field: f, ch: ch });

    if (c < 0x20 || c >= 0x7F) {
      var hex = c.toString(16).toUpperCase();
      if (hex.length < 2) hex = "0" + hex;
      return t("\"{field}\" contains a non-printable character (byte 0x{hex}) " +
               "at position {pos}. Only plain printable ASCII is allowed.",
               { field: f, hex: hex, pos: i + 1 });
    }

    if (ch === "/" && val.charAt(i + 1) === "/")
      return t("\"{field}\" contains an empty level ('//') at position {pos}. " +
               "That is legal MQTT but almost always a typo, so it is refused " +
               "rather than producing a topic nobody can read.", { field: f, pos: i + 1 });
  }

  if (val.charAt(val.length - 1) === "/")
    return t("\"{field}\" must not end with '/' \u2014 a trailing slash makes an " +
             "empty last topic level.", { field: f });

  return "";
}

/*
 * The "Expose to Home Assistant / MQTT" checkbox, shared by the three node types
 * the bridge actually looks at (MQTT button, Switch, MQTT publish).
 *
 * CHECKED BY DEFAULT, and `mqtt_enabled !== false` rather than a truthy test on
 * purpose: a node saved by an older firmware has no such field at all, and the
 * honest reading of "absent" is the default, which is on.
 *
 * It exists because a blank topic stopped meaning "no MQTT" — a Switch with no
 * topic now falls back to a slug of its NAME, so there was no longer any way to
 * keep one off Home Assistant. A magic topic value was considered and rejected:
 * '-' is a perfectly legal MQTT topic level, so any sentinel collides with
 * something somebody could legitimately want to publish to.
 */
function exposeField(n, onChange) {
  var f = checkField(t("Expose to Home Assistant / MQTT"), n.mqtt_enabled !== false,
    t("On by default. Uncheck it and this node disappears from MQTT completely \u2014 nothing " +
      "subscribed, nothing published, and its Home Assistant entity removed rather than left " +
      "behind permanently unavailable. Inside the graph nothing changes: it still fires, still " +
      "gates, still works from this page and the REST API."));
  f.classList.add("full");
  f.input.addEventListener("change", onChange);
  return f;
}

/*
 * Wire live validation onto one topic input.
 *
 * `errEl` carries the message, `field` is the name the message uses (so a user
 * looking at three topic fields is told WHICH one is wrong, exactly as the
 * firmware's error would). Returns a function that re-runs the check and returns
 * true when the value is usable — call it before POSTing, so a Save can be
 * refused for a value the user pasted rather than typed.
 */
function bindTopicCheck(input, errEl, field, after) {
  function run() {
    var e = topicError(trimOf(input), field, TOPIC_MAX);
    if (errEl) {
      errEl.textContent = e;
      errEl.className = e ? "hint bad" : "hint";
    }
    if (e) input.classList.add("badinput");
    else   input.classList.remove("badinput");
    if (after) after(e);
    return !e;
  }
  input.addEventListener("input", run);
  run();
  return run;
}

/* "outside_bell" -> "Outside bell". Mirrors what the firmware does when a node
   still carries its default name: the topic becomes the label. */
function prettyTopic(topic) {
  var s = String(topic || "").replace(/[_\-/]+/g, " ").trim();
  return s ? s.charAt(0).toUpperCase() + s.slice(1) : t("Switch");
}

/* Names the palette gave a node, in both shipped languages plus the wire name
   and the pre-0.5 label. A node still wearing one of these is named after its
   TOPIC in Home Assistant instead — see name_is_default() in mqtt_bridge.c,
   which holds the same list on the other side and is the authority. Only the
   MQTT-button side needs the list; the Switch's own check is inline below and
   its label is "Switch" in both languages. */
var DEFAULT_TRIGGER_NAME =
  /^(mqtt[ -]?button|mqtt[ -]?taster|virtual trigger|virtueller ausl\u00f6ser|source\.virtual)$/i;

function isSwitch(n) { return !!n && n.type === "logic.switch"; }
function switchOn(n) { return !!n && n.enabled !== false; }

/* Blocked = an event travelling this link never arrives, because one of its
   two ends is a node the engine will not enter. One rule for every type, so a
   Switch turned OFF and a node disabled by hand look the same on the map --
   they behave the same. */
function linkBlocked(a, b) {
  return (!!a && a.enabled === false) || (!!b && b.enabled === false);
}

/* The one place a switch is moved, so the card, the editor and the canvas
   badge cannot drift apart -- and so all three go through the endpoint that
   does NOT rewrite flash on every toggle (see docs/API.md). */
function setSwitch(n, on, btn, msgNode) {
  if (btn) btn.disabled = true;
  setMsg(msgNode, on ? t("Switching on…") : t("Switching off…"));
  return postJSON("/api/graph/nodes/" + n.id + "/switch", { on: !!on })
    .then(function () {
      setMsg(msgNode, on ? t("On — this path conducts again.") : t("Off — this path is blocked."), "ok");
      return loadGraph().then(function () { return true; });
    }).catch(function (e) {
      setMsg(msgNode, e.message, "err");
      return false;
    }).then(function (ok) {
      if (btn) btn.disabled = false;
      return ok;
    });
}
function linkExists(from, to) {
  var ls = (S.graph && S.graph.links) || [];
  for (var i = 0; i < ls.length; i++) if (ls[i].from === from && ls[i].to === to) return true;
  return false;
}

/* ONE delete path, ONE confirmation, shared by the list card, the editor sheet
   and the ✕ on the map. The confirmation is what makes a bare ✕ on the canvas
   safe to offer at all — so it has to be this dialog, with this wording, and
   not a second one that could drift out of step with it. */
function deleteNodeConfirmed(n, onError, onDone) {
  var ty = nodeType(n.type);
  return confirmSheet(t("Delete “{name}”?", { name: n.name || ty.label }),
    [t("The node and every link to or from it are removed."),
     t("Stored signals are NOT touched — nothing has to be learned again.")],
    t("Delete node"), true).then(function (ok) {
    if (!ok) return false;
    return api("/api/graph/nodes/" + n.id, { method: "DELETE" }).then(function () {
      if (onDone) onDone();
      return loadGraph().then(function () { return true; });
    }).catch(function (e) {
      if (onError) onError(e);
      return false;
    });
  });
}

/* ------------------------------------------------------- monitor readout --

   A Monitor node is the only node whose whole value is what it LOOKS like, so
   its two pieces of chrome live here and are reused wherever the node appears:

     monitorLamp()      a dot that lights while the newest hit is inside hold_s
     monitorTimeline()  the last 10 minutes as an inline SVG strip

   Both are marked with a data attribute and refreshed in place by
   refreshMonitors() on every poll, rather than by re-rendering the cards. A
   card rebuild every second would fight the sheet, lose focus and thrash the
   DOM; a lamp is a class toggle, and CSS turns that into a fade. */

var MON_WINDOW_S = 600;   /* fallback until the box states its own retention */

/* A Monitor node only shows its chrome when the firmware actually serves
   /api/monitor — a 404 there hides the lamp and the strip everywhere at once. */
function isMonitor(n) { return !!n && n.type === "sink.monitor" && S.has.monitor; }

function monitorEntry(id) {
  return (S.monitor && S.monitor.by && S.monitor.by[id]) || null;
}
/* now_s advanced locally since the sample arrived — see loadMonitor(). */
function monitorNow() {
  if (!S.monitor) return 0;
  return S.monitor.now_s + (Date.now() - S.monitor.at) / 1000;
}
function monitorLit(id) {
  var e = monitorEntry(id);
  if (!e || !e.hits || !e.hits.length) return false;
  return (monitorNow() - e.hits[0]) < numOr(e.hold_s, 3);
}

function monitorLamp(id, cls) {
  var s = el("span", "lamp" + (cls ? " " + cls : ""));
  s.setAttribute("data-monlamp", id);
  s.setAttribute("role", "img");
  s.setAttribute("aria-label", t("Monitor indicator"));
  return s;
}

/* The strip. Newest on the RIGHT, because that is where "now" is on every
   timeline anyone has read before, and the eye should land on the most recent
   hit first.

   viewBox width is exactly the retention in seconds, so one unit is one second
   and a hit of hold_s seconds is hold_s units wide with no scaling maths. With
   preserveAspectRatio="none" the strip stretches to whatever width it is given
   — 328 px inside a card on a 360 px phone, more on a laptop — which is why the
   axis labels are HTML underneath rather than SVG text that would be squashed
   horizontally along with everything else. */
function monitorTimeline(id) {
  var e = monitorEntry(id);
  var win = numOr(e && e.retention_s, MON_WINDOW_S);
  var hold = numOr(e && e.hold_s, 3);
  var hits = (e && e.hits) || [];
  var now = monitorNow();

  var box = el("div", "montl");
  box.setAttribute("data-monstrip", id);

  var H = 26;
  var svg = svgEl("svg", "montl-svg");
  svg.setAttribute("viewBox", "0 0 " + win + " " + H);
  svg.setAttribute("preserveAspectRatio", "none");
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label",
    hits.length ? t("{n} hits in the last {min} minutes",
                    { n: hits.length, min: Math.round(win / 60) })
                : t("No hits in the last {min} minutes", { min: Math.round(win / 60) }));

  /* One tick a minute: enough to read "about four minutes ago" off the strip
     without turning it into graph paper. */
  for (var m = 60; m < win; m += 60) {
    var gl = svgEl("line", "montl-grid");
    gl.setAttribute("x1", win - m); gl.setAttribute("x2", win - m);
    gl.setAttribute("y1", "0"); gl.setAttribute("y2", H);
    add(svg, gl);
  }

  var drawn = 0;
  hits.forEach(function (hit) {
    var age = now - hit;
    if (age < 0) age = 0;
    if (age > win) return;
    /* A 3 s hold is 0.5% of a ten-minute strip — under two pixels on a phone,
       which is a mark you cannot see. Floor the drawn width so every hit is
       legible; the bar still grows with a longer hold. */
    var w = Math.max(5, hold);
    var x = Math.min(win - w, Math.max(0, win - age - w / 2));
    var r = svgEl("rect", "montl-hit");
    r.setAttribute("x", x.toFixed(1)); r.setAttribute("y", "3");
    r.setAttribute("width", w.toFixed(1)); r.setAttribute("height", H - 6);
    r.setAttribute("rx", "2");
    var tip = svgEl("title");
    tip.textContent = t("{ago} ago", { ago: shortDur(age) });
    add(r, tip);
    add(svg, r);
    drawn++;
  });
  add(box, svg);

  var ax = el("div", "montl-axis");
  add(ax, el("span", null, t("{min} min ago", { min: Math.round(win / 60) })));
  add(ax, el("span", null,
    drawn ? t(drawn === 1 ? "{n} hit" : "{n} hits",
              { n: drawn + (hits.length > drawn ? "+" : "") }) : ""));
  add(ax, el("span", null, t("now")));
  add(box, ax);

  if (!drawn) {
    add(box, el("div", "montl-empty",
      S.monitor ? t("Nothing yet — this monitor has not fired in the last {min} minutes.",
                    { min: Math.round(win / 60) })
                : t("Waiting for the box…")));
  }
  return box;
}

/* Called once per poll. Lamps are a class toggle; strips are rebuilt, which is
   correct rather than wasteful — the window rolls, so every mark moves left a
   pixel a second whether or not anything new arrived. */
function refreshMonitorLamps() {
  $$("[data-monlamp]").forEach(function (n) {
    var lit = monitorLit(parseInt(n.getAttribute("data-monlamp"), 10));
    if (n.classList) n.classList.toggle("on", lit);
  });
}
function refreshMonitors() {
  refreshMonitorLamps();
  $$("[data-monstrip]").forEach(function (n) {
    var id = parseInt(n.getAttribute("data-monstrip"), 10);
    var fresh = monitorTimeline(id);
    if (n.parentNode) n.parentNode.replaceChild(fresh, n);
  });
}

/* One place fires a node, so the list card, the editor sheet and the canvas ▶
   all behave identically — including refreshing the monitors straight away,
   which is what makes "click ▶, watch 💡 light" feel immediate instead of
   waiting out the next poll tick. */
function fireNode(id, btn, msgNode) {
  if (btn) btn.disabled = true;
  setMsg(msgNode, t("Firing…"));
  return postJSON("/api/graph/nodes/" + id + "/fire", {}).then(function () {
    setMsg(msgNode, t("Fired. Watch the monitors and the Activity feed for what it triggered."), "ok");
    if (S.has.monitor && monitorNodes().length) setTimeout(loadMonitor, 120);
    return true;
  }).catch(function (e) {
    setMsg(msgNode, e.message, "err");
    return false;
  }).then(function (ok) {
    if (btn) btn.disabled = false;
    return ok;
  });
}

var autoEls = {};   /* the graph */
var dashEls = {};   /* the Dashboard's two warnings and its box-status chips */

function buildDashboard() {
  if (S.built.dashboard) return;
  S.built.dashboard = true;
  var root = clear($("#tab-dashboard"));

  /* NO PROSE HERE. This screen is the working surface: three paragraphs
     explaining what a node is used to sit above the map, which on a phone put
     the thing you opened the app to look at below the fold, every single time,
     forever. All of it moved to the Handbook — which is the tab that exists for
     exactly this — and what is left is a title and one clause. */
  var p = el("div", "panel");
  var h = el("div", "panel-head");
  add(h, el("h2", null, t("Your box, as a flow")));
  add(h, el("p", "hint", t("Events run left to right. Tap a node to edit it.")));
  add(p, h);

  /* The two things that make the whole screen a lie if they are wrong: no
     radio, or no home network. They used to head the old Dashboard. */
  dashEls.statusNote = el("div");
  add(p, dashEls.statusNote);
  dashEls.txNote = el("div");
  add(p, dashEls.txNote);

  /* The box-status chips that used to sit under the feed are in the HEADER
     now — see buildStatusChips(). They are a fact about the box, not about this
     screen, and on the header they answer "is the radio even listening" from
     wherever you happen to be. What stays here is the pair of warnings, which
     ARE about this screen: with no radio or no network the map is a lie. */

  /* ONE toolbar row: ➕ Add node on the left, the view switch and the canvas
     zoom controls pushed right. The zoom buttons used to live in a second row
     inside the canvas panel, which meant two toolbars stacked above the map for
     no reason other than where the code that built them happened to be. */
  var topRow = el("div", "row toolbar");
  var addBtn = el("button", "btn primary", t("➕ Add node"));
  addBtn.type = "button";
  addBtn.addEventListener("click", openAddNode);
  add(topRow, addBtn);

  /* View switch: hidden below 900 px by CSS -- the list is the whole product
     on a phone, and the canvas is strictly additive.

     Which one OPENS by default follows the screen, because the right answer
     genuinely differs: on a desktop the map shows the whole flow at a glance,
     while on a phone it is a cramped picture you cannot edit. An explicit
     choice is remembered and wins on screens wide enough to honour it. */
  autoEls.viewSwitch = el("div", "segmented graph-viewswitch");
  var bList = el("button", null, t("List")); bList.type = "button";
  var bMap = el("button", null, t("Map")); bMap.type = "button";
  autoEls.bList = bList; autoEls.bMap = bMap;
  bList.addEventListener("click", function () { setGraphView("list", bList, bMap, true); });
  bMap.addEventListener("click", function () { setGraphView("map", bList, bMap, true); });
  /* Map first: it is the default on any screen wide enough to show this
     switch at all, and the leading position should match the default. */
  add(autoEls.viewSwitch, bMap, bList);
  add(topRow, autoEls.viewSwitch);

  /* renderCanvas() fills this; setGraphView() empties the row of it in List
     view. It is `:empty`-collapsed in CSS, so an absent zoom control leaves no
     gap and the view switch does not shift when the two views are swapped. */
  autoEls.zoomSlot = el("div", "zoomslot");
  add(topRow, autoEls.zoomSlot);
  add(p, topRow);
  add(root, p);

  autoEls.listWrap = el("div", "stack");
  add(root, autoEls.listWrap);
  autoEls.canvasWrap = el("div", "panel hidden");
  add(root, autoEls.canvasWrap);
  autoEls.empty = el("div", "empty",
    t("Nothing wired yet. Tap ➕ Add node and start with a Signal receiver for your doorbell."));
  add(root, autoEls.empty);

  setGraphView(defaultGraphView(), bList, bMap, false);
  watchGraphViewport();

  renderGraph();
  renderDashNotes();
}

function renderTxNote() {
  if (!dashEls.txNote) return;
  clear(dashEls.txNote);
  var warn = txBlockNote();
  if (warn) add(dashEls.txNote, warn);
}

/* ---------------------------------------------------- header status chips --

   Six chips, right-aligned on their own header line. The uptime one relabels
   itself every second, which is exactly why none of this is built on a tick.

   TWO separate things make a 1 Hz row flicker, and both are dealt with here:

   1. REBUILDING. clear() + re-create at 1 Hz repaints the whole row every
      second and, on a phone, is visible as a shimmer. So the chips are created
      ONCE, kept in `hdrChips`, and every update assigns .textContent on an
      element that already exists. The 1 Hz path touches exactly one string.

   2. REFLOW. "up 9s" -> "up 10s" and "up 59m 59s" -> "up 1h 0m" change the
      chip's WIDTH, and in a right-aligned row every chip to its left jumps.
      tabular-nums stops digits from changing width, and a min-width sized for
      the widest plausible value stops the chip itself from changing width. The
      dBm floor gets the same treatment: it moves too.

   Nothing is ever shown stale or invented. A value the box has not reported --
   no radio, no /api/radio, no Wi-Fi -- either says so in words or the chip is
   removed from the row entirely. */

var hdrChips = {};

function buildStatusChips() {
  var host = $("#status-chips");
  if (!host || hdrChips.host) return;
  hdrChips.host = host;

  /* TWO EXPLICIT LINES, not one wrapping row.

     Natural wrapping would put the break wherever the text happened to end,
     and two of these chips change their text every second — so the break point
     itself would move, and a chip would hop between lines while you watched.
     Fixing the composition of each line in the DOM makes the split a fact
     rather than a consequence.

     The split is also the right one to read: line one is what the box IS
     (radio state, which chip, which network) and line two is what it is
     MEASURING (frequency, noise floor, uptime). Every value that ticks is on
     line two, and both of those chips are min-width locked, so line two can
     never change width either. Line one is set by the SSID and does not move
     while you look at it. The block's width is therefore constant, which is
     what stops the brand and the tabs beside it from twitching. */
  var lineA = el("div", "sc-line");
  var lineB = el("div", "sc-line");
  add(host, lineA, lineB);

  function chip(line, cls) {
    var c = el("span", "chip " + cls);
    c.__base = "chip " + cls;    /* the state class (ok/bad/warn) is appended */
    c.__txt = null;
    /* The text node is created HERE, once, and every later update writes its
       nodeValue. `el.textContent = s` would replace it instead — a childList
       mutation per chip per second, which is real DOM churn even though it
       looks like a string assignment. */
    c.__node = document.createTextNode("");
    c.appendChild(c.__node);
    add(line, c);
    return c;
  }
  /* sc-t2 / sc-t3 are the tiers CSS drops as the header gets tight. */
  /* Grouped by subject, and the ticking values stay on line B.
     Line A is the NETWORK side (which Wi-Fi, which broker); line B is the RADIO
     side (which chip, which frequency, how noisy) with uptime last. Both chips
     whose text changes every second — the noise floor and the uptime — are on
     line B and min-width locked, so that line can never change width; line A is
     set by the SSID and does not move while you look at it. */
  hdrChips.wifi  = chip(lineA, "sc-t2");
  hdrChips.mqtt  = chip(lineA, "sc-t2");
  hdrChips.radio = chip(lineA, "sc-radio");
  hdrChips.part  = chip(lineB, "mono sc-t3");
  hdrChips.freq  = chip(lineB, "mono sc-t3");
  hdrChips.floor = chip(lineB, "mono sc-floor sc-t2");
  hdrChips.up    = chip(lineB, "mono sc-up");
}

/* text === null removes the chip. Both writes are guarded, so a tick that
   changes nothing performs no DOM write at all. */
function setHdrChip(node, text, state) {
  if (!node) return;
  var cls = node.__base + (text === null ? " hidden" : (state ? " " + state : ""));
  if (text !== null && node.__txt !== text) { node.__txt = text; node.__node.nodeValue = text; }
  if (node.className !== cls) node.className = cls;
}

function renderStatusChips(err) {
  buildStatusChips();
  if (!hdrChips.host) return;
  /* `err` matters as much as `!sys`: a failed /api/system leaves S.sys holding
     the last good reading, and presenting that as the current state is the one
     lie this row exists to avoid. */
  var sys = err ? null : S.sys;

  if (S.recovery) { hdrChips.host.classList.add("hidden"); return; }
  hdrChips.host.classList.remove("hidden");

  if (!sys) {
    /* Everything else on the row would be the last known value of a box that
       is no longer answering, which is the one thing a status row must not
       show. So the row collapses to the single true statement. */
    setHdrChip(hdrChips.radio, t("● Box not reachable"), "bad");
    setHdrChip(hdrChips.part, null);
    setHdrChip(hdrChips.freq, null);
    setHdrChip(hdrChips.floor, null);
    setHdrChip(hdrChips.wifi, null);
    setHdrChip(hdrChips.mqtt, null);
    setHdrChip(hdrChips.up, null);
    return;
  }

  var radio = sys.radio || {};
  if (radio.present === false) setHdrChip(hdrChips.radio, t("● No radio"), "bad");
  else setHdrChip(hdrChips.radio, t("● Radio listening"), "ok");

  setHdrChip(hdrChips.part, typeof radio.version === "number"
    ? ("CC1101 v0x" + (radio.version || 0).toString(16) + " p" + numOr(radio.partnum, 0))
    : null);

  setHdrChip(hdrChips.freq,
    (S.radio && typeof S.radio.freq_hz === "number") ? fmtHz(S.radio.freq_hz) : null);
  setHdrChip(hdrChips.floor,
    (S.radio && typeof S.radio.rssi_dbm === "number")
      ? t("{dbm} dBm floor", { dbm: S.radio.rssi_dbm }) : null);

  setHdrChip(hdrChips.wifi,
    sys.sta_connected ? ("Wi-Fi " + (sys.sta_ssid || "?")) : t("AP only"),
    sys.sta_connected ? "ok" : "warn");

  /* Only when MQTT is switched on. A chip reading "MQTT off" on the majority of
     boxes that never enable it is noise, not status — and this row has to earn
     every pixel. Enabled but not connected is worth shouting about, though:
     that is the state where Home Assistant has silently stopped hearing presses.
     A firmware without the field at all (older build) also shows nothing. */
  var mq = sys.mqtt;
  if (!mq || !mq.enabled) setHdrChip(hdrChips.mqtt, null);
  else setHdrChip(hdrChips.mqtt, mq.connected ? "MQTT" : "MQTT offline",
                  mq.connected ? "ok" : "bad");

  tickUptimeChip();
}

/* The whole of the 1 Hz path: one comparison and, at most, one string
   assignment. No layout is invalidated because .sc-up cannot change width. */
function tickUptimeChip() {
  if (!hdrChips.up || S.recovery) return;
  var up = uptimeNow();
  setHdrChip(hdrChips.up, up === null ? null : t("up {d}", { d: durText(up) }));
}

/* ------------------------------------------------------- dashboard notes --
   What is left on the Dashboard once the chips have gone: the two conditions
   that make the map itself untrustworthy. */
function renderDashNotes() {
  if (!dashEls.statusNote) return;
  var sys = S.sys;

  renderTxNote();

  var note = clear(dashEls.statusNote);
  if (!sys) return;
  var radio = sys.radio || {};
  if (radio.present === false) {
    add(note, el("div", "note bad",
      t("The CC1101 module did not answer on SPI, so this box can neither receive nor " +
        "transmit. Check the 3V3/GND and the four SPI wires, then open Diagnostics for " +
        "the exact probe result.")));
  } else if (!sys.sta_connected) {
    add(note, el("div", "note warn",
      t("No home Wi-Fi connection. The box works normally over its own access point " +
        "{ap}but is not reachable from your LAN. Add a network under Settings.",
        { ap: sys.ap_ssid ? "(" + sys.ap_ssid + ") " : "" })));
  }
}

/* ======================================================================
   ACTIVITY -- the live event feed, a whole tab of it

   This used to be a panel wedged between the graph and the recipes, capped at
   five rows with a "Show all" button, because anything taller pushed the map
   off the screen. On its own page that cap is pure obstruction: the list is the
   page, so it is shown whole and scrolls.

   What survived the move -- and is the reason the move was worth making -- is
   the filtering. Search and the kind filter are built ONCE, in buildActivity(),
   and renderFeed() only ever replaces the <ul> beneath them. The feed repaints
   every 2 s; an input rebuilt on that cadence would drop focus and swallow the
   word you were halfway through typing.
   ====================================================================== */

var actEls = {};   /* the feed, its filters and its result counter */

var EVENT_KINDS = {
  rf_unmatched: { ico: "❓", label: "Unrecognised signal" },
  button_press: { ico: "🔔", label: "Button press" },
  wired_press: { ico: "🔌", label: "Wired button" },
  node_fired: { ico: "↳", label: "Node fired" },
  transmit: { ico: "📡", label: "Transmit" },
  /* The wire name is still "learn"; what it means, and always meant, is that a
     signal was registered. */
  learn: { ico: "🎓", label: "Signal registered" },
  system: { ico: "⚙", label: "System" }
};

/* The box keeps 60 events; this is the ceiling on how many are ever put in the
   DOM at once, so a firmware that one day returns more cannot turn a scroll
   into a stall. */
var FEED_MAX = 200;

function buildActivity() {
  if (S.built.activity) return;
  S.built.activity = true;
  var root = clear($("#tab-activity"));

  var p = el("div", "panel");
  var h = el("div", "panel-head");
  add(h, el("h2", null, t("Activity")));
  add(h, el("p", null,
    t("Everything the receiver hears and everything the nodes do about it, as it happens. " +
      "The radio listens continuously; a listening session only decides what happens to a " +
      "signal it does not recognise.")));
  add(p, h);

  var tools = el("div", "feedtools");

  actEls.feedSearch = inputEl("search", "", { placeholder: t("Search activity…") });
  actEls.feedSearch.setAttribute("aria-label", t("Search activity"));
  actEls.feedSearch.addEventListener("input", renderFeed);

  actEls.feedKind = selectEl(
    [{ value: "", label: t("All kinds") }].concat(
      Object.keys(EVENT_KINDS).map(function (k) {
        return { value: k, label: EVENT_KINDS[k].ico + " " + t(EVENT_KINDS[k].label) };
      })), "");
  actEls.feedKind.setAttribute("aria-label", t("Filter by kind"));
  actEls.feedKind.addEventListener("change", renderFeed);

  /* One tap back to everything. Without it, clearing a filter on a phone means
     selecting the search text and deleting it AND remembering the kind select
     is still set — two operations to undo what one tap set up. It hides itself
     when nothing is filtered, so it is never a control that does nothing. */
  actEls.feedClear = el("button", "btn feedclear", t("✕ Clear"));
  actEls.feedClear.type = "button";
  actEls.feedClear.addEventListener("click", function () {
    actEls.feedSearch.value = "";
    actEls.feedKind.value = "";
    renderFeed();
    actEls.feedSearch.focus();
  });

  add(tools, actEls.feedSearch, actEls.feedKind, actEls.feedClear);
  add(p, tools);

  actEls.feedCount = el("div", "feedcount");
  add(p, actEls.feedCount);

  actEls.feed = el("ul", "feed");
  add(p, actEls.feed);
  actEls.feedEmpty = el("div", "empty", t("Nothing heard yet. Press a doorbell button."));
  add(p, actEls.feedEmpty);
  add(root, p);

  renderFeed();
}

/* Relabel the age column in place. Cheap, and it deliberately does NOT call
   renderFeed() — a full re-render once a second would fight the search box for
   focus and reset the scroll position mid-read. */
function tickFeedClock() {
  if (!S.built.activity || !actEls.feed) return;
  $$(".fi-age", actEls.feed).forEach(function (n) {
    var ts = parseFloat(n.dataset.ts);
    if (isFinite(ts)) n.textContent = agoText(ts);
  });
}

function renderFeed() {
  if (!actEls.feed) return;
  var ul = clear(actEls.feed);

  var q = (actEls.feedSearch ? actEls.feedSearch.value : "").trim().toLowerCase();
  var kindSel = actEls.feedKind ? actEls.feedKind.value : "";

  var matched = S.events.filter(function (ev) {
    if (kindSel && ev.kind !== kindSel) return false;
    if (!q) return true;
    var k = EVENT_KINDS[ev.kind];
    /* Search the text, the kind's wire name and its human label -- in BOTH
       languages, so both "transmit" and "Transmit" find the same rows, and so a
       German reader typing the word they can actually see finds it too. */
    var hay = ((ev.text || "") + " " + (ev.kind || "") + " " +
               (k ? k.label + " " + t(k.label) : "")).toLowerCase();
    return hay.indexOf(q) !== -1;
  });

  var filtering = !!(q || kindSel);
  var shown = matched.slice(0, FEED_MAX);

  if (actEls.feedClear) actEls.feedClear.classList.toggle("hidden", !filtering);

  /* The count is what makes a filter honest: "3 of 60" says both that the
     filter worked and that there is more behind it. Unfiltered it states the
     depth of the box's own buffer, which is the other thing people ask. */
  if (actEls.feedCount) {
    clear(actEls.feedCount);
    if (S.events.length) {
      actEls.feedCount.textContent = filtering
        ? t("{n} of {total} events", { n: matched.length, total: S.events.length })
        : t("{n} events, newest first", { n: S.events.length });
    }
  }

  actEls.feedEmpty.classList.toggle("hidden", matched.length > 0);
  if (matched.length === 0)
    actEls.feedEmpty.textContent = filtering
      ? t("Nothing in the feed matches that filter.")
      : t("Nothing heard yet. Press a doorbell button.");

  shown.forEach(function (ev) {
    var kind = EVENT_KINDS[ev.kind] || { ico: "·", label: ev.kind || "event" };
    var li = el("li", "feeditem k-" + (ev.kind || "other"));
    add(li, el("div", "fi-ico", kind.ico));
    var body = el("div", "fi-body");
    add(body, el("div", "fi-text", ev.text || t(kind.label)));
    var meta = [];
    if (ev.kind && !ev.text) meta.push(t(kind.label));
    else if (ev.kind) meta.push(ev.kind);
    if (typeof ev.rssi_dbm === "number") meta.push(ev.rssi_dbm + " dBm");
    if (typeof ev.repeats === "number" && ev.repeats) meta.push(ev.repeats + "x");
    if (meta.length) add(body, el("div", "fi-meta", meta.join("  ·  ")));
    add(li, body);
    var age = el("div", "fi-age", agoText(ev.ts_s));
    age.dataset.ts = String(numOr(ev.ts_s, 0));
    add(li, age);
    add(ul, li);
  });
}

/* Must match the `min-width: 900px` rule that reveals .graph-viewswitch in
   style.css. If the two ever disagree, the map can be selected on a screen where
   the switch is invisible and the user has no way back to the list. */
var GRAPH_MAP_MQ = "(min-width: 900px)";
var GRAPH_VIEW_KEY = "klingelbox-graphview";

function canvasUsable() {
  return window.matchMedia ? window.matchMedia(GRAPH_MAP_MQ).matches : true;
}

function storedGraphView() {
  try {
    var v = localStorage.getItem(GRAPH_VIEW_KEY);
    return (v === "list" || v === "map") ? v : null;
  } catch (e) { return null; }
}

/* Map on a real screen, list on a phone or tablet — unless the user has said
   otherwise, in which case that stands (but only where the canvas is reachable). */
function defaultGraphView() {
  if (!canvasUsable()) return "list";
  return storedGraphView() || "map";
}

/* Rotating a tablet or narrowing a window can take the canvas away underneath a
   user who is looking at it; drop them back to the list rather than leaving a
   hidden pane and no visible switch. */
function watchGraphViewport() {
  if (!window.matchMedia) return;
  var mq = window.matchMedia(GRAPH_MAP_MQ);
  var onChange = function () {
    if (!autoEls.bList) return;
    if (!mq.matches && S.graphView === "map")
      setGraphView("list", autoEls.bList, autoEls.bMap, false);
    else if (mq.matches && S.graphView === "list" && !storedGraphView())
      setGraphView("map", autoEls.bList, autoEls.bMap, false);
  };
  if (mq.addEventListener) mq.addEventListener("change", onChange);
  else if (mq.addListener) mq.addListener(onChange);
}

function setGraphView(v, bList, bMap, persist) {
  S.graphView = v;
  bList.classList.toggle("active", v === "list");
  bMap.classList.toggle("active", v === "map");
  autoEls.listWrap.classList.toggle("hidden", v !== "list");
  autoEls.canvasWrap.classList.toggle("hidden", v !== "map");
  /* The zoom controls belong to the canvas, so they leave the toolbar with it.
     Emptying rather than hiding keeps :empty collapse honest. */
  if (autoEls.zoomSlot && v !== "map") clear(autoEls.zoomSlot);
  if (persist) {
    try { localStorage.setItem(GRAPH_VIEW_KEY, v); } catch (e) { /* private mode */ }
  }
  if (v === "map") renderCanvas();
}

function renderGraph(err) {
  if (!autoEls.listWrap) return;
  var wrap = clear(autoEls.listWrap);

  if (err) {
    add(wrap, el("div", "note bad",
      t("Could not read the node graph: {msg}", { msg: err.message })));
    autoEls.empty.classList.add("hidden");
    return;
  }
  var nodes = (S.graph && S.graph.nodes) || [];
  var links = (S.graph && S.graph.links) || [];
  autoEls.empty.classList.toggle("hidden", nodes.length > 0);

  /* Sources, then logic, then sinks: that is the direction of flow, and on a
     phone the reading order IS the diagram. There used to be a fourth rank
     between sources and logic for the two-ported signal node, which could never
     honestly be in one place in a left-to-right list; a Signal receiver sorts
     with the sources and a Signal sender with the sinks, because that is what
     they now are. `|| 0` and not `numOr`: an unknown group must sort somewhere
     definite, because an undefined here makes the comparator return NaN and the
     whole list order arbitrary. */
  var order = { source: 0, logic: 1, sink: 2 };
  nodes.slice().sort(function (a, b) {
    var d = (order[nodeType(a.type).g] || 0) - (order[nodeType(b.type).g] || 0);
    return d !== 0 ? d : (a.id - b.id);
  }).forEach(function (n) {
    add(wrap, nodeCard(n, links));
  });

  if (S.graphView === "map") renderCanvas();
  /* Freshly built lamps start dark; give them the state we already hold rather
     than making them wait out a poll tick. The strips were just drawn from the
     same data, so only the lamps need it. */
  refreshMonitorLamps();
}

function noteRow(text) {
  var d = el("div", "hint");
  d.textContent = text;
  return d;
}

function nodeSummary(n) {
  switch (n.type) {
    /* One direction each, and the summary says which. The send parameters
       appear only on the sender, because they are the only node they act on. */
    case "signal.rx": {
      var rn = signalName(n.signal_id);
      if (!rn) return n.signal_id ? t("Signal {id} (missing from the store)", { id: n.signal_id })
                                  : t("No signal chosen yet");
      return t("{name} · fires when this code is heard on air", { name: rn });
    }
    case "signal.tx": {
      var tn = signalName(n.signal_id);
      if (!tn) return n.signal_id ? t("Signal {id} (missing from the store)", { id: n.signal_id })
                                  : t("No signal chosen yet");
      return t("{name} · sends this code when triggered ({repeats}x, {gap} us gap)",
               { name: tn, repeats: numOr(n.repeats, 6), gap: numOr(n.gap_us, 8000) });
    }
    case "source.gpio":
      return (n.gpio_pin >= 0 ? "GPIO " + n.gpio_pin : t("No pin chosen")) +
        " · " + (n.gpio_active_low === false ? t("active high") : t("active low")) +
        " · " + t("{ms} ms debounce", { ms: numOr(n.gpio_debounce_ms, 50) });
    case "source.virtual": {
      /* No new badge and no extra line: the subtitle already had a slot for the
         topic, and naming a topic nothing is listening on would be a lie. */
      if (n.mqtt_enabled === false) return t("Not on MQTT \u00b7 fired from this page or the REST API");
      /* Blank follows the NAME, exactly as the firmware resolves it — the card
         showed "fired from this page only" for a node Home Assistant could
         press perfectly well. */
      var vsfx = n.topic || topicSlug(n.name);
      return vsfx
        ? t("Home Assistant can press it \u00b7 {topic}", { topic: mqttTriggerTopic(vsfx) })
        : t("No name and no topic \u2014 fired from this page or the REST API only");
    }
    case "source.any_rf":
      return t("Fires on every received burst — registered buttons and strangers alike");
    /* The window is named only where it is used. Printing "window 1 s" beside
       mode ANY described a rule the firmware does not have: ANY passes the
       first thing through immediately and never looks at the clock. */
    case "logic.group":
      return n.group_mode === "all"
        ? t("ALL — fires only when every input has fired within {s} s", { s: numOr(n.window_s, 1) })
        : t("ANY — a merge point: anything arriving passes straight through");
    case "logic.throttle":
      return t("Rings once, then ignores presses for {s} s", { s: numOr(n.window_s, 10) });
    case "logic.repeat": {
      /* "1x" would read as a setting the user got wrong; it is a legal
         pass-through, so say what it actually does. */
      var times = numOr(n.repeats, 3);
      return times <= 1
        ? t("Passes through once (no repeat)")
        : t("Rings {times}x total, {s} s apart", { times: times, s: numOr(n.window_s, 5) });
    }
    case "logic.switch": {
      /* Position first, because that is what anyone scanning the list is
         after. What MOVES it comes next, and the radio is named before the
         broker: a code on air is the surprising one, and the summary is the
         only place it shows without opening the node. */
      var swSub = switchOn(n) ? t("ON — events pass through")
                              : t("OFF — everything past it is blocked");
      var csid = numOr(n.signal_id, 0);
      if (csid) {
        var csn = signalName(csid);
        swSub += " · " + (csn ? t("toggled by “{name}”", { name: csn })
                              : t("toggled by signal {id} (missing)", { id: csid }));
      }
      return swSub + (n.mqtt_enabled === false ? " · " + t("not on MQTT")
               : (n.topic ? (" · " + mqttSwitchTopic(n.topic, "set")) : " · " + t("no MQTT topic")));
    }
    case "sink.mqtt":
      if (n.mqtt_enabled === false) return t("Not on MQTT — publishes nothing");
      return n.topic ? t("Publishes to {topic}", { topic: mqttPublishTopic(n.topic) }) : t("No topic set");
    case "sink.monitor":
      return t("Watches only — nothing is sent or published. Lamp stays lit {s} s per hit.",
               { s: numOr(n.window_s, 3) });
    default:
      return "";
  }
}
/* The real base topic always comes from GET /api/config; "klingelbox" is only
   the illustrative placeholder shown before that answer arrives. */
function mqttBase() {
  var b = S.config && S.config.mqtt && S.config.mqtt.base_topic;
  return b || "klingelbox";
}
function mqttEnabled() { return !!(S.config && S.config.mqtt && S.config.mqtt.enabled); }
function mqttTriggerTopic(topic) { return mqttBase() + "/trigger/" + (topic || ""); }
function mqttPublishTopic(topic) { return mqttBase() + "/" + (topic || ""); }
/* leaf is "set" (what Home Assistant writes) or "state" (what it reads back,
   retained). Both are shown to the user, because an automation may well want
   the state one directly. */
function mqttSwitchTopic(topic, leaf) {
  return mqttBase() + "/switch/" + (topic || "") + "/" + leaf;
}

function nodeCard(n, links) {
  var ty = nodeType(n.type);
  /* A Switch that is OFF is not a "disabled" card -- it is a working node doing
     its job. It gets its own class so the styling can say "blocked" rather than
     "greyed out and ignore me". */
  var sw = isSwitch(n);
  var c = el("div", "card nodecard g-" + ty.g +
    (n.enabled === false ? (sw ? " swoff" : " off") : ""));

  var head = el("div", "card-head");
  add(head, el("span", "li-ico", ty.ico));
  var main = el("div", "card-title");
  add(main, document.createTextNode(n.name || ty.label));
  var wrapT = el("div");
  wrapT.style.flex = "1";
  wrapT.style.minWidth = "0";
  add(wrapT, main, el("div", "node-type", ty.label));
  add(head, wrapT);
  if (sw) add(head, el("span", "chip " + (switchOn(n) ? "ok" : "bad"),
                       switchOn(n) ? t("ON") : t("OFF")));
  else if (n.enabled === false) add(head, el("span", "chip warn", t("disabled")));
  /* The lamp belongs in the head, beside the name: on a phone the list IS the
     diagram, so this is where you watch a chain fire. */
  if (isMonitor(n)) add(head, monitorLamp(n.id));
  add(c, head);

  add(c, el("div", "card-sub", nodeSummary(n)));

  /* The strip inline on the card, not only in the editor. It is 26 px tall and
     full-bleed, so it costs one line and does not crowd anything even at
     360 px — and a monitor whose history you have to open a sheet to see is a
     monitor you stop looking at. */
  if (isMonitor(n)) add(c, monitorTimeline(n.id));

  /* --- links as chips --- */
  var ins = links.filter(function (l) { return l.to === n.id; });
  var outs = links.filter(function (l) { return l.from === n.id; });

  if (hasInput(n)) add(c, linkGroup(t("Inputs"), ins, n, "in"));
  if (hasOutput(n)) add(c, linkGroup(t("Outputs"), outs, n, "out"));

  var msg = el("div", "formmsg");
  var row = el("div", "btnrow");
  var edit = el("button", "btn", t("Edit"));
  edit.type = "button";
  edit.addEventListener("click", function () { openNodeEditor(n); });
  /* Firing a node makes that node do ITS OWN thing, and since the split there
     is only ever one of those per type. On a Signal receiver that is "pretend
     this code was just heard": its output fires, the chain runs, and nothing
     goes on air. On a Signal sender it is a real transmission.

     Two acts, two glyphs, everywhere: 📡 sends a code OUT over the air, 📥
     pretends one came IN. The card spends the glyph rather than a ▶ precisely
     because it has room for it — one press of 📡 can ring a chime in someone's
     house, and a button that says which way it goes is worth two characters. */
  /* On an MQTT button this is not a debug affordance, it is the node's
     entire purpose — being fired by hand is what the type is FOR — so it reads
     as the primary action of the card rather than a test button. */
  var virt = n.type === "source.virtual";
  var fire;
  if (sw) {
    /* On a Switch the primary action of the card is not firing it -- it is
       moving it. "Test fire" would start a traversal AT the switch, which
       walks straight past the very thing the node exists to control, so it
       would be a button that lies. */
    fire = el("button", "btn " + (switchOn(n) ? "danger" : "primary"),
              switchOn(n) ? t("Switch OFF") : t("Switch ON"));
    fire.type = "button";
    fire.addEventListener("click", function () { setSwitch(n, !switchOn(n), fire, msg); });
  } else {
    fire = el("button", "btn" + (virt ? " primary" : ""),
      virt ? t("▶ Trigger")
        : isSignalRx(n) ? t("📥 Simulate heard")
        : isSignalTx(n) ? t("📡 Transmit now")
        : t("Test fire"));
    fire.type = "button";
    fire.addEventListener("click", function () { fireNode(n.id, fire, msg); });
  }
  var del = el("button", "btn danger", t("Delete"));
  del.type = "button";
  del.addEventListener("click", function () {
    deleteNodeConfirmed(n, function (e) { setMsg(msg, e.message, "err"); });
  });
  add(row, edit, fire, del);
  add(c, row, msg);
  return c;
}

/* A link group is the phone replacement for wires: chips you tap to remove,
   plus a dashed chip that opens a picker to add one. */
function linkGroup(label, list, n, dir) {
  var g = el("div", "linkgroup");
  add(g, el("div", "lg-label", label));
  var row = el("div", "chiprow");
  list.forEach(function (l) {
    var otherId = dir === "in" ? l.from : l.to;
    var other = nodeById(otherId);
    var chip = el("button", "linkchip" + (other ? "" : " dangling"));
    chip.type = "button";
    add(chip, el("span", "lc-text",
      (dir === "in" ? "← " : "→ ") + nodeName(otherId)));
    add(chip, el("span", "lc-x", "✕"));
    chip.setAttribute("aria-label", dir === "in"
      ? t("Remove link from {name}", { name: nodeName(otherId) })
      : t("Remove link to {name}", { name: nodeName(otherId) }));
    chip.addEventListener("click", function () {
      confirmSheet(t("Remove this link?"),
        [(dir === "in" ? nodeName(otherId) + "  →  " + nodeName(n.id)
                       : nodeName(n.id) + "  →  " + nodeName(otherId)),
         t("Both nodes stay; only the connection between them goes away.")],
        t("Remove link"), true).then(function (ok) {
        if (!ok) return;
        delJSON("/api/graph/links", { from: l.from, to: l.to }).then(loadGraph)
          .catch(function (e) { alertSheet(t("Could not remove the link"), e.message); });
      });
    });
    add(row, chip);
  });

  var addChip = el("button", "linkchip add");
  addChip.type = "button";
  add(addChip, el("span", "lc-text", dir === "in" ? t("＋ Add input") : t("＋ Add output")));
  addChip.addEventListener("click", function () { openLinkPicker(n, dir); });
  add(row, addChip);
  add(g, row);
  return g;
}

function alertSheet(title, text) {
  var sh = openSheet(title);
  add(sh.body, el("p", "small", text));
  var foot = el("div", "formfoot");
  var ok = el("button", "btn primary", t("OK")); ok.type = "button";
  ok.addEventListener("click", sh.close);
  add(foot, ok);
  add(sh.body, foot);
}

function openLinkPicker(n, dir) {
  var nodes = (S.graph && S.graph.nodes) || [];
  var items = [];
  nodes.forEach(function (o) {
    if (o.id === n.id) return;
    var from = dir === "in" ? o.id : n.id;
    var to = dir === "in" ? n.id : o.id;
    var fromN = nodeById(from), toN = nodeById(to);
    if (!fromN || !toN) return;
    if (!hasOutput(fromN) || !hasInput(toN)) return;
    var dup = linkExists(from, to);
    var ty = nodeType(o.type);
    items.push({
      value: o.id, icon: ty.ico,
      label: o.name || ty.label,
      sub: dup ? t("Already linked") : nodeSummary(o),
      meta: ty.label,
      disabled: dup
    });
  });
  if (!items.length) {
    alertSheet(dir === "in" ? t("Nothing can feed this node") : t("Nothing this node can feed"),
      dir === "in"
        ? t("Add a source or a logic node first — sinks have no output to connect from.")
        : t("Add a logic or sink node first — sources have no input to connect to."));
    return;
  }
  pickerSheet(dir === "in" ? t("Pick the node that feeds {name}", { name: n.name || nodeType(n.type).label })
                           : t("Pick the node to send to"),
    dir === "in" ? t("It will fire this node when it triggers.")
                 : t("This node will fire it when it triggers."),
    items, function (otherId) {
      var body = dir === "in" ? { from: otherId, to: n.id } : { from: n.id, to: otherId };
      postJSON("/api/graph/links", body).then(loadGraph)
        .catch(function (e) { alertSheet(t("Could not add the link"), e.message); });
    });
}

/* Sources on the left, logic in the middle, sinks on the right — the layout
   mirrors the direction events actually travel.

   Three columns, one per group, and no special case left. The two-ported signal
   node needed a fourth column of its own because there was no honest answer for
   where a node that is both ends of a chain belongs; a Signal receiver drops in
   with the sources and a Signal sender with the sinks, which is where each of
   them genuinely sits in the flow.

   The columns are at least 200 apart because a node box is 168 wide: any
   tighter and they would visibly overlap. Existing nodes keep whatever ui_x
   they were saved with; this only decides where the NEXT one is dropped.

   Counting nodes in the column is not enough to pick a free row: delete the
   second of three and the next node lands on top of the third. So walk down the
   column and take the first row nothing already occupies. */
function nextPosition(group) {
  var nodes = (S.graph && S.graph.nodes) || [];
  var col = { source: 40, logic: 440, sink: 640 }[group] || 440;
  var ROW = 96;
  for (var row = 0; row < 40; row++) {
    var y = 30 + row * ROW;
    var taken = nodes.some(function (n) {
      return Math.abs(numOr(n.ui_x, 40) - col) < 80 && Math.abs(numOr(n.ui_y, 40) - y) < 60;
    });
    if (!taken) return { x: col, y: y };
  }
  return { x: col, y: 30 };
}

function openAddNode() {
  var items = NODE_TYPES.filter(function (ty) {
    return !(ty.t === "source.gpio" && !S.has.gpio) &&
           !(ty.t === "sink.monitor" && !S.has.monitor);
  }).map(function (ty) {
    return {
      value: ty.t, icon: ty.ico, label: t(ty.label), sub: t(ty.help), meta: t(ty.g)
    };
  });
  pickerSheet(t("Add a node"),
    t("Sources hear things, logic shapes, sinks act. A 433 MHz code needs a receiver to " +
      "hear it and a sender to say it — pick whichever end you are wiring."),
    items, function (type) {
    /* A node that needs a signal is never created empty: the signal IS the
       node's identity, so the choice of one is part of adding it. Both halves
       of the pair go through the same flow, told apart only by which type it
       ends up creating. */
    if (type === "signal.rx" || type === "signal.tx") {
      openSignalNodeFlow(type);
      return;
    }
    createNode(type, null);
  });
}

/* Creating the node, optionally already bound to the signal that was just
   learned, picked or synthesized. The node is named after that signal, because
   "Front door" is what the user just typed and "Signal" is not. */
function createNode(type, sig) {
  var ty = nodeType(type);
  var pos = nextPosition(ty.g);
  /* A repeat node reads both of these differently — 3 emissions 5 s apart, not
     6 frame copies and a 10 s cooldown — so it gets its own starting point
     rather than being created wrong and corrected in the editor. */
  var rep = (type === "logic.repeat");
  /* window_s means three different things depending on the type, so each one
     starts from its own number rather than from a shared 10 that is wrong for
     two of them: a repeat INTERVAL, a monitor's lamp HOLD, a cooldown. */
  var win = rep ? 5 : (type === "sink.monitor" ? 3 : 10);
  var body = {
    type: type, name: sig ? signalLabel(sig) : ty.label, enabled: true,
    signal_id: sig ? sig.id : 0, gpio_pin: -1, gpio_active_low: true, gpio_debounce_ms: 50,
    repeats: rep ? 3 : 6, gap_us: 8000, window_s: win, group_mode: "any",
    topic: "", mqtt_enabled: true, ui_x: pos.x, ui_y: pos.y
  };
  return postJSON("/api/graph/nodes", body).then(function (created) {
    return loadGraph().then(function () {
      var n = created && created.id ? nodeById(created.id) : null;
      if (n) openNodeEditor(n);
    });
  }).catch(function (e) { alertSheet(t("Could not add the node"), e.message); });
}

/* The fork that replaces the old Learn tab and the old Signals tab in one
   question: where does this node's signal come from? Every branch ends with a
   node that already works, in one flow and one confirmation. */
function openSignalNodeFlow(type) {
  var rx = (type !== "signal.tx");
  function listen() {
    openListenFlow({
      title: t("Listen for a signal"),
      sub: rx
        ? t("Press the remote button a few times. Everything heard is kept and ranked; pick " +
            "the one that is yours, test it, name it — this node then fires whenever it is " +
            "heard again.")
        : t("Press the remote button a few times. Everything heard is kept and ranked; pick " +
            "the one that is yours, test it, name it — this node then sends that same code " +
            "whenever it is triggered.")
    }).then(function (sig) { if (sig) createNode(type, sig); });
  }
  function virtual() {
    openVirtualFlow({ mode: "signal" })
      .then(function (sig) { if (sig) createNode(type, sig); });
  }
  function existing() {
    openSignalPicker({
      title: t("Signal for this node"),
      onPick: function (sig) { createNode(type, sig); },
      onListen: S.has.raw ? listen : null,
      onVirtual: virtual
    });
  }

  /* Listen · Select · Configure, in that order: the common case first, the
     store second, and hand-entry last for the case where the code is known but
     the remote is not in reach. */
  /* Listening is the first option whenever the firmware has it. Older firmware
     is served the other two rather than a button that 404s — the probe at boot
     is what makes that decision possible here. */
  var items = [];
  if (S.has.raw) items.push(
    { value: "listen", icon: "🎧", label: t("Listen for a new button"),
      sub: t("Press your remote a few times. Everything heard is ranked, and you pick the one "
        + "that rings your bell — decoded or not."),
      meta: t("capture") });
  items = items.concat([
    { value: "existing", icon: "📚", label: t("Use a signal you already have"),
      sub: t("Everything this box has stored, whether a node uses it or not."),
      meta: t("stored") },
    { value: "virtual", icon: "✨", label: t("Configure by hand"),
      sub: t("Type an EV1527 code you know, or roll a random one to pair your own chime to."),
      meta: t("by hand") }
  ]);
  pickerSheet(rx ? t("Which code should this node listen for?")
                 : t("Which code should this node send?"),
    rx ? t("A Signal receiver IS its signal: it fires whenever that code is heard on air, and " +
           "sends nothing. Pick where the code comes from.")
       : t("A Signal sender IS its signal: it puts that code on air whenever something " +
           "triggers it, and listens for nothing. Pick where the code comes from."),
    items, function (choice) {
      if (choice === "listen") listen();
      else if (choice === "virtual") virtual();
      else existing();
    });
}

function openNodeEditor(n) {
  var ty = nodeType(n.type);
  var sh = openSheet(n.name || ty.label, ty.help);
  var patch = {};   /* only what the user touched is sent */

  var nameIn = inputEl("text", n.name || "", { maxlength: "40" });
  add(sh.body, field(t("Name"), nameIn));

  /* On a Switch node this field IS the position, and it already has a control
     of its own further down that says so in the right words and moves it
     through the endpoint built for it. Offering "Enabled" as well would be two
     controls for one flag, and the wrong one would win on Save. */
  var enabled = isSwitch(n) ? null
    : checkField(t("Enabled"), n.enabled !== false,
                 t("A disabled node stays in the graph but never fires."));
  if (enabled) add(sh.body, enabled);

  var ctl = {};   /* type-specific controls */

  /* The signal lives HERE, in full, and ABOVE the node's own parameters: this
     is where the deleted Signals screen went, and "which signal" is the first
     question anyone has about a signal node. Decoded identity, waveform,
     pairing, transmit-to-test, rename, and the ways to point the node
     somewhere else. */
  if (isSignalNode(n)) {
    var sigSec = el("div", "sigsec");
    add(sh.body, el("div", "lg-label", t("Signal")));
    add(sh.body, el("div", "hint", isSignalRx(n)
      ? t("This node fires when this code is heard on air. It never transmits — to send a " +
          "code, add a Signal sender node and wire something into it.")
      : t("This node transmits this code whenever something linked into it fires. It never " +
          "listens — to react to a code, add a Signal receiver node for it.")));
    add(sh.body, sigSec);
    renderNodeSignal(sigSec, n);
  }

  var grid = el("div", "formgrid");
  add(sh.body, grid);

  /* Transmit policy, on the only node that transmits. It used to be shown on
     every signal node whether or not anything was wired into it, because there
     was no way to tell from the type which of them would ever send; a Signal
     receiver has no send side at all, so these would be two settings that
     provably do nothing. */
  if (isSignalTx(n)) {
    ctl.repeats = inputEl("number", numOr(n.repeats, 6), { min: "1", max: "32", step: "1", inputmode: "numeric" });
    ctl.gap = inputEl("number", numOr(n.gap_us, 8000), { min: "500", max: "60000", step: "500", inputmode: "numeric" });
    add(grid, field(t("Repeats when sending"), ctl.repeats,
      t("Many cheap receivers need several identical copies before they act.")));
    add(grid, field(t("Gap between copies (us)"), ctl.gap));
    if (!txAvailable()) add(sh.body, el("div", "note bad", txBlockText()));
  }

  if (n.type === "source.gpio") {
    var opts = [{ value: -1, label: t("— choose a pin —") }];
    var g = S.gpio;
    if (g) {
      (g.suggested || []).forEach(function (p) {
        opts.push({ value: p, label: t("GPIO {pin}  (recommended)", { pin: p }) });
      });
      (g.available || []).forEach(function (p) {
        if ((g.suggested || []).indexOf(p) >= 0) return;
        opts.push({ value: p, label: "GPIO " + p });
      });
      (g.in_use || []).forEach(function (p) {
        opts.push({ value: p, label: t("GPIO {pin}  (in use — unavailable)", { pin: p }), disabled: true });
      });
    } else if (n.gpio_pin >= 0) {
      opts.push({ value: n.gpio_pin, label: "GPIO " + n.gpio_pin });
    }
    ctl.pin = selectEl(opts, numOr(n.gpio_pin, -1));
    add(grid, field(t("Pin"), ctl.pin,
      g ? t("Recommended pins are listed first; pins already used by the radio are disabled.") :
          t("The pin list is unavailable, so only the current pin is offered."), "full"));
    ctl.activeLow = checkField(t("Button pulls the pin to ground (active low)"), n.gpio_active_low !== false);
    add(grid, ctl.activeLow);
    ctl.pin.parentNode.classList.add("full");
    ctl.activeLow.classList.add("full");
    ctl.debounce = inputEl("number", numOr(n.gpio_debounce_ms, 50), { min: "0", max: "1000", step: "5", inputmode: "numeric" });
    add(grid, field(t("Debounce (ms)"), ctl.debounce, t("Ignores contact bounce. 50 ms suits an ordinary push button.")));
    add(sh.body, el("div", "note",
      t("Wiring: one leg of the button to the chosen GPIO, the other leg to GND — the " +
        "internal pull-up does the rest, so no resistor and no extra supply is needed.")));
  }

  if (n.type === "source.virtual") {
    /* First, above the settings: the reason this node exists is to be fired by
       hand, so the button that does it comes before the topic it may never
       need. Same action and the same words as the card and the canvas ▶. */
    var fireMsg = el("div", "formmsg");
    var fireBtn = el("button", "btn primary block", t("▶ Trigger"));
    fireBtn.type = "button";
    fireBtn.addEventListener("click", function () { fireNode(n.id, fireBtn, fireMsg); });
    sh.body.insertBefore(fireBtn, grid);
    sh.body.insertBefore(fireMsg, grid);

    ctl.topic = inputEl("text", n.topic || "",
      { maxlength: String(TOPIC_MAX), placeholder: "front_gate" });
    var topicPreview = el("div", "hint mono");
    var topicErr = el("div", "hint");
    var tf = field(t("MQTT topic"), ctl.topic, null, "full");

    /* THE SAME SHAPE AS THE SWITCH EDITOR, because it is now the same rule:
       blank follows a slug of the node's NAME instead of meaning "no MQTT".
       Leaving it blank used to make the node invisible to the broker with
       nothing on screen saying so — a node that looked perfectly healthy and
       could not be pressed from anywhere but this page. */
    function syncTopic() {
      var typed = trimOf(ctl.topic);
      var auto = topicSlug(trimOf(nameIn));
      var topic = typed || auto;
      clear(topicPreview);
      ctl.topic.placeholder = auto || "front_gate";
      if (!ctl.mqttOn.input.checked) {
        add(topicPreview, el("div", null,
          t("Not exposed \u2014 nothing subscribes to this node and Home Assistant has no button " +
            "for it. The topic is kept for when you switch it back on.")));
        return;
      }
      if (!topic) {
        topicPreview.textContent =
          t("Give this node a name (or a topic) and Home Assistant gets a button for it.");
        return;
      }
      if (!typed)
        add(topicPreview, el("div", "muted",
          t("Following the node name. Type your own to pin it; clear it to follow again.")));
      add(topicPreview, el("div", null,
        t("Anything published here fires it:  {topic}", { topic: mqttTriggerTopic(topic) })));
      /* What Home Assistant will actually CALL it. The switch had exactly this
         defect — every box in the world showing one entity named "Klingelbox
         Switch" — so a node still wearing its palette label is named after the
         topic instead, and this is the only place that says so. */
      var nm = trimOf(nameIn);
      var generic = !nm || DEFAULT_TRIGGER_NAME.test(nm);
      add(topicPreview, el("div", null,
        t("Appears in Home Assistant as:  {name}", { name: generic ? prettyTopic(topic) : nm }) +
        (generic ? t("   (from the topic — rename this node to change it)") : "")));
    }
    /* The topic and the checkbox must never look as though they disagree: with
       the node not exposed the field is disabled and dimmed, so it plainly does
       not apply rather than sitting there implying it does. */
    function syncExposed() {
      var on = ctl.mqttOn.input.checked;
      ctl.topic.disabled = !on;
      if (on) tf.classList.remove("dim");
      else    tf.classList.add("dim");
      syncTopic();
    }
    ctl.mqttOn = exposeField(n, syncExposed);
    add(grid, ctl.mqttOn);
    ctl.topicCheck = bindTopicCheck(ctl.topic, topicErr, "topic", syncTopic);
    if (nameIn) nameIn.addEventListener("input", syncTopic);
    add(tf, topicErr, topicPreview);
    add(grid, tf);
    syncExposed();
    /* FOUR WAYS IN, SAID PLAINLY. The node was asked for as a missing feature
       by someone who had it on the palette already, because nothing here said
       "Home Assistant". So the list leads with Home Assistant and the ▶ is
       described last — it is the one nobody has to be told about. */
    add(sh.body, el("div", "note",
      t("Four things can press this button: a Home Assistant button entity (it appears there by " +
        "itself, no YAML), any MQTT client publishing ANY message to the topic above, the ▶ " +
        "above, and POST /api/graph/nodes/{id}/fire. The payload is never looked at — arriving " +
        "IS the press.", { id: n.id })));
    add(sh.body, el("div", "note",
      t("Leave the topic empty and it follows the node's name, so a button called “Ring the " +
        "chime” answers on ring_the_chime without you typing anything. Two nodes that end up " +
        "on the same topic share one Home Assistant button, and pressing it fires both.")));
    if (S.has.config && !mqttEnabled()) {
      add(sh.body, el("div", "note warn",
        t("MQTT is currently disabled, so the topic is stored but nothing subscribes to it yet. " +
          "Enable MQTT under Settings and it starts working — no change needed here.")));
    }
  }

  if (n.type === "source.any_rf") {
    /* Deliberately no parameters: the whole point is that it is a wildcard. */
    add(sh.body, el("div", "note",
      t("This node has nothing to configure. It fires on every burst the receiver hears, " +
        "including signals from buttons you never registered. Wire it to an MQTT publish sink " +
        "and Home Assistant sees every press on the band.")));
    add(sh.body, el("div", "note",
      t("It fires IN ADDITION to any matching Signal receiver — a registered press drives both its " +
        "own chain and this wildcard chain. That is intended, not double-firing.")));
    add(sh.body, el("div", "note warn",
      t("If the band around you is busy, put a Rate limit between this node and its sink. " +
        "A chatty neighbouring remote will otherwise spam your broker.")));
  }

  /* TWO MODES, TWO DIFFERENT NODES really, and the editor stops pretending
     otherwise. The window belongs to ALL alone -- in ANY the firmware returns
     "passes" before it ever looks at a clock -- so showing a Window field on an
     ANY group offered a setting that changes nothing, which is worse than
     offering none. It is hidden rather than disabled: the value is still there,
     still saved, and comes straight back if the mode is switched to ALL. */
  if (n.type === "logic.group") {
    ctl.mode = selectEl([
      { value: "any", label: t("ANY — merge: pass everything straight through") },
      { value: "all", label: t("ALL — coincidence: wait until every input has fired") }
    ], n.group_mode === "all" ? "all" : "any");
    ctl.windowS = inputEl("number", numOr(n.window_s, 1), { min: "1", max: "6000", step: "1", inputmode: "numeric" });
    ctl.windowDflt = 1;
    add(grid, field(t("Mode"), ctl.mode, null, "full"));
    /* The API's field is window_s, in SECONDS -- this said "ms" and passed the
       wrong variable, so the input never reached the DOM at all. */
    var winField = field(t("Window (seconds)"), ctl.windowS,
      t("How long an input is remembered while the group waits for the others."));
    add(grid, winField);
    var modeNote = el("div", "note");
    add(sh.body, modeNote);
    function syncGroupMode() {
      var all = ctl.mode.value === "all";
      winField.classList.toggle("hidden", !all);
      modeNote.textContent = all
        ? t("ALL is a coincidence detector. Nothing passes until EVERY link into this node has " +
            "carried an event inside the window; when that happens it fires once, forgets them " +
            "all and starts over. Use it for “both buttons within 10 seconds means something”. " +
            "A group with no inputs can never be satisfied.")
        : t("ANY is a merge point, not a filter. Whatever arrives is passed straight on, " +
            "immediately — the window is not used at all in this mode. Its value is that " +
            "several buttons can meet at one node, so the chain after it is wired and edited " +
            "in a single place.");
    }
    ctl.mode.addEventListener("change", syncGroupMode);
    syncGroupMode();
  }

  if (n.type === "logic.throttle") {
    ctl.windowS = inputEl("number", numOr(n.window_s, 10), { min: "1", max: "6000", step: "1", inputmode: "numeric" });
    add(grid, field(t("Cooldown (seconds)"), ctl.windowS,
      t("The first press passes straight through; anything within the cooldown after it " +
        "is dropped. Set 30 and the bell rings at most once every 30 seconds, no matter " +
        "how often the button is pressed.")));
    add(grid, noteRow(t("Works the same for every input — a 433 MHz remote, a wired button " +
      "or an MQTT trigger. It limits whatever is linked into it.")));
  }

  if (n.type === "logic.repeat") {
    ctl.times = inputEl("number", numOr(n.repeats, 3), { min: "1", max: "20", step: "1", inputmode: "numeric" });
    ctl.windowS = inputEl("number", numOr(n.window_s, 5), { min: "1", max: "6000", step: "1", inputmode: "numeric" });
    ctl.windowDflt = 5;
    add(grid, field(t("Times"), ctl.times,
      t("How many times in total, counting the first one. 3 rings the chime three times.")));
    add(grid, field(t("Interval (seconds)"), ctl.windowS,
      t("The gap between rings. 3 times at 5 seconds rings at 0 s, 5 s and 10 s.")));
    add(grid, noteRow(t("The first ring is immediate — nobody waits at the door for a chime. " +
      "Set Times to 1 and the node simply passes the press through unchanged.")));
    add(sh.body, el("div", "note",
      t("Pressing again while it is still running starts the count over rather than adding a " +
        "second run, so leaning on the button cannot queue up a dozen chimes.")));
  }

  if (n.type === "logic.switch") {
    /* The position leads, above everything else and above the grid: opening a
       switch is nearly always "is it on, and make it the other thing". */
    var swMsg = el("div", "formmsg");
    var swBtn = el("button", "btn block " + (switchOn(n) ? "danger" : "primary"),
                   switchOn(n) ? t("Switch OFF — block this path")
                               : t("Switch ON — let this path conduct"));
    swBtn.type = "button";
    var swState = el("div", "note " + (switchOn(n) ? "" : "warn"),
      switchOn(n)
        ? t("Currently ON. Events reaching this node pass straight through, unchanged.")
        : t("Currently OFF. Nothing gets past this node — everything wired after it is dead " +
            "until it is switched back on. The nodes and links are all still there."));
    swBtn.addEventListener("click", function () {
      setSwitch(n, !switchOn(n), swBtn, swMsg).then(function (ok) {
        if (ok) sh.close();
      });
    });
    sh.body.insertBefore(swState, grid);
    sh.body.insertBefore(swBtn, grid);
    sh.body.insertBefore(swMsg, grid);

    /* The RF control input, directly under the position and above the MQTT
       block: the two questions about a switch are "which way is it" and "what
       moves it", in that order, and Home Assistant is only one of the answers
       to the second. Applied the moment a signal is chosen -- like the binding
       on a Signal node, and for the same reason: it is an action, not a form
       field the user has to remember to Save. */
    var swSig = el("div", "sigctl");
    sh.body.insertBefore(swSig, grid);
    renderSwitchControl(swSig, n);

    ctl.topic = inputEl("text", n.topic || "",
      { maxlength: String(TOPIC_MAX), placeholder: "outside_bell" });
    var swPreview = el("div", "hint mono");
    var swErr = el("div", "hint");
    var sf = field(t("MQTT topic"), ctl.topic, null, "full");

    function syncSw() {
      var typed = trimOf(ctl.topic);
      var auto = topicSlug(trimOf(nameIn));
      var topic = typed || auto;
      clear(swPreview);
      ctl.topic.placeholder = auto || "outside_bell";
      /* Said first and on its own, because with the node not exposed every other
         line below would be describing a topic that is not live. */
      if (!ctl.mqttOn.input.checked) {
        add(swPreview, el("div", null,
          t("Not exposed \u2014 no subscription, no Home Assistant entity, and the retained " +
            "state cleared. The switch still works from this page and the REST API.")));
        return;
      }
      if (!topic) {
        swPreview.textContent =
          t("Give this node a name (or a topic) and Home Assistant can switch it.");
        return;
      }
      if (!typed)
        add(swPreview, el("div", "muted",
          t("Following the node name. Type your own to pin it; clear it to follow again.")));
      add(swPreview, el("div", null, t("Home Assistant sets:  {topic}", { topic: mqttSwitchTopic(topic, "set") })));
      add(swPreview, el("div", null, t("Box reports (retained):  {topic}", { topic: mqttSwitchTopic(topic, "state") })));
      /* What HA will actually CALL it. A node still named "Switch" would appear
         as "Klingelbox Switch" on every box ever built, so the firmware falls
         back to the topic -- but that is invisible unless it is said here, and
         a user who wants a nicer label needs to know renaming the node is what
         changes it. */
      var nm = trimOf(nameIn);
      var generic = !nm || /^(switch|logic\.switch)$/i.test(nm);
      add(swPreview, el("div", null,
        t("Appears in Home Assistant as:  {name}", { name: generic ? prettyTopic(topic) : nm }) +
        (generic ? t("   (from the topic \u2014 rename this node to change it)") : "")));
    }
    function syncSwExposed() {
      var on = ctl.mqttOn.input.checked;
      ctl.topic.disabled = !on;
      if (on) sf.classList.remove("dim");
      else    sf.classList.add("dim");
      syncSw();
    }
    ctl.mqttOn = exposeField(n, syncSwExposed);
    add(grid, ctl.mqttOn);
    ctl.topicCheck = bindTopicCheck(ctl.topic, swErr, "topic", syncSw);
    if (nameIn) nameIn.addEventListener("input", syncSw);
    add(sf, swErr, swPreview);
    add(grid, sf);
    syncSwExposed();
    add(sh.body, el("div", "note",
      t("With a topic set, Home Assistant discovers this as a real switch entity on the " +
        "Klingelbox device — a toggle, not a workaround. ON, OFF, 1, 0, true and false are all " +
        "accepted on the set topic; the box answers on the state topic, retained, so the toggle " +
        "is never stale after a restart.")));
    add(sh.body, el("div", "note",
      t("Several Switch nodes may share ONE topic on purpose: give “Outside bell” to two switches " +
        "and a single Home Assistant toggle gates both paths at once. The reported state is ON if " +
        "any of them is conducting.")));
    if (S.has.config && !mqttEnabled()) {
      add(sh.body, el("div", "note warn",
        t("MQTT is currently disabled, so the topic is stored but nothing subscribes to it yet. " +
          "The switch still works from this page. Enable MQTT under Settings and Home Assistant " +
          "picks it up — no change needed here.")));
    }
  }

  if (n.type === "sink.mqtt") {
    ctl.topic = inputEl("text", n.topic || "",
      { maxlength: String(TOPIC_MAX), placeholder: "front" });
    var pubPreview = el("div", "hint mono");
    var pubErr = el("div", "hint");
    var pf = field(t("Topic"), ctl.topic, null, "full");

    function syncPub() {
      if (!ctl.mqttOn.input.checked) {
        pubPreview.textContent =
          t("Not exposed \u2014 this node publishes nothing at all, not even to the " +
            "{base}/event stream. The chain still reaches it.", { base: mqttBase() });
        return;
      }
      var topic = trimOf(ctl.topic);
      pubPreview.textContent = topic ? t("Publishes to:  {topic}", { topic: mqttPublishTopic(topic) }) : t("No topic set — nothing is published.");
    }
    function syncPubExposed() {
      var on = ctl.mqttOn.input.checked;
      ctl.topic.disabled = !on;
      if (on) pf.classList.remove("dim");
      else    pf.classList.add("dim");
      syncPub();
    }
    ctl.mqttOn = exposeField(n, syncPubExposed);
    add(grid, ctl.mqttOn);
    ctl.topicCheck = bindTopicCheck(ctl.topic, pubErr, "topic", syncPub);
    add(pf, pubErr, pubPreview);
    add(grid, pf);
    syncPubExposed();
    if (S.has.config && !mqttEnabled()) {
      add(sh.body, el("div", "note warn",
        t("MQTT is disabled under Settings, so this node stores its topic but publishes nothing yet.")));
    }
  }

  if (n.type === "sink.monitor") {
    /* The timeline is the node, so it leads — before the one setting it has. */
    var monHead = el("div", "montl-head");
    add(monHead, el("span", "lg-label", t("Last 10 minutes")), monitorLamp(n.id, "lamp-lg"));
    sh.body.insertBefore(monHead, grid);
    sh.body.insertBefore(monitorTimeline(n.id), grid);

    ctl.windowS = inputEl("number", numOr(n.window_s, 3),
      { min: "1", max: "60", step: "1", inputmode: "numeric" });
    ctl.windowDflt = 3;
    add(grid, field(t("Lamp stays lit (seconds)"), ctl.windowS,
      t("How long the indicator glows after each hit, and how wide each mark is drawn " +
        "on the timeline. 1–60 s.")));
    add(sh.body, el("div", "note",
      t("This node acts on nothing. It transmits no signal, publishes no message and touches " +
        "no pin — it only records that the chain reached it, in RAM, for the last 10 minutes. " +
        "Nothing is written to flash and nothing survives a reboot.")));
    add(sh.body, el("div", "note",
      t("Wire one alongside a real sink to prove a chain fires without ringing anything: " +
        "link the same node into both the Signal sender you would use and this Monitor, then " +
        "trigger the chain and watch the lamp instead of listening for the chime.")));
  }

  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");
  /* Delete belongs here too, not only on the list card: opening a node from the
     map was a one-way trip with Save as the only exit. */
  var delBtn = el("button", "btn danger", t("Delete node"));
  delBtn.type = "button";
  delBtn.addEventListener("click", function () {
    deleteNodeConfirmed(n,
      function (e) { setMsg(msg, e.message, "err"); },
      function () { sh.close(); });
  });

  var save = el("button", "btn primary", t("Save"));
  save.type = "button";
  save.addEventListener("click", function () {
    /* signal_id is deliberately absent: binding a signal is an action of its
       own, applied the moment it is chosen, not a form field to remember. */
    patch = { name: trimOf(nameIn) || nodeType(n.type).label };
    /* Absent for a Switch: its position is not a form field to remember, and
       posting it here would persist synchronously behind the user's back. */
    if (enabled) patch.enabled = enabled.input.checked;
    if (ctl.repeats) patch.repeats = intOf(ctl.repeats, 6);
    /* Same wire field as a Signal sender's copy count, different meaning and a
       different default — hence a control of its own rather than reusing
       ctl.repeats and its default of 6. */
    if (ctl.times) patch.repeats = intOf(ctl.times, 3);
    if (ctl.gap) patch.gap_us = intOf(ctl.gap, 8000);
    if (ctl.pin) patch.gpio_pin = intOf(ctl.pin, -1);
    if (ctl.activeLow) patch.gpio_active_low = ctl.activeLow.input.checked;
    if (ctl.debounce) patch.gpio_debounce_ms = intOf(ctl.debounce, 50);
    if (ctl.windowS) patch.window_s = intOf(ctl.windowS, numOr(ctl.windowDflt, 10));
    if (ctl.mode) patch.group_mode = ctl.mode.value;
    if (ctl.topic) patch.topic = trimOf(ctl.topic);
    if (ctl.mqttOn) patch.mqtt_enabled = ctl.mqttOn.input.checked;
    /* Re-checked here as well as on every keystroke: a value can arrive by paste
       or autofill without an input event, and the firmware would refuse it
       anyway — better to say so in the same words, without a round trip. The
       topic is validated even when the node is not exposed, because it is still
       stored and would still be refused on the next save. */
    if (ctl.topicCheck && !ctl.topicCheck()) {
      setMsg(msg, topicError(patch.topic, "topic", TOPIC_MAX), "err");
      return;
    }
    save.disabled = true;
    setMsg(msg, t("Saving…"));
    postJSON("/api/graph/nodes/" + n.id, patch).then(function () {
      sh.close();
      loadGraph();
    }).catch(function (e) { save.disabled = false; setMsg(msg, e.message, "err"); });
  });
  var cancel = el("button", "btn", t("Cancel"));
  cancel.type = "button";
  cancel.addEventListener("click", sh.close);
  add(foot, save, cancel, delBtn, msg);
  add(sh.body, foot);
}

/* ------------------------------------------------- a switch's RF control --

   "React to a signal" on a Switch node, rebuilt in place when it changes.

   THIS IS NOT A WIRE, AND THAT IS THE WHOLE REASON IT IS A FIELD. A Switch's
   input PORT is a data input: events arriving there travel THROUGH it while it
   is on. What this offers is a CONTROL input -- a code that moves the switch
   rather than passing through it -- and the map has no such thing to draw. So
   it lives on the node, in the editor, and reuses the same signal picker every
   other binding in this UI uses rather than being a second chooser to learn.

   None is a real option and is the default, so it is spelled out as a button
   rather than left as "clear the field somehow". */
function renderSwitchControl(wrap, n) {
  clear(wrap);
  var sid = numOr(n.signal_id, 0);
  var msg = el("div", "formmsg");

  add(wrap, el("div", "lg-label", t("React to a signal")));
  add(wrap, el("div", "hint",
    t("Optional. Each press of that button flips this switch — on, off, on again. " +
      "Nothing is transmitted and nothing travels down the wire; only the position changes.")));

  if (!sid) {
    add(wrap, el("div", "note",
      t("None. This switch is moved from this page, the REST API and Home Assistant only — " +
        "nothing on air touches it.")));
  } else {
    var nm = signalName(sid);
    add(wrap, el("div", nm ? "note ok" : "note bad", nm
      ? t("Every press of “{name}” toggles this switch.", { name: nm })
      : t("Set to signal {id}, which is no longer in the store — nothing will ever toggle it. " +
          "Pick another one, or set it back to None.", { id: sid })));
  }

  function apply(id, busy) {
    setMsg(msg, busy);
    return postJSON("/api/graph/nodes/" + n.id, { signal_id: id }).then(function () {
      n.signal_id = id;
      loadGraph();
      renderSwitchControl(wrap, n);
    }).catch(function (e) { setMsg(msg, e.message, "err"); });
  }

  var row = el("div", "btnrow");
  row.style.marginTop = ".6rem";
  var pick = el("button", "btn",
    sid ? t("🔀 Use a different signal") : t("📻 Choose a signal"));
  pick.type = "button";
  pick.addEventListener("click", function () {
    openSignalPicker({
      title: t("Signal that toggles this switch"),
      node: n,
      onPick: function (sig) { if (sig) apply(sig.id, t("Linking…")); }
    });
  });
  add(row, pick);

  if (sid) {
    var none = el("button", "btn", t("✕ None"));
    none.type = "button";
    none.addEventListener("click", function () { apply(0, t("Clearing…")); });
    add(row, none);
  }
  add(wrap, row, msg);
}

/* The signal section of a node editor, rebuilt in place whenever the binding
   changes. Async because the waveform only comes with GET /api/signals/{id}. */
function renderNodeSignal(wrap, n) {
  clear(wrap);
  var sid = numOr(n.signal_id, 0);

  if (!sid) {
    add(wrap, el("div", "note warn", isSignalTx(n)
      ? t("This node has no signal yet, so there is nothing for it to send. " +
          "Listen for the button it should stand for, pick one you already have, or invent a " +
          "code your own receiver can be paired to.")
      : t("This node has no signal yet, so nothing on air can ever fire it. " +
          "Listen for the button it should stand for, pick one you already have, or invent a " +
          "code your own receiver can be paired to.")));
    add(wrap, signalChooser(wrap, n));
    return;
  }

  add(wrap, el("div", "empty", t("Loading signal…")));
  api("/api/signals/" + sid).then(function (sig) {
    clear(wrap);
    add(wrap, signalBlock(sig, { node: n }));
    add(wrap, signalChooser(wrap, n));
  }).catch(function (e) {
    clear(wrap);
    add(wrap, el("div", "note bad",
      t("This node points at signal {id}, which the box cannot produce: {err} Pick another one below.",
        { id: sid, err: e.message })));
    add(wrap, signalChooser(wrap, n));
  });
}

/* The three ways out of the current binding. Choosing one applies immediately
   -- and never deletes the signal that was there before. */
function signalChooser(wrap, n) {
  var bound = numOr(n.signal_id, 0) > 0;
  var box = el("div");
  var row = el("div", "btnrow");
  row.style.marginTop = ".6rem";
  var msg = el("div", "formmsg");

  function bind(sig) {
    if (!sig) return;
    setMsg(msg, t("Linking…"));
    postJSON("/api/graph/nodes/" + n.id, { signal_id: sig.id }).then(function () {
      n.signal_id = sig.id;
      loadGraph();
      renderNodeSignal(wrap, n);
    }).catch(function (e) { setMsg(msg, e.message, "err"); });
  }

  var pick = el("button", "btn", bound ? t("🔀 Use a different signal") : t("📚 Choose a stored signal"));
  pick.type = "button";
  pick.addEventListener("click", function () {
    openSignalPicker({
      title: t("Signal for this node"),
      node: n,
      onPick: bind,
      onListen: S.has.raw ? function () { relisten(); } : null,
      onVirtual: function () { makeVirtual(); }
    });
  });

  function relisten() {
    openListenFlow({
      title: bound ? t("Listen for a replacement") : t("Listen for a button"),
      sub: bound
        ? t("Press the button this node should stand for from now on, several times."
            + " The signal it uses today stays in the store under its name — this only "
            + "changes what the node points at.")
        : t("Press the button this node should stand for from now on, several times.")
    }).then(bind);
  }
  add(row, pick);
  if (S.has.raw) {
    var listen = el("button", "btn", bound ? t("🎧 Listen again") : t("🎧 Listen for a button"));
    listen.type = "button";
    listen.addEventListener("click", relisten);
    add(row, listen);
  }

  /* The third path: a code entered by hand rather than learned or picked.
     openVirtualFlow does the explaining about what a made-up code means. */
  function makeVirtual() { openVirtualFlow({ mode: "signal" }).then(bind); }
  var virt = el("button", "btn", t("✨ Configure by hand"));
  virt.type = "button";
  virt.addEventListener("click", makeVirtual);
  add(row, virt);

  add(box, row, msg);
  return box;
}

/* ---------------------------------------------------------------- canvas --
   Second view, >= 900 px only. Nodes sit at ui_x/ui_y and can be dragged to
   rearrange; links are cubic beziers. Tapping a node opens the same editor the
   list uses, so the two views never diverge in capability. */
function renderCanvas() {
  if (!autoEls.canvasWrap) return;
  var wrap = clear(autoEls.canvasWrap);
  var nodes = (S.graph && S.graph.nodes) || [];
  var links = (S.graph && S.graph.links) || [];
  if (!nodes.length) {
    if (autoEls.zoomSlot) clear(autoEls.zoomSlot);   /* nothing to zoom */
    add(wrap, el("div", "empty", t("No nodes to draw.")));
    return;
  }

  /* The paragraph that used to explain every badge on a node is in the
     Handbook now, under "How it works" -> Reading the map. It was a nine-line
     legend permanently occupying the top of the map it described. */
  /* The canvas has no per-node message line, so one shared one under the tools
     carries the confirmations the ▶ button owes. */
  var canvasMsg = el("div", "formmsg");

  /* 168 x 60. The extra height over the original 52 is the badge strip along
     the top: without it the badges would sit on top of the title and force it
     to truncate at about thirteen characters, which costs more than four units
     of box height ever could. Title and type simply moved down; the ports
     follow NH/2 as they always did, and every position below is derived. */
  var NW = 168, NH = 60;
  var maxX = 0, maxY = 0;
  nodes.forEach(function (n) {
    maxX = Math.max(maxX, numOr(n.ui_x, 40) + NW);
    maxY = Math.max(maxY, numOr(n.ui_y, 40) + NH);
  });
  /* Generous slack past the furthest node, so there is ALWAYS empty canvas to
     drag something into. Sizing the viewBox tightly around the existing nodes
     left nowhere to drop a new one. */
  var VW = Math.max(900, maxX + 280), VH = Math.max(420, maxY + 220);

  /* Zoom lives outside renderCanvas so it survives a re-render (every link edit
     redraws the whole canvas). Kept in the same 0.4..2 range the buttons offer. */
  var zoom = numOr(S.graphZoom, 1);

  /* Into the Dashboard's one toolbar row, beside the view switch — not into a
     second bar of its own above the canvas. */
  var zbar = clear(autoEls.zoomSlot);
  var zOut = el("button", "btn small", "\u2212"); zOut.type = "button";
  zOut.setAttribute("aria-label", t("Zoom out"));
  var zLbl = el("span", "hint mono zoom-lbl");
  var zIn = el("button", "btn small", "+"); zIn.type = "button";
  zIn.setAttribute("aria-label", t("Zoom in"));
  var zFit = el("button", "btn small", t("Fit")); zFit.type = "button";
  add(zbar, zOut, zLbl, zIn, zFit);
  add(wrap, canvasMsg);

  var box = el("div", "canvas-wrap");
  var svg = svgEl("svg", "canvas");
  /* 1:1 and scrolling, NOT scale-to-fit.
     With `xMidYMid meet` the whole graph was squeezed to fit a fixed-height box:
     nodes shrank as the graph grew, and — because the viewBox and the element
     rarely share an aspect ratio — `meet` letterboxed, leaving bands of the
     canvas where a pointer lands outside the viewBox and a node simply cannot be
     placed. Rendering at true size inside a scrolling wrapper removes both: the
     drag distance always equals the pointer distance, and every pixel of the
     canvas is real canvas. */
  svg.setAttribute("viewBox", "0 0 " + VW + " " + VH);

  /* Zoom scales the RENDERED size while the viewBox stays in graph units, so all
     the geometry below keeps working untouched — and svgPoint(), which derives
     its ratio from viewBox-vs-rect, stays correct at any zoom without knowing
     zoom exists. */
  function applyZoom() {
    zoom = Math.min(2, Math.max(0.4, zoom));
    S.graphZoom = zoom;
    svg.style.width = Math.round(VW * zoom) + "px";
    svg.style.height = Math.round(VH * zoom) + "px";
    zLbl.textContent = Math.round(zoom * 100) + "%";
  }
  zOut.addEventListener("click", function () { zoom -= 0.2; applyZoom(); });
  zIn.addEventListener("click", function () { zoom += 0.2; applyZoom(); });
  zFit.addEventListener("click", function () {
    var avail = box.clientWidth || VW;
    zoom = avail / VW;
    applyZoom();
  });
  applyZoom();
  var gLinks = svgEl("g");
  var gNodes = svgEl("g");
  var gTemp  = svgEl("g");   /* the in-progress link, drawn above everything */
  add(svg, gLinks);
  add(svg, gNodes);
  add(svg, gTemp);

  function pos(n) { return { x: numOr(n.ui_x, 40), y: numOr(n.ui_y, 40) }; }

  /* A node's ports follow directly from its group: a source has nothing feeding
   * it, a sink emits nothing onward, and logic has both. Drawing them makes the
   * direction of flow legible at a glance, and gives the pointer something
   * concrete to drag from — without connectors the canvas is only a picture,
   * and every link has to be made over in the list view.
   *
   * A Signal receiver draws one dot on its right and a Signal sender one on its
   * left, which is the entire argument for splitting the old two-ported signal
   * node in two: the picture now cannot show a connection the engine will not
   * make, and a relay reads left to right as rx → … → tx. */
  function hasIn(ty)  { return ty.g !== "source"; }
  function hasOut(ty) { return ty.g !== "sink"; }
  function outXY(n) { var p = pos(n); return { x: p.x + NW, y: p.y + NH / 2 }; }
  function inXY(n)  { var p = pos(n); return { x: p.x,      y: p.y + NH / 2 }; }

  function curve(x1, y1, x2, y2) {
    var dx = Math.max(30, Math.abs(x2 - x1) / 2);
    return "M" + x1 + "," + y1 + " C" + (x1 + dx) + "," + y1 +
           " " + (x2 - dx) + "," + y2 + " " + x2 + "," + y2;
  }

  function drawLinks() {
    clear(gLinks);
    links.forEach(function (l) {
      var a = nodeById(l.from), b = nodeById(l.to);
      if (!a || !b) return;
      var p1 = outXY(a), p2 = inXY(b);
      /* A fat transparent path under the visible one: a 1.6 px stroke is far too
       * thin to hit with a finger or a quick click. */
      var hit = svgEl("path", "lnk-hit");
      hit.setAttribute("d", curve(p1.x, p1.y, p2.x, p2.y));
      /* THE POINT OF LOOKING AT THE MAP. A path that cannot carry an event must
       * not look like one that can — otherwise the first thing a user does after
       * turning a Switch off is stare at an unchanged picture and wonder whether
       * it worked. A blocked wire is drawn broken and faded, which is what a
       * broken wire looks like in every diagram anyone has read. */
      var path = svgEl("path", "lnk" + (linkBlocked(a, b) ? " blocked" : ""));
      path.setAttribute("d", curve(p1.x, p1.y, p2.x, p2.y));
      function removeLink(ev) {
        ev.stopPropagation();
        confirmSheet(t("Remove this link?"),
          [nodeName(l.from) + "  \u2192  " + nodeName(l.to),
           t("Both nodes stay; only the connection between them goes away.")],
          t("Remove link"), true).then(function (ok) {
          if (!ok) return;
          delJSON("/api/graph/links", { from: l.from, to: l.to }).then(loadGraph)
            .catch(function (e) { alertSheet(t("Could not remove the link"), e.message); });
        });
      }
      hit.addEventListener("click", removeLink);
      path.addEventListener("click", removeLink);
      hit.setAttribute("tabindex", "0");
      add(gLinks, hit);
      add(gLinks, path);
    });
  }

  /* ---- drag a link from an output port to a target node ----
   * The drop target is the whole node rather than its input port: a 6 px circle
   * is an unfair target on a trackpad and impossible on a touchscreen, and there
   * is no ambiguity about what dropping on a node means. */
  var linking = null;

  function cancelLink() {
    linking = null;
    clear(gTemp);
    svg.classList.remove("linking");
  }

  function nodeAt(pt) {
    for (var i = nodes.length - 1; i >= 0; i--) {
      var q = pos(nodes[i]);
      if (pt.x >= q.x && pt.x <= q.x + NW && pt.y >= q.y && pt.y <= q.y + NH)
        return nodes[i];
    }
    return null;
  }

  svg.addEventListener("pointermove", function (ev) {
    if (!linking) return;
    var pt = svgPoint(svg, ev);
    clear(gTemp);
    var from = nodeById(linking.from);
    if (!from) return;
    var a = outXY(from);
    var tmp = svgEl("path", "lnk tmp");
    tmp.setAttribute("d", curve(a.x, a.y, pt.x, pt.y));
    add(gTemp, tmp);
  });

  svg.addEventListener("pointerup", function (ev) {
    if (!linking) return;
    var pt = svgPoint(svg, ev);
    var target = nodeAt(pt);
    var from = linking.from;
    cancelLink();
    if (!target) return;
    /* Only a logic node has both ports, so only a logic node can have its own
       output dragged onto its own input. The firmware refuses a self-link
       outright (a cycle of one), so say why rather than letting the drag die
       silently. */
    if (target.id === from) {
      if (hasIn(nodeType(target.type)) && hasOut(nodeType(target.type)))
        alertSheet(t("A node cannot feed itself"),
          t("{name} would be its own input, which is a loop of one and would " +
            "never settle. Link it to another node instead.", { name: nodeName(from) }));
      return;
    }
    if (!hasIn(nodeType(target.type))) {
      alertSheet(t("That node has no input"),
                 t("{name} is a source, so nothing can feed into it.", { name: nodeName(target.id) }));
      return;
    }
    var dup = links.some(function (l) { return l.from === from && l.to === target.id; });
    if (dup) return;
    postJSON("/api/graph/links", { from: from, to: target.id }).then(loadGraph)
      .catch(function (e) { alertSheet(t("Could not create the link"), e.message); });
  });
  svg.addEventListener("pointerleave", cancelLink);

  /* Drag empty canvas to pan. Only when the press lands on the background — a
     press on a node still moves that node, and one on a port still starts a
     link. Matters once you are zoomed in and the graph is bigger than the box. */
  var pan = null;
  svg.addEventListener("pointerdown", function (ev) {
    if (linking || ev.target !== svg) return;
    pan = { x: ev.clientX, y: ev.clientY, l: box.scrollLeft, t: box.scrollTop };
    svg.classList.add("panning");
  });
  svg.addEventListener("pointermove", function (ev) {
    if (!pan) return;
    box.scrollLeft = pan.l - (ev.clientX - pan.x);
    box.scrollTop  = pan.t - (ev.clientY - pan.y);
  });
  function endPan() { pan = null; svg.classList.remove("panning"); }
  svg.addEventListener("pointerup", endPan);
  svg.addEventListener("pointercancel", endPan);

  /* ---- the small controls a node box may carry ----
   *
   * THE LAYOUT IS ONE DESIGN, not three additions. The fixed furniture is the
   * ports: they sit on the vertical centre line at (0, NH/2) and (NW, NH/2),
   * and the output one hides a 13-unit .port-grab disc. So the controls get a
   * strip of their own along the INSIDE of the top edge, right-aligned, and the
   * box grew from 52 to 60 units tall to give them that strip rather than
   * making them sit on the title.
   *
   *   ▶ / 💡  act or lamp   (NW - 56, 14) = (112, 14)   drawn r 9, hit r 13
   *   ✕        delete       (NW - 26, 14) = (142, 14)   drawn r 9, hit r 13
   *
   * ✕ is always the rightmost badge, so its position never moves with the type.
   * The slot to its left holds whatever that type has: ▶ on a signal or a
   * virtual trigger, 💡 on a monitor, I/O on a switch, nothing otherwise. No
   * type has two, so two slots is the whole vocabulary — a new badge takes the
   * act slot or it does not exist. Verifiable separations:
   *
   *   ✕ to the output grab (NW, NH/2):  sqrt(26² + 16²) = 30.5  vs 13 + 13 = 26
   *   ✕ to the act badge:               30                      vs 13 + 13 = 26
   *   act badge to the input port:      sqrt(112² + 16²) = 113.1  (no grab disc)
   *   ✕ to the input port:              sqrt(142² + 16²) = 142.9
   *
   * The natural inset corner (NW-16, 14) = (152, 14) is only sqrt(16² + 16²) =
   * 22.6 from the output port — a 13-unit hit disc there already overlaps the
   * 13-unit grab — which is why ✕ is inset a further ten units instead. And the
   * badges clear the text vertically (they end at y 27, the title's cap height
   * starts at 31), so a title still gets its full 22 characters on every type
   * and a node with no badges reads exactly as one with them.
   *
   * Three types get a ▶, and it always means "do this node's own thing, now":
   *
   *   signal.tx       transmit that code over the air
   *   signal.rx       pretend that code was just heard — fires its output only
   *   source.virtual  fire its output
   *
   * THOSE FIRST TWO ARE OPPOSITE DIRECTIONS, which is the one thing this glyph
   * must not hide. It is still the right glyph — each node has exactly one own
   * thing to do, and ▶ is "do it" — but the two cannot be told apart by looking,
   * so playAction() gives every ▶ a tooltip that names the direction in words,
   * and the ✕-style confirmation-free click is backed by a message on the line
   * under the tools saying what just happened. The list cards spend the clearer
   * 📡 / 📥 glyphs instead, where there is room for them.
   *
   * source.gpio and source.any_rf get none: they are driven by the physical
   * world, and a fake press would be a lie about what happened. logic.* and
   * sink.mqtt get none either, and that is a judgement rather than an
   * oversight — firing a logic node starts a traversal AT it, and traverse()
   * never gates its own start, so ▶ on a Rate limit would push an event
   * through the very node whose job is to block one; and ▶ on an MQTT sink
   * would publish a real message to the user's broker with no trigger behind
   * it. Neither is that node doing its own thing. Both keep Test fire on their
   * list card, where the wording can say what it really is.
   *
   * Every interactive one stops the pointerdown that would otherwise start a
   * node drag, and draws a bigger invisible disc than its glyph, exactly as
   * .port-grab does. The action runs on pointerup, so a press that slides off
   * the target does nothing. */
  var BADGE_Y = 14,      /* the badge strip, inside the top edge            */
      BADGE_R = 9,       /* drawn radius                                    */
      BADGE_HIT = 13,    /* invisible hit radius, comfortably larger        */
      BADGE_X_DEL = NW - 26,   /* 142 — ✕ is always the rightmost badge     */
      BADGE_X_ACT = NW - 56;   /* 112 — the type's own badge, if it has one */

  /* A badge is a filled disc with a border and a glyph centred on it, so it
     reads as a button rather than a loose mark on the box. Every control shares
     the shape — ✕, ▶ and 💡 alike — because three different visual languages on
     one 168-unit box is what makes a node look like a toolbar. */
  function badge(g, cx, cy, cls, glyph) {
    var c = svgEl("circle", "nbadge " + cls);
    c.setAttribute("cx", cx); c.setAttribute("cy", cy); c.setAttribute("r", BADGE_R);
    add(g, c);
    if (glyph) {
      var txt = svgEl("text", "nbadge-gl " + cls);
      txt.setAttribute("x", cx); txt.setAttribute("y", cy + 3.5);
      txt.setAttribute("text-anchor", "middle");
      txt.textContent = glyph;
      add(g, txt);
    }
    return c;
  }

  /* A widened badge carrying a short VALUE rather than a glyph.
     Rate limit and Repeat are configured entirely by two numbers, and on the map
     those numbers are the only thing distinguishing one from another — a row of
     identical "Rate limit" boxes tells you nothing about which is 10 s and which
     is 10 minutes. It shares the badge shape, sits in the same slot the lamp and
     ▶ use, and is right-aligned to that slot's edge so it grows leftwards into
     empty space instead of towards the ✕. It is a readout: no hit disc. */
  function pill(g, xEnd, cy, cls, text) {
    var w = Math.max(2 * BADGE_R, text.length * 6 + 10);
    var x = xEnd - w;
    var r = svgEl("rect", "nbadge npill " + cls);
    r.setAttribute("x", x); r.setAttribute("y", cy - BADGE_R);
    r.setAttribute("width", w); r.setAttribute("height", BADGE_R * 2);
    r.setAttribute("rx", BADGE_R);
    add(g, r);
    var txt = svgEl("text", "nbadge-gl npill " + cls);
    txt.setAttribute("x", x + w / 2); txt.setAttribute("y", cy + 3.5);
    txt.setAttribute("text-anchor", "middle");
    txt.textContent = text;
    add(g, txt);
    return r;
  }

  /* Seconds, compact enough for a 168-unit box: 90 -> "90s", 600 -> "10m". */
  function shortSecs(v) {
    var n = Math.max(0, Math.round(numOr(v, 0)));
    if (n < 60 || n % 60) return n + "s";
    var m = n / 60;
    return (m < 60 || m % 60) ? m + "m" : (m / 60) + "h";
  }

  function hitDisc(g, cx, cy, r, label, onFire) {
    var d = svgEl("circle", "nhit");
    d.setAttribute("cx", cx); d.setAttribute("cy", cy); d.setAttribute("r", r);
    var tip = svgEl("title");
    tip.textContent = label;
    add(d, tip);
    d.addEventListener("pointerdown", function (ev) {
      if (linking) return;    /* a link drop lands on the node, not on this */
      ev.stopPropagation();   /* must not also start dragging the node */
      ev.preventDefault();
    });
    d.addEventListener("pointerup", function (ev) {
      /* Releasing a link drag over one of these must complete the LINK. Let it
         bubble to the canvas handler untouched rather than acting on the node
         the user was aiming a wire at. */
      if (linking) return;
      ev.stopPropagation();
      onFire();
    });
    add(g, d);
    return d;
  }

  /* What ▶ does on this node, or null when it has none. Kept in one place so
     the glyph, the tooltip, the disabled reason and the action can never say
     three different things about the same button. */
  function playAction(n) {
    if (n.type === "source.virtual") {
      return {
        tip: t("Trigger “{name}” now (fires its output; nothing is sent on air)",
               { name: nodeName(n.id) }),
        why: null,
        run: function (flash) { flash(); fireNode(n.id, null, canvasMsg); }
      };
    }
    /* The receiver's ▶ is the safe one, and its tooltip has to say so as
       plainly as the sender's says the opposite — the two wear the same glyph
       on the same canvas. Never disabled: a node with no code bound still has
       an output, and firing it is exactly how you test the chain hanging off
       it before the code exists. */
    if (n.type === "signal.rx") {
      var rsid = numOr(n.signal_id, 0);
      var what = rsid ? t("“{name}”", { name: signalName(rsid) || t("signal {id}", { id: rsid }) })
                      : t("this node's code");
      return {
        tip: t("Simulate hearing {what} — fires this node's output and runs the chain " +
               "after it. NOTHING is transmitted; no chime rings from this.", { what: what }),
        why: null,
        run: function (flash) {
          flash();
          setMsg(canvasMsg, t("Simulating {what} — nothing was sent on air.", { what: what }));
          fireNode(n.id, null, canvasMsg);
        }
      };
    }
    if (n.type === "signal.tx") {
      var sid = numOr(n.signal_id, 0);
      var why = !sid ? t("This sender has no code bound to it yet, so there is nothing to send.")
                     : (!txAvailable() ? txBlockText() : null);
      return {
        /* Says OVER THE AIR in as many words. One click here can ring a chime
           in someone's house, so the tooltip must not be coy about it. */
        tip: why || t("Transmit “{name}” OVER THE AIR now — this rings anything paired to it",
                      { name: signalName(sid) || t("signal {id}", { id: sid }) }),
        why: why,
        run: function (flash) {
          flash();
          setMsg(canvasMsg, t("Sending “{name}”…",
                              { name: signalName(sid) || t("signal {id}", { id: sid }) }));
          transmit(sid, null, canvasMsg, {
            body: { repeats: numOr(n.repeats, 6), gap_us: numOr(n.gap_us, 8000) },
            ok: t("Sent “{name}” over the air ✓ " +
                  "(that the pulses left the radio — it cannot know a receiver reacted)",
                  { name: signalName(sid) || t("signal {id}", { id: sid }) })
          });
        }
      };
    }
    /* Everything else: no ▶. See the note above for why. */
    return null;
  }

  nodes.forEach(function (n) {
    var ty = nodeType(n.type);
    var p = pos(n);
    var mon = isMonitor(n);
    var play = playAction(n);
    var g = svgEl("g", "node" + (n.enabled === false ? " off" : ""));
    g.setAttribute("transform", "translate(" + p.x + "," + p.y + ")");
    var r = svgEl("rect", "nbox g-" + ty.g + (n.enabled === false ? " off" : ""));
    r.setAttribute("width", NW); r.setAttribute("height", NH);
    r.setAttribute("rx", "10");
    add(g, r);
    /* Every control lives outside the box or on a port line a sink does not
       use, so the labels sit at the same inset on every type — a node with no
       controls at all reads exactly as it always did. Only the 💡 takes room
       inside, and only on the type that has one. */
    var t1 = svgEl("text", "ntitle");
    t1.setAttribute("x", "12"); t1.setAttribute("y", "40");
    t1.textContent = (n.name || ty.label).slice(0, 22);
    add(g, t1);
    var t2 = svgEl("text", "ntype");
    t2.setAttribute("x", "12"); t2.setAttribute("y", "53");
    t2.textContent = ty.ico + " " + ty.label;
    /* A Switch with an RF control signal says so ON THE TYPE LINE, not as a
       third badge. The badge strip is deliberately two slots wide -- ✕ plus
       whichever one control the type has -- and on a Switch the act slot is
       already the I/O button, which is not giving it up: the position is the
       thing you reach for. A radio marker on the subtitle costs no geometry,
       cannot collide with the ports, and answers the question the map raises
       ("why did that flip on its own?") without turning a 168-unit box into a
       toolbar. The full sentence is one tap away in the editor. */
    if (isSwitch(n) && numOr(n.signal_id, 0)) {
      var cn = signalName(n.signal_id) || t("signal {id}", { id: n.signal_id });
      t2.textContent = (t2.textContent + " · 📻 " + cn).slice(0, 26);
    }
    add(g, t2);

    /* 💡 — the whole reason the Monitor node exists on the map: you can watch a
       chain light up node by node instead of reading a log. It takes the act
       slot (a monitor has no ▶) and wears the same badge shape as the others,
       but no hit disc: it is a readout, not a button. refreshMonitors() toggles
       .on and CSS fades it. */
    if (mon) {
      var lamp = badge(g, BADGE_X_ACT, BADGE_Y, "nlamp", null);
      lamp.setAttribute("data-monlamp", n.id);
    }

    /* The logic types' settings, in the slot the lamp and ▶ occupy on other
       types. These nodes have no action to offer, so the space is free — and
       what you want from them at a glance is their VALUE, not their name. */
    if (n.type === "logic.throttle") {
      pill(g, BADGE_X_ACT + BADGE_R, BADGE_Y, "nval",
           shortSecs(numOr(n.window_s, 10)));
    } else if (n.type === "logic.repeat") {
      /* "3x5s" — the two numbers that define it, in the order they happen. */
      pill(g, BADGE_X_ACT + BADGE_R, BADGE_Y, "nval",
           numOr(n.repeats, 3) + "\u00d7" + shortSecs(numOr(n.window_s, 5)));
    } else if (n.type === "logic.group") {
      /* ANY ignores the window entirely, so showing one would be a lie. */
      pill(g, BADGE_X_ACT + BADGE_R, BADGE_Y, "nval",
           n.group_mode === "all"
             ? t("ALL {win}", { win: shortSecs(numOr(n.window_s, 1)) }) : t("ANY"));
    }

    if (hasIn(ty)) {
      var pin = svgEl("circle", "port in");
      pin.setAttribute("cx", "0"); pin.setAttribute("cy", NH / 2); pin.setAttribute("r", "5");
      add(g, pin);
    }
    if (hasOut(ty)) {
      var pout = svgEl("circle", "port out");
      pout.setAttribute("cx", NW); pout.setAttribute("cy", NH / 2); pout.setAttribute("r", "5");
      add(g, pout);
      /* Bigger invisible grab area over the port than the dot it draws. */
      var grab = svgEl("circle", "port-grab");
      grab.setAttribute("cx", NW); grab.setAttribute("cy", NH / 2); grab.setAttribute("r", "13");
      grab.addEventListener("pointerdown", function (ev) {
        ev.stopPropagation();   /* must not also start dragging the node */
        ev.preventDefault();
        linking = { from: n.id };
        svg.classList.add("linking");
      });
      add(g, grab);
    }

    /* ▶ — do this node's own thing, now. On an MQTT button and a Signal
       receiver that is firing its output and nothing more; on a Signal sender
       it is transmitting the code OVER THE AIR, which is audible in someone's
       house. Same glyph, opposite directions, so every one of them names what
       it does in its tooltip and reports back on the message line — see the
       badge notes above. Deliberately no confirmation: it is not destructive,
       and the user asked for a button, not a dialog.

       With a Monitor downstream this is also the whole verification loop for a
       MQTT button: click ▶, watch 💡 light, see the mark land on the
       timeline — no transmitter, no doorbell press, nothing audible. */
    if (play) {
      var off = play.why ? " off" : "";
      var fbg = badge(g, BADGE_X_ACT, BADGE_Y, "nfire" + off, "▶");
      hitDisc(g, BADGE_X_ACT, BADGE_Y, BADGE_HIT, play.tip, function () {
        /* Disabled is shown, not silent: say WHY on the message line rather
           than swallowing the click. */
        if (play.why) { setMsg(canvasMsg, play.why, "err"); return; }
        play.run(function () {
          /* A flash on the badge itself, so the eye that is on the node does
             not have to travel to the message line to know the click landed. */
          if (!fbg.classList) return;
          fbg.classList.add("fired");
          setTimeout(function () { if (fbg.classList) fbg.classList.remove("fired"); }, 550);
        });
      });
    }

    /* I / O — the position of a Switch node, in the act slot (a Switch has no
       ▶: firing one would start a traversal AT it, walking straight past the
       thing it exists to control). Same badge shape as everything else, and it
       is a BUTTON, so the map is somewhere you can actually flip a switch and
       watch the wires after it break.

       The glyph is the mains-rocker pair rather than a word, because at 9 units
       across nothing longer is legible — and the colour carries it anyway: the
       ON badge is the same green as ▶, the OFF one is red and the whole box and
       its outgoing wires go with it. */
    if (isSwitch(n)) {
      var on = switchOn(n);
      var sbg = badge(g, BADGE_X_ACT, BADGE_Y, "nsw" + (on ? " on" : " off"), on ? "I" : "O");
      var csid2 = numOr(n.signal_id, 0);
      /* The tooltip carries the control signal too, so hovering the badge
         explains the 📻 on the subtitle without a trip to the editor. */
      var swTip = csid2
        ? t("   Each press of “{name}” also flips it.",
            { name: signalName(csid2) || t("signal {id}", { id: csid2 }) })
        : "";
      hitDisc(g, BADGE_X_ACT, BADGE_Y, BADGE_HIT,
        (on ? t("Switch “{name}” OFF — blocks everything wired after it",
                { name: n.name || ty.label })
            : t("Switch “{name}” ON — lets this path conduct again",
                { name: n.name || ty.label })) + swTip,
        function () {
          if (sbg.classList) sbg.classList.add("fired");
          setSwitch(n, !on, null, canvasMsg);
        });
    }

    /* ✕ — delete, through the SAME confirmation the editor's Delete uses. The
       confirmation is what makes a bare ✕ on the map safe; a second dialog or a
       second delete path would only be a second thing to keep in step. */
    badge(g, BADGE_X_DEL, BADGE_Y, "ndel", "✕");
    hitDisc(g, BADGE_X_DEL, BADGE_Y, BADGE_HIT,
            t("Delete {name}", { name: n.name || ty.label }), function () {
      deleteNodeConfirmed(n, function (e) {
        setMsg(canvasMsg, e.message, "err");
      });
    });

    add(gNodes, g);

    /* pointer drag -> persist ui_x/ui_y on release */
    var drag = null;
    g.addEventListener("pointerdown", function (ev) {
      if (linking) return;
      var pt = svgPoint(svg, ev);
      drag = { sx: pt.x, sy: pt.y, ox: numOr(n.ui_x, 40), oy: numOr(n.ui_y, 40), moved: false };
      try { g.setPointerCapture(ev.pointerId); } catch (e) { /* ignore */ }
    });
    g.addEventListener("pointermove", function (ev) {
      if (!drag) return;
      var pt = svgPoint(svg, ev);
      /* Clamp to the canvas. Without an upper bound a node could be dragged off
         the right or bottom edge and effectively lost — the viewBox only grows
         to contain it on the NEXT render, so mid-drag it simply vanished. The
         canvas already keeps slack past the furthest node, so there is room to
         move outward; it just now has an edge that stops you. */
      var nx = Math.min(Math.max(0, Math.round(drag.ox + pt.x - drag.sx)), VW - NW);
      var ny = Math.min(Math.max(0, Math.round(drag.oy + pt.y - drag.sy)), VH - NH);
      if (Math.abs(nx - drag.ox) > 3 || Math.abs(ny - drag.oy) > 3) drag.moved = true;
      n.ui_x = nx; n.ui_y = ny;
      g.setAttribute("transform", "translate(" + nx + "," + ny + ")");
      drawLinks();
    });
    function endDrag() {
      if (!drag) return;
      var moved = drag.moved;
      drag = null;
      if (moved) postJSON("/api/graph/nodes/" + n.id, { ui_x: n.ui_x, ui_y: n.ui_y }).catch(function () { /* cosmetic */ });
      else openNodeEditor(n);
    }
    g.addEventListener("pointerup", endDrag);
    g.addEventListener("pointercancel", function () { drag = null; });
  });

  drawLinks();
  add(box, svg);
  add(wrap, box);
  refreshMonitorLamps();
}

/* The canvas renders 1:1 inside a scrolling wrapper, so this is a plain offset
   from the element's top-left. The ratios stay in the maths only so a future
   zoom control cannot silently break dragging. */
function svgPoint(svg, ev) {
  var r = svg.getBoundingClientRect();
  var vb = svg.viewBox.baseVal;
  var sx = r.width ? vb.width / r.width : 1;
  var sy = r.height ? vb.height / r.height : 1;
  return { x: (ev.clientX - r.left) * sx, y: (ev.clientY - r.top) * sy };
}

/* ======================================================================
   SETTINGS
   ====================================================================== */

function section(title, subtitle, open) {
  var d = el("details", "panel");
  if (open) d.open = true;
  var sum = el("summary", "sect-summary", title);
  add(d, sum);
  if (subtitle) add(d, el("p", "hint", subtitle));
  var body = el("div");
  add(d, body);
  d.bodyEl = body;
  return d;
}

function buildSettings() {
  if (S.built.settings) return;
  S.built.settings = true;
  var root = clear($("#tab-settings"));

  add(root, sectionIdentity());
  add(root, sectionWifi());
  add(root, sectionAp());
  add(root, sectionMqtt());
  add(root, sectionRadio());
  add(root, sectionSignals());
  add(root, sectionFirmware());
  add(root, sectionBackup());
  add(root, sectionReboot());
}

function sectionIdentity() {
  var s = section(t("Name on the network"), t("Sets the mDNS hostname: http://<name>.local"), true);
  var input = inputEl("text", (S.sys && S.sys.hostname) || "klingelbox", { maxlength: "31" });
  add(s.bodyEl, field("Hostname", input, t("Letters, digits and hyphens. Applies after a reboot.")));
  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");
  var save = el("button", "btn primary", t("Save hostname"));
  save.type = "button";
  save.addEventListener("click", function () {
    var h = trimOf(input);
    if (!h) { setMsg(msg, t("The hostname cannot be empty."), "err"); return; }
    save.disabled = true;
    setMsg(msg, t("Saving…"));
    postJSON("/api/system/hostname", { hostname: h }).then(function () {
      setMsg(msg, t("Saved. It takes effect on the next reboot: http://{host}.local", { host: h }), "ok");
    }).catch(function (e) { setMsg(msg, e.message, "err"); })
      .then(function () { save.disabled = false; });
  });
  add(foot, save, msg);
  add(s.bodyEl, foot);
  return s;
}

function sectionWifi() {
  var s = section(t("Wi-Fi networks"), t("Up to three home networks, tried in order."));
  var body = s.bodyEl;
  add(body, el("div", "note",
    t("Losing the LAN at runtime never drops this box into setup mode — it keeps retrying " +
      "in the background and stays usable over its own access point.")));
  var slots = [];
  var dl = el("datalist");
  dl.id = "wifi-seen";
  add(body, dl);

  for (var i = 0; i < 3; i++) slots.push(wifiSlot(i, dl));
  slots.forEach(function (sl) { add(body, sl.wrap); });

  var scanRow = el("div", "row");
  var scanBtn = el("button", "btn", t("Scan for networks"));
  scanBtn.type = "button";
  var scanMsg = el("div", "formmsg");
  scanBtn.addEventListener("click", function () {
    scanBtn.disabled = true;
    setMsg(scanMsg, t("Scanning…"));
    api("/api/wifi/scan").then(function (res) {
      var nets = dedupeNetworks(res.networks || []);
      clear(dl);
      nets.forEach(function (nw) { var o = el("option"); o.value = nw.ssid; add(dl, o); });
      setMsg(scanMsg, t("{n} network(s) found — tap an SSID box to pick one.", { n: nets.length }), "ok");
    }).catch(function (e) { setMsg(scanMsg, e.message, "err"); })
      .then(function () { scanBtn.disabled = false; });
  });
  add(scanRow, scanBtn);
  add(body, scanRow, scanMsg);

  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");
  var save = el("button", "btn primary", t("Save networks"));
  save.type = "button";
  save.addEventListener("click", function () {
    var nets = slots.map(function (sl) {
      var o = { ssid: trimOf(sl.ssid) };
      /* Empty password means "leave unchanged" -- never blank a stored secret
         just because the field renders empty. */
      if (sl.pass.value) o.password = sl.pass.value;
      return o;
    });
    save.disabled = true;
    setMsg(msg, t("Saving…"));
    postJSON("/api/config", { sta: { networks: nets } }).then(function () {
      setMsg(msg, t("Saved. The box connects on the next reboot, or when the current network drops."), "ok");
      slots.forEach(function (sl) { sl.pass.value = ""; });
      loadConfig().then(function () { slots.forEach(function (sl) { sl.refresh(); }); });
    }).catch(function (e) { setMsg(msg, e.message, "err"); })
      .then(function () { save.disabled = false; });
  });
  add(foot, save, msg);
  add(body, foot);

  loadConfig().then(function () { slots.forEach(function (sl) { sl.refresh(); }); });
  return s;
}

function dedupeNetworks(list) {
  var best = {};
  list.forEach(function (nw) {
    if (!nw || !nw.ssid) return;
    var cur = best[nw.ssid];
    if (!cur || numOr(nw.rssi, -100) > numOr(cur.rssi, -100)) best[nw.ssid] = nw;
  });
  return Object.keys(best).map(function (k) { return best[k]; })
    .sort(function (a, b) { return numOr(b.rssi, -100) - numOr(a.rssi, -100); });
}

function wifiSlot(idx, dl) {
  var fs = el("fieldset");
  add(fs, el("legend", null, idx === 0
    ? t("Network {n} (tried first)", { n: idx + 1 })
    : t("Network {n}", { n: idx + 1 })));
  var ssid = inputEl("text", "", { maxlength: "32", list: dl.id, placeholder: "SSID" });
  var pass = inputEl("password", "", { maxlength: "63", placeholder: t("leave empty to keep") });
  pass.autocomplete = "new-password";
  var passHint = el("span", "hint", "");
  add(fs, field(t("Network name"), ssid));
  var pf = field("Passphrase", pass);
  add(pf, passHint);
  add(fs, pf);
  function refresh() {
    var nets = (S.config && S.config.sta && S.config.sta.networks) || [];
    var n = nets[idx] || {};
    ssid.value = n.ssid || "";
    passHint.textContent = n.has_pass
      ? t("A passphrase is stored. Leave empty to keep it; type a new one to replace it.")
      : t("No passphrase stored (open network, or slot unused).");
  }
  refresh();
  return { wrap: fs, ssid: ssid, pass: pass, refresh: refresh };
}

function sectionAp() {
  var s = section(t("Access point"), t("The box's own hotspot -- how you reach it with no LAN."));
  var body = s.bodyEl;
  var msg = el("div", "formmsg");
  var loading = el("div", "empty", t("Loading…"));
  add(body, loading);

  api("/api/ap").then(function (ap) {
    S.ap = ap;
    loading.remove();
    var grid = el("div", "formgrid");
    var ssid = inputEl("text", ap.ssid || "", { maxlength: "32" });
    var chan = inputEl("number", numOr(ap.channel, 6), { min: "1", max: "13", step: "1", inputmode: "numeric" });
    var enabled = checkField(t("Access point enabled"), ap.enabled !== false);
    var fallback = checkField(t("Raise a setup hotspot at boot when no network connects"),
      ap.fallback_enabled !== false,
      t("This is what puts a factory-fresh box on the air. Turning it off can lock you out."));
    var rpass = inputEl("password", "", { maxlength: "63",
      placeholder: ap.has_recovery_pass ? t("set - leave empty to keep") : t("empty = open hotspot") });
    rpass.autocomplete = "new-password";
    add(grid, field(t("Hotspot name (SSID)"), ssid, null, "full"));
    add(grid, field(t("Channel"), chan));
    var ipRow = field(t("IP address"), inputEl("text", ap.ip || "", {}), t("Read-only."));
    $("input", ipRow).disabled = true;
    add(grid, ipRow);
    add(body, grid);
    add(body, enabled);
    add(body, fallback);
    add(body, field(t("Setup hotspot passphrase"), rpass,
      t("8-63 characters, or leave empty to keep the current setting. An open hotspot is fine on a trusted site.")));

    var foot = el("div", "formfoot");
    var save = el("button", "btn primary", t("Save access point"));
    save.type = "button";
    save.addEventListener("click", function () {
      var body2 = {
        ssid: trimOf(ssid),
        channel: intOf(chan, 6),
        enabled: enabled.input.checked,
        fallback_enabled: fallback.input.checked
      };
      if (rpass.value) body2.recovery_pass = rpass.value;
      save.disabled = true;
      setMsg(msg, t("Saving…"));
      postJSON("/api/ap", body2).then(function () {
        setMsg(msg, t("Saved. Changes to the hotspot apply on the next reboot."), "ok");
        rpass.value = "";
      }).catch(function (e) { setMsg(msg, e.message, "err"); })
        .then(function () { save.disabled = false; });
    });
    add(foot, save, msg);
    add(body, foot);
  }).catch(function (e) {
    loading.remove();
    S.has.ap = false;
    add(body, el("div", "note warn", t("Access-point settings are not available on this firmware ({err}).", { err: e.message })));
  });
  return s;
}

function sectionMqtt() {
  var s = section("MQTT / Home Assistant", t("Publish presses to a broker and get discovered by HA."));
  var body = s.bodyEl;
  var loading = el("div", "empty", t("Loading…"));
  add(body, loading);

  loadConfig().then(function (cfg) {
    loading.remove();
    if (!cfg || !cfg.mqtt) {
      add(body, el("div", "note warn", t("This firmware does not expose MQTT settings.")));
      return;
    }
    var m = cfg.mqtt;
    var enabled = checkField(t("MQTT enabled"), !!m.enabled);
    add(body, enabled);
    var grid = el("div", "formgrid");
    var host = inputEl("text", m.host || "", { maxlength: "63", placeholder: "192.168.1.10" });
    var port = inputEl("number", numOr(m.port, 1883), { min: "1", max: "65535", step: "1", inputmode: "numeric" });
    var user = inputEl("text", m.user || "", { maxlength: "63" });
    var pass = inputEl("password", "", { maxlength: "63", placeholder: t("leave empty to keep") });
    pass.autocomplete = "new-password";
    var base = inputEl("text", m.base_topic || "klingelbox", { maxlength: String(TOPIC_MAX) });
    var disc = inputEl("text", m.discovery_prefix || "homeassistant", { maxlength: String(TOPIC_MAX) });
    var baseErr = el("div", "hint");
    var discErr = el("div", "hint");
    var ha = checkField(t("Home Assistant discovery"), m.homeassistant !== false);
    add(grid, field(t("Broker host"), host, null, "full"));
    add(grid, field("Port", port));
    add(grid, field(t("Username"), user));
    add(grid, field(t("Password"), pass, t("Never sent back to this page."), "full"));
    /* These two matter more than any single node's topic: the base topic is the
       prefix of EVERY topic the box publishes, and the discovery prefix is the
       root of everything Home Assistant reads. A '#' in either does not break
       one entity, it takes the whole bridge down. Same rule, same words, same
       moment — as you type. */
    var baseField = field(t("Base topic"), base,
      t("Everything is published under this prefix, and virtual triggers listen on <base>/trigger/<topic>."));
    add(baseField, baseErr);
    add(grid, baseField);
    var discField = field(t("Discovery prefix"), disc);
    add(discField, discErr);
    add(grid, discField);
    var baseCheck = bindTopicCheck(base, baseErr, "mqtt.base_topic");
    var discCheck = bindTopicCheck(disc, discErr, "mqtt.discovery_prefix");
    add(body, grid, ha);

    var msg = el("div", "formmsg");
    var foot = el("div", "formfoot");
    var save = el("button", "btn primary", t("Save MQTT"));
    save.type = "button";
    save.addEventListener("click", function () {
      /* Refused here rather than as a server error, and refused BEFORE the rest
         of the form is sent: this handler posts host, port and credentials in
         the same body, and a rejected topic would otherwise mean a round trip
         that saved nothing. */
      var baseOk = baseCheck(), discOk = discCheck();
      if (!baseOk || !discOk) {
        setMsg(msg, !baseOk
          ? topicError(trimOf(base), "mqtt.base_topic", TOPIC_MAX)
          : topicError(trimOf(disc), "mqtt.discovery_prefix", TOPIC_MAX), "err");
        return;
      }
      var mm = {
        enabled: enabled.input.checked,
        host: trimOf(host),
        port: intOf(port, 1883),
        user: trimOf(user),
        /* Empty still means "use the default" — that has always been true and
           the firmware resolves it, so it is not a validation failure. */
        base_topic: trimOf(base) || "klingelbox",
        homeassistant: ha.input.checked,
        discovery_prefix: trimOf(disc) || "homeassistant"
      };
      if (pass.value) mm.password = pass.value;
      save.disabled = true;
      setMsg(msg, t("Saving…"));
      postJSON("/api/config", { mqtt: mm }).then(function () {
        setMsg(msg, t("Saved."), "ok");
        pass.value = "";
        return loadConfig();
      }).catch(function (e) { setMsg(msg, e.message, "err"); })
        .then(function () { save.disabled = false; });
    });
    add(foot, save, msg);
    add(body, foot);
  }).catch(function (e) {
    loading.remove();
    add(body, el("div", "note warn", t("Could not load MQTT settings: {err}", { err: e.message })));
  });
  return s;
}

function sectionRadio() {
  var s = section(t("Radio"), t("433 MHz front end. Change these only if you know why."));
  var body = s.bodyEl;
  var loading = el("div", "empty", t("Loading…"));
  add(body, loading);

  api("/api/radio").then(function (r) {
    S.radio = r;
    loading.remove();
    var grid = el("div", "formgrid");
    var freq = inputEl("number", numOr(r.freq_hz, 433920000), { min: "300000000", max: "928000000", step: "1000", inputmode: "numeric" });
    var mod = selectEl([{ value: "ook", label: "OOK / ASK" }, { value: "fsk", label: "FSK" }],
      r.modulation || "ook");
    var rate = inputEl("number", numOr(r.datarate_bps, 5000), { min: "600", max: "250000", step: "100", inputmode: "numeric" });
    var bw = inputEl("number", numOr(r.bandwidth_hz, 203000), { min: "58000", max: "812000", step: "1000", inputmode: "numeric" });
    var pwr = inputEl("number", numOr(r.tx_power_dbm, 10), { min: "-30", max: "12", step: "1", inputmode: "numeric" });
    var reps = inputEl("number", numOr(r.tx_repeats, 6), { min: "1", max: "32", step: "1", inputmode: "numeric" });
    var gap = inputEl("number", numOr(r.tx_gap_us, 8000), { min: "500", max: "60000", step: "500", inputmode: "numeric" });
    add(grid, field(t("Frequency (Hz)"), freq, t("433920000 for European doorbells."), "full"));
    add(grid, field("Modulation", mod));
    add(grid, field(t("Data rate (bps)"), rate));
    add(grid, field(t("Receive bandwidth (Hz)"), bw));
    add(grid, field(t("TX power (dBm)"), pwr, t("Keep this modest: 433 MHz duty-cycle limits vary by region.")));
    add(grid, field(t("Default TX repeats"), reps));
    add(grid, field(t("Default TX gap (us)"), gap));
    add(body, grid);
    if (typeof r.rssi_dbm === "number") {
      add(body, el("div", "note",
        t("Current noise floor: {dbm} dBm. On a quiet band this sits near -95 dBm; " +
          "a real press in the same room measures -24 to -42 dBm.", { dbm: r.rssi_dbm })));
    }
    var msg = el("div", "formmsg");
    var foot = el("div", "formfoot");
    var save = el("button", "btn primary", t("Apply to the radio"));
    save.type = "button";
    save.addEventListener("click", function () {
      save.disabled = true;
      setMsg(msg, t("Reconfiguring…"));
      postJSON("/api/radio", {
        freq_hz: intOf(freq, 433920000),
        modulation: mod.value,
        datarate_bps: intOf(rate, 5000),
        bandwidth_hz: intOf(bw, 203000),
        tx_power_dbm: intOf(pwr, 10),
        tx_repeats: intOf(reps, 6),
        tx_gap_us: intOf(gap, 8000)
      }).then(function (res) {
        S.radio = res && res.freq_hz ? res : S.radio;
        setMsg(msg, t("Applied live — no reboot needed."), "ok");
        renderStatusChips();
      }).catch(function (e) { setMsg(msg, e.message, "err"); })
        .then(function () { save.disabled = false; });
    });
    add(foot, save, msg);
    add(body, foot);
  }).catch(function (e) {
    loading.remove();
    S.has.radioCfg = false;
    add(body, el("div", "note bad",
      t("The radio is not answering, so its parameters cannot be read or changed ({err}). " +
        "Open Diagnostics for the probe result.", { err: e.message })));
  });
  return s;
}

/* ----------------------------------------------------------------------
   Stored signals -- housekeeping, and the ONLY place a signal is destroyed.

   The store outlives the graph on purpose: a learned waveform is a recording
   of a physical remote, and deleting a node must never cost a walk to the
   front door with that remote in hand. So nothing on the Dashboard can remove
   a signal, and this list exists so an entry you genuinely never want again
   can still go away without reflashing anything.

   The LIST stays a maintenance list -- name, identity, in-use marker, age --
   and a tap opens the full detail sheet: the very same signalBlock() the node
   editor embeds, so the waveform, the confidence meter, the pairing panel and
   Transmit are all here without a second implementation of any of them, plus
   the delete this screen alone is allowed to offer.

   Transmit being here is the point at which a signal no node uses stops being
   a dead entry: it can be test-fired straight from the store.
   ---------------------------------------------------------------------- */
var settingsSigRender = null;   /* set once Settings is built */

function sectionSignals() {
  /* The blurb and the create buttons sit BELOW the list: the list is what you
     came for, and pushing it under two paragraphs of explanation buries it. */
  var s = section(t("Stored signals"), "", false);
  /* Signals can be born here too, not only while wiring a node. Learning a
     remote you want on the box before you have decided what it should DO is a
     perfectly normal order to work in, and hand-entering a code you already know
     never needed a node at all. Both reuse the same flows the node builder
     uses — one implementation, so they cannot drift. */
  var listWrap = el("div");
  add(s.bodyEl, listWrap);

  /* ...and everything explanatory goes after it. */
  add(s.bodyEl, el("div", "divider"));
  add(s.bodyEl, el("p", "muted",
    t("Every waveform the box has learned or synthesized. Nodes come and go without "
      + "touching this list — removing one here is permanent.")));

  var mkRow = el("div", "row");
  /* Two ways to make a signal, and only two: hear one, or invent one. There
     used to be three, because "learn" and "raw capture" were separate screens
     doing the same job with different thresholds. */
  if (S.has.raw) {
    add(mkRow, listenButton(t("\u{1F3A7} Listen for a signal"), function (sig) {
      if (sig) { loadSignals(); if (S.graph) loadGraph(); }
    }));
  }
  var bMake = el("button", "btn", t("\u2728 Create custom or random"));
  bMake.type = "button";
  bMake.addEventListener("click", function () {
    openVirtualFlow({ mode: "sink" }).then(function (sig) { if (sig) loadSignals(); });
  });
  add(mkRow, bMake);

  /* Import exactly ONE signal.
     Separate from Settings -> Backup on purpose: this is "someone sent me a
     doorbell code", not "move a whole box". It therefore REFUSES a full bundle
     rather than quietly importing the first signal out of it and dropping the
     rest -- a partial restore that reports success is the failure mode worth
     designing out. The file format is identical either way, so the same export
     works in both places; only the acceptance rule differs. */
  var bImp = el("button", "btn", t("\u2b06 Import a signal"));
  bImp.type = "button";
  bImp.addEventListener("click", function () { openImportSignal(); });
  add(mkRow, bImp);

  add(s.bodyEl, mkRow);
  add(s.bodyEl, el("div", "hint",
    t("A signal created here belongs to no node yet. Wire it up later from the "
      + "Dashboard, or test it straight away by opening it above.")));

  /* The rows stay a maintenance list: name, what it decodes to, whether a node
     uses it. Everything else is one tap away, in the same signalBlock() the
     node editor shows -- a signal must look and behave identically wherever
     you meet it, so there is exactly one rendering of one. */
  function render() {
    clear(listWrap);
    if (S.signalsErr) {
      add(listWrap, el("div", "note bad",
        t("Could not read the signal store: {err}", { err: S.signalsErr.message })));
      return;
    }
    var list = S.signals || [];
    if (!list.length) {
      add(listWrap, el("div", "empty",
        t("Nothing stored yet. Signals appear here once you listen for a button or create a " +
          "virtual one while adding a node.")));
      return;
    }
    var ul = el("ul", "list");
    list.forEach(function (sig) {
      var users = nodesUsingSignal(sig.id, 0);
      var li = el("li");
      var b = el("button", "listitem"); b.type = "button";
      add(b, el("span", "li-ico", sig.origin === "synthesized" ? "✨" : "📥"));
      var main = el("div", "li-main");
      add(main, el("div", "li-title", signalLabel(sig)));
      add(main, el("div", "li-sub", signalIdent(sig) + "  ·  " +
        (sig.origin === "synthesized" ? t("virtual") : t("learned"))));
      add(main, el("div", "li-sub", usedByText(users)));
      add(b, main);
      var meta = el("div", "li-meta");
      if (typeof sig.last_seen_s === "number") add(meta, el("div", null, agoText(sig.last_seen_s)));
      add(b, meta);
      b.setAttribute("aria-label", t("Open {name}", { name: signalLabel(sig) }));
      b.title = t("Waveform, transmit, rename, delete");
      b.addEventListener("click", function () { openStoredSignal(sig.id); });
      add(li, b);
      add(ul, li);
    });
    add(listWrap, ul);
  }

  settingsSigRender = render;
  render();
  return s;
}

/* The full detail view of a stored signal, reached from the list above.
 *
 * The body is signalBlock() -- the SAME fragment the node editor embeds -- so
 * identity, the "no decoder claimed this" note, the confidence meter, base
 * pulse, pulse count, RSSI, seen/last-seen, the inline waveform with its
 * 49-vs-50 note, the pairing panel for a synthesized code and Transmit are all
 * here by construction, and can never drift from the node editor's version.
 *
 * That also makes this the place a signal NO node uses can still be test-fired:
 * the store is reachable on its own, so a waveform does not have to be wired to
 * something before you can hear whether it works.
 *
 * Only the destructive action is added on top, because this is the one screen
 * that is allowed to have one. */
function openStoredSignal(id) {
  var sh = openSheet("Signal", null);
  add(sh.body, el("div", "empty", t("Loading…")));
  api("/api/signals/" + id).then(function (sig) {
    clear(sh.body);
    $("h3", sh.sheet).textContent = signalLabel(sig);

    add(sh.body, signalBlock(sig, {
      storeNote: false,
      onChanged: function (what, value) {
        if (what === "renamed") $("h3", sh.sheet).textContent = value;
      }
    }));

    /* --- the one destructive action in the whole UI --- */
    var users = nodesUsingSignal(sig.id, 0);
    add(sh.body, el("div", "divider"));
    add(sh.body, el("div", "lg-label", t("Remove from the box")));
    add(sh.body, el("div", "hint", users.length
      ? t("{used}. Deleting leaves those nodes without a signal until you give " +
          "them another one — they are not deleted with it.", { used: usedByText(users) })
      : t("No node uses this signal, so deleting it changes nothing in your graph.")));

    var msg = el("div", "formmsg");
    var foot = el("div", "formfoot");
    var del = el("button", "btn danger", t("Delete signal"));
    del.type = "button";
    del.addEventListener("click", function () {
      var lines = [t("The stored waveform is removed permanently. " +
        "If it came off a remote you will have to learn that button again.")];
      if (users.length) {
        lines.push(t("{used}. Those nodes keep existing but are left without a " +
          "signal, and do nothing until you give them another one.", { used: usedByText(users) }));
      } else {
        lines.push(t("No node uses it, so nothing in your graph changes."));
      }
      confirmSheet(t("Delete “{name}”?", { name: signalLabel(sig) }), lines, t("Delete"), true).then(function (ok) {
        if (!ok) return;
        setMsg(msg, t("Deleting…"));
        api("/api/signals/" + sig.id, { method: "DELETE" }).then(function () {
          sh.close();
          loadSignals();
          if (S.graph) loadGraph();
        }).catch(function (e) { setMsg(msg, e.message, "err"); });
      });
    });
    add(foot, del, msg);
    add(sh.body, foot);
  }).catch(function (e) {
    clear(sh.body);
    add(sh.body, el("div", "note bad", t("Could not load this signal: {err}", { err: e.message })));
  });
}

/*
 * The one place a "the box downloads and flashes it itself" request becomes UI.
 *
 * Shared by the URL form and by the update-check Install buttons on purpose:
 * both start the SAME background OTA in ota.c and both end in a reboot, so they
 * must confirm the same way, use the same in-flight wording and give the same
 * recovery hint. A second implementation would inevitably drift from this one.
 */
function startOta(path, payload, what, btn, msgNode, lines) {
  return confirmSheet(t("Update the {what}?", { what: what }), lines, t("Update")).then(function (ok) {
    if (!ok) return;
    btn.disabled = true;
    setMsg(msgNode, t("Downloading and flashing… this can take a minute."));
    return postJSON(path, payload).then(function () {
      setMsg(msgNode, t("Flashed. The box is rebooting — reload this page in a few seconds."), "ok");
    }).catch(function (e) { setMsg(msgNode, e.message, "err"); })
      .then(function () { btn.disabled = false; });
  });
}

/*
 * "Is there a newer release?" -- GET /api/update, POST /api/update/check.
 *
 * HIDES ITSELF ON 404 (rule 3 of the file header): a firmware built before this
 * endpoint existed has no update check, and the rest of the update section --
 * URL and browser upload -- must keep working on it untouched.
 *
 * The check itself is asynchronous on the box (a TLS fetch from GitHub), so the
 * button POSTs /api/update/check and then this polls the cheap local GET until
 * `checking` clears. The POST is never polled: the firmware rate-limits it
 * because GitHub allows only 60 anonymous requests an hour per address.
 */
var updateRefresh = null;   /* set once Settings is built; null = no such endpoint */

function updateCheckBlock() {
  var wrap = el("div", "hidden");
  add(wrap, el("h3", null, t("Newer release")));
  add(wrap, el("p", "hint",
    t("Asks GitHub for the newest published release. The answer is cached for a few hours — " +
      "GitHub rate-limits anonymous requests, so checking is deliberately not automatic.")));

  var chips = el("div", "chiprow");
  var info = el("div");
  info.style.marginTop = ".4rem";
  var msg = el("div", "formmsg");
  var row = el("div", "btnrow");
  row.style.marginTop = ".6rem";

  var checkBtn = el("button", "btn", t("Check for updates"));
  checkBtn.type = "button";
  var appBtn = el("button", "btn primary hidden", t("Install firmware"));
  appBtn.type = "button";
  var uiBtn = el("button", "btn hidden", t("Install web UI"));
  uiBtn.type = "button";
  add(row, checkBtn, appBtn, uiBtn);
  add(wrap, chips, info, row, msg);

  var last = null;

  function show(btn, on) {
    if (on) btn.classList.remove("hidden"); else btn.classList.add("hidden");
  }

  function render(st) {
    last = st;
    wrap.classList.remove("hidden");
    clear(chips);
    clear(info);

    add(chips, el("span", "chip mono", t("running {version}", { version: st.current || "?" })));
    if (st.checking) {
      add(chips, el("span", "chip", t("checking…")));
    } else if (st.valid) {
      add(chips, el("span", "chip mono", t("latest {version}", { version: st.latest || "?" })));
      add(chips, st.update_available ? el("span", "chip warn", t("update available"))
                                     : el("span", "chip ok", t("up to date")));
    } else if (!st.error) {
      add(chips, el("span", "chip", t("not checked yet")));
    }

    if (st.error) add(info, el("div", "note warn", st.error));

    if (st.valid && st.html_url) {
      var a = el("a", "small", t("Release notes for {version} ↗", { version: st.latest || t("the newest release") }));
      a.href = st.html_url;
      a.target = "_blank";
      a.rel = "noopener noreferrer";
      /* No <a> anywhere else in this UI, so it carries its own colour rather
         than a stylesheet rule that would exist for one link. */
      a.style.color = "var(--accent)";
      add(info, a);
    }
    if (st.valid && st.checked_at_s) {
      add(info, el("div", "hint", t("Checked {ago}.", { ago: agoText(st.checked_at_s) })));
    }
    /* The box downloads the image itself, so no home network means no check and
       no install -- the browser upload below is the answer in that case. */
    if (st.sta_connected === false) {
      add(info, el("div", "note",
        t("The box is not on a home network, so it cannot reach GitHub. Upload an image from " +
          "this device instead — that path needs no internet on the box at all.")));
    }

    checkBtn.disabled = !!st.checking || st.sta_connected === false;
    checkBtn.textContent = st.checking ? t("Checking…") : t("Check for updates");
    show(appBtn, !!(st.update_available && st.app_url));
    show(uiBtn, !!(st.update_available && st.webui_url));
  }

  function refresh() {
    return api("/api/update").then(function (st) {
      render(st);
      return st;
    }).catch(function (e) {
      stopPoll("update");
      /* 404: this firmware predates the update check. Say nothing, show nothing. */
      if (e.status === 404) { wrap.remove(); updateRefresh = null; return null; }
      setMsg(msg, e.message, "err");
      return null;
    });
  }

  /* The polled variant: it retires its own timer the moment the box says the
     fetch has finished, so this never outlives one check. */
  function tick() {
    refresh().then(function (st) { if (!st || !st.checking) stopPoll("update"); });
  }

  checkBtn.addEventListener("click", function () {
    setMsg(msg, "");
    checkBtn.disabled = true;
    checkBtn.textContent = t("Checking…");
    postJSON("/api/update/check", { force: true }).then(function (st) {
      render(st);
      /* The fetch runs on the box; poll the local status until it settles. */
      if (st.checking) poll("update", 2000, tick);
    }).catch(function (e) {
      setMsg(msg, e.message, "err");
      checkBtn.disabled = false;
      checkBtn.textContent = t("Check for updates");
    });
  });

  function install(webui, btn) {
    if (!last) return;
    var url = webui ? last.webui_url : last.app_url;
    startOta("/api/update/install", { webui: webui },
      t("{what} to {version}", { what: webui ? t("web UI") : t("firmware"),
                                 version: last.latest || t("the newest release") }),
      btn, msg,
      [url,
       t("The box downloads and flashes it, then reboots. Do not power it off."),
       webui ? t("The current web UI is erased first; the firmware and your signals are untouched.")
             : t("Only the app is replaced. Update the web UI as a second step once the box is back.")]);
  }
  appBtn.addEventListener("click", function () { install(false, appBtn); });
  uiBtn.addEventListener("click", function () { install(true, uiBtn); });

  /* Settings is built exactly once, but this block's answer can change while
     the box runs (a check finishes on the device, or the STA comes back), so
     re-entering the tab re-reads it. */
  updateRefresh = function () {
    refresh().then(function (st) { if (st && st.checking) poll("update", 2000, tick); });
  };
  updateRefresh();
  return wrap;
}

function sectionFirmware() {
  var s = section(t("Firmware & web UI update"),
    t("Two separate images: the app, and this web UI. Updating one leaves the other alone."));
  var body = s.bodyEl;

  add(body, el("div", "note",
    t("The app image and the web UI live in different partitions. After an app update the old " +
      "UI is still being served until you update it too — that is normal, not a failure.")));

  /* --- is there a newer release? --- */
  add(body, updateCheckBlock());

  /* --- from a URL ---
     TWO fields, not one. There are two images and they live at two different
     URLs, and a single box that had to be re-typed between the two buttons was
     how "Update web UI" ended up flashing an app image at people. Each is
     prefilled with the stable release asset for its own kind, so the common
     case is a click. */
  add(body, el("div", "divider"));
  add(body, el("h3", null, t("From a URL")));
  add(body, el("div", "note",
    t("Prefilled with the latest stable release of each image. They are ordinary text fields — " +
      "point them at a fork, a test build or a file on your own web server and the box fetches " +
      "that instead. The automatic check above needs none of this: it uses the URLs it finds in " +
      "the release itself.")));

  var urlIn = inputEl("url", "", { placeholder: OTA_DEFAULT_APP_URL });
  urlIn.type = "url";
  add(body, field(t("Firmware image URL"), urlIn,
    t("The box downloads it itself, so it needs working internet.")));
  var uiUrlIn = inputEl("url", "", { placeholder: OTA_DEFAULT_WEBUI_URL });
  uiUrlIn.type = "url";
  add(body, field(t("Web UI image URL"), uiUrlIn,
    t("The second, separate image — this page itself.")));

  var urlMsg = el("div", "formmsg");
  var urlRow = el("div", "btnrow");
  var appBtn = el("button", "btn primary", t("Update firmware"));
  appBtn.type = "button";
  var uiBtn = el("button", "btn", t("Update web UI"));
  uiBtn.type = "button";
  function urlUpdate(path, what, btn, input) {
    var u = trimOf(input);
    if (!u) { setMsg(urlMsg, t("Enter the {what} image URL first.", { what: t(what) }), "err"); return; }
    startOta(path, { url: u }, t("{what} from this URL", { what: t(what) }), btn, urlMsg,
      [u, t("The box downloads and flashes it, then reboots. Do not power it off.")]);
  }
  appBtn.addEventListener("click", function () { urlUpdate("/api/ota", "firmware", appBtn, urlIn); });
  uiBtn.addEventListener("click", function () { urlUpdate("/api/ota/webui", "web UI", uiBtn, uiUrlIn); });
  add(urlRow, appBtn, uiBtn);
  add(body, urlRow, urlMsg);

  /* Prefill only what is EMPTY, and never over something already typed: the
     stored ota.url wins (it is what this box was last told to use), then the
     box's own defaults, then the constants above for a firmware too old to
     serve them. */
  urlIn.value = OTA_DEFAULT_APP_URL;
  uiUrlIn.value = OTA_DEFAULT_WEBUI_URL;
  loadConfig().then(function (cfg) {
    var o = (cfg && cfg.ota) || {};
    if (o.url) urlIn.value = o.url;
    else if (o.default_url) urlIn.value = o.default_url;
    if (o.default_webui_url) uiUrlIn.value = o.default_webui_url;
  }).catch(function () { /* the constants above already filled both in */ });

  /* --- browser upload --- */
  add(body, el("div", "divider"));
  add(body, el("h3", null, t("Or upload from this device")));
  add(body, el("p", "hint",
    t("Works with no internet on the box at all — useful over the setup hotspot.")));

  var prog = el("div", "progress hidden");
  var bar = el("i");
  add(prog, bar);
  var upMsg = el("div", "formmsg");

  function uploadRow(labelText, path, what) {
    var f = el("input");
    f.type = "file";
    f.accept = ".bin,application/octet-stream";
    f.style.fontSize = "1rem";
    f.style.padding = ".55rem 0";
    var wrap = field(labelText, f);
    var b = el("button", "btn", t("Upload & flash"));
    b.type = "button";
    b.style.marginTop = ".3rem";
    b.addEventListener("click", function () {
      var file = f.files && f.files[0];
      if (!file) { setMsg(upMsg, t("Choose a .bin file first."), "err"); return; }
      confirmSheet(t("Flash “{name}” as the new {what}?", { name: file.name, what: t(what) }),
        [t("{size} MB, written into flash as it arrives.", { size: (file.size / 1048576).toFixed(2) }),
         what === "web UI"
           ? t("The current web UI is erased first. If the upload is interrupted the UI stays blank until a good image is pushed; the firmware and your signals are untouched.")
           : t("The image is validated before it becomes the boot target, and a bad one is rolled back on the next boot."),
         t("Do not close this page or power the box off during the upload.")],
        t("Upload & flash")).then(function (ok) {
        if (!ok) return;
        b.disabled = true;
        prog.classList.remove("hidden");
        bar.style.width = "0%";
        setMsg(upMsg, t("Uploading…"));
        var xhr = new XMLHttpRequest();
        xhr.open("POST", path);
        xhr.setRequestHeader("Content-Type", "application/octet-stream");
        xhr.timeout = 10 * 60 * 1000;
        xhr.upload.onprogress = function (ev) {
          if (!ev.lengthComputable) return;
          var pc = Math.round(ev.loaded * 100 / ev.total);
          bar.style.width = pc + "%";
          setMsg(upMsg, t("Uploading… {pc}%", { pc: pc }));
        };
        xhr.onload = function () {
          var res = {};
          try { res = JSON.parse(xhr.responseText || "{}"); } catch (e) { /* non-JSON */ }
          b.disabled = false;
          if (xhr.status === 200 && !res.error) {
            bar.style.width = "100%";
            setMsg(upMsg, t("Flashed. The box is rebooting — reload this page in a few seconds."), "ok");
          } else {
            prog.classList.add("hidden");
            setMsg(upMsg, t("Upload failed: {err}", { err: res.error || ("HTTP " + xhr.status) }), "err");
          }
        };
        xhr.onerror = xhr.ontimeout = function () {
          b.disabled = false;
          prog.classList.add("hidden");
          setMsg(upMsg, t("Upload failed: the connection dropped. If the box was mid-flash it reboots on its current image."), "err");
        };
        xhr.send(file);
      });
    });
    add(wrap, b);
    return wrap;
  }
  /* The release asset is called klingelbox.bin; the label said doorbell433.bin,
     which is the project's old name and matches no file anyone can download. */
  add(body, uploadRow(t("Firmware image (klingelbox.bin)"), "/api/ota/upload", "firmware"));
  add(body, uploadRow(t("Web UI image (storage.bin)"), "/api/ota/webui/upload", "web UI"));
  add(body, prog, upMsg);
  return s;
}

/* ---------------------------------------------------------------- backup --

   Move a box's learned signals AND its automations to another Klingelbox.

   THE BROWSER ORCHESTRATES; THE FIRMWARE NEVER HOLDS THE DOCUMENT. Please do
   not "simplify" the four functions below into one GET /api/backup and one
   POST /api/backup. Measured on the live box: ~126 KB of free heap, and a full
   backup of a filled store is ~86 KB of JSON (32 signals x ~2.7 KB of
   durations_us, plus the graph). cJSON needs the body string AND a parse tree
   of two to three times the document alive at the same moment, so a
   whole-bundle endpoint would ask for roughly 300 KB on a box that has 126 KB
   -- while Wi-Fi buffers and this very connection are also holding heap. It
   would not be slow. It would run the box out of memory.

   So the bundle is assembled HERE out of endpoints that already exist and are
   already streamed (GET /api/signals, GET /api/signals/{id}, GET /api/graph),
   and restored HERE one item at a time through per-item endpoints that ordinary
   use exercises every day. Every request stays a few kilobytes. The price is
   that an import is NOT atomic, and the price is paid honestly: the summary
   says what actually got in, and what did not, and why.

   ID REMAPPING is the whole difficulty. Signal ids and node ids are handed out
   by whichever box you are restoring INTO, so a graph carried over from another
   box points at nothing -- or, worse, at somebody else's doorbell. The order is
   not negotiable:
     1. signals first, recording old id -> new id as the device assigns them;
     2. nodes next, with signal_id rewritten through that map, recording
        old node id -> new node id;
     3. links last, with BOTH endpoints rewritten.

   NO SECRETS EVER LEAVE. A backup file gets mailed around and dropped in cloud
   folders. It carries what the doorbell KNOWS -- waveforms and wiring -- and
   never what it can LOG IN TO: no Wi-Fi SSID or passphrase, no AP password, no
   MQTT host or credentials, not even hashes. */

/* ----------------------------------------------------------- import one signal
   Accepts a bundle carrying exactly one signal and no graph -- i.e. what the
   per-signal Export produces. A full backup is refused WITH ITS CONTENTS NAMED
   and a pointer to Settings -> Backup, because "wrong file" is not a useful
   thing to tell someone holding a file that is perfectly valid elsewhere. */
function openImportSignal() {
  var sh = openSheet(t("Import a signal"), "");
  var body = sh.bodyEl;

  add(body, el("p", "muted",
    t("Choose a .json exported from a signal's Export button \u2014 on this box or another one. " +
      "The waveform is re-analysed here, so the imported signal behaves exactly like one you learned yourself.")));

  var file = el("input");
  file.type = "file";
  file.accept = ".json,application/json";
  file.style.fontSize = "1rem";
  file.style.padding = ".55rem 0";
  add(body, field(t("Signal file"), file, t("Nothing is written until you choose Import below.")));

  var nameIn = inputEl("text", "", { maxlength: "31", placeholder: t("leave empty to keep the exported name") });
  add(body, field(t("Name on this box"), nameIn, t("Optional \u2014 useful when you already have a signal with the same name.")));

  var info = el("div", "note hidden");
  add(body, info);

  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");
  var go = el("button", "btn primary", t("Import"));
  go.type = "button";
  go.disabled = true;
  var cancel = el("button", "btn ghost", t("Cancel"));
  cancel.type = "button";
  cancel.addEventListener("click", function () { sh.close(); });
  add(foot, go, cancel, msg);
  add(body, foot);

  var pending = null;

  file.addEventListener("change", function () {
    pending = null; go.disabled = true;
    info.classList.add("hidden");
    setMsg(msg, "");
    var f = file.files && file.files[0];
    if (!f) return;
    var rd = new FileReader();
    rd.onload = function () {
      var b;
      try { b = JSON.parse(String(rd.result)); }
      catch (e) { setMsg(msg, t("That file is not valid JSON."), "err"); return; }

      if (!b || b.kind !== BACKUP_KIND) {
        setMsg(msg, t("That is not a Klingelbox export."), "err"); return;
      }
      if (numOr(b.version, 0) > BACKUP_VERSION) {
        setMsg(msg, t("That file was written by a newer Klingelbox (format v{version}). Update this box first.",
                      { version: b.version }), "err"); return;
      }
      var sigs = isArr(b.signals) ? b.signals : [];
      var nodes = (b.graph && isArr(b.graph.nodes)) ? b.graph.nodes : [];
      var links = (b.graph && isArr(b.graph.links)) ? b.graph.links : [];

      if (sigs.length !== 1 || nodes.length || links.length) {
        /* Name what it actually is, and where it DOES work. */
        var what = (sigs.length === 1 ? t("{n} signal", { n: sigs.length })
                                      : t("{n} signals", { n: sigs.length })) +
                   (nodes.length ? (nodes.length === 1 ? t(", {n} node", { n: nodes.length })
                                                       : t(", {n} nodes", { n: nodes.length })) : "");
        setMsg(msg, t("This looks like a full backup ({what}). Import it from Settings \u2192 Backup " +
                      "instead \u2014 this button takes a single exported signal.", { what: what }), "err");
        return;
      }
      var sig = sigs[0];
      if (!isArr(sig.durations_us) || !sig.durations_us.length) {
        setMsg(msg, t("That signal carries no waveform, so there is nothing to import."), "err"); return;
      }
      pending = sig;
      info.classList.remove("hidden");
      clear(info);
      add(info, el("div", null, t("\u201c{name}\u201d \u2014 {pulses} pulses, origin {origin}",
                                  { name: sig.name || t("unnamed"),
                                    pulses: sig.durations_us.length,
                                    origin: sig.origin || "captured" })));
      if (b.device && b.device.hostname)
        add(info, el("div", "hint", t("Exported from {host}", { host: b.device.hostname })));
      if (!trimOf(nameIn)) nameIn.value = sig.name || "";
      go.disabled = false;
      setMsg(msg, "");
    };
    rd.onerror = function () { setMsg(msg, t("Could not read that file."), "err"); };
    rd.readAsText(f);
  });

  go.addEventListener("click", function () {
    if (!pending) return;
    go.disabled = true;
    setMsg(msg, t("Importing\u2026"));
    postJSON("/api/signals/import", {
      name: trimOf(nameIn) || pending.name || t("Imported signal"),
      first_level: numOr(pending.first_level, 0),
      durations_us: pending.durations_us,
      origin: "imported"
    }).then(function (created) {
      loadSignals();
      sh.close();
      alertSheet(t("Signal imported"),
        t("\u201c{name}\u201d is stored as id {id}. {decoded}Wire it up from the Dashboard, or open it here to test it.",
          { name: created.name || "", id: created.id,
            decoded: (created.decoded ? t("It decodes as {text}. ", { text: created.decoded.text })
                                      : t("No decoder recognised it, which is fine \u2014 it can still be transmitted. ")) }));
    }).catch(function (e) {
      go.disabled = false;
      setMsg(msg, e.message, "err");
    });
  });
}

var BACKUP_KIND = "klingelbox-backup";
var BACKUP_VERSION = 1;

/* The firmware's ceilings, from signal_store.h and node_graph.h. Duplicated
   here for one reason only: a Replace has to be REFUSABLE BEFORE it destroys
   anything. The single worst outcome this feature can produce is a wiped box
   plus an import that then does not fit. */
var LIM_SIGNALS = 32;
var LIM_NODES = 24;
var LIM_LINKS = 48;

function isArr(v) { return Object.prototype.toString.call(v) === "[object Array]"; }

/* Run `step` over `list` strictly one at a time, resolving when the last has
   settled. Sequential and NOT Promise.all(): the box serves this from a handful
   of httpd workers, each of which mallocs a 1 KB frame buffer for a waveform,
   and firing 32 of those at once is how you turn a working feature into an
   out-of-memory report. Latency is not the constraint here; heap is. */
function serialEach(list, step) {
  var i = 0;
  function next() {
    if (i >= list.length) return Promise.resolve();
    var item = list[i];
    i++;
    return Promise.resolve(step(item)).then(next);
  }
  return next();
}

/* klingelbox-<hostname>-<yyyymmdd>.json -- the two things you need to tell two
   backup files apart in a downloads folder six months from now. */
function backupFilename(host) {
  function p2(n) { return (n < 10 ? "0" : "") + n; }
  var d = new Date();
  var h = String(host || "klingelbox").toLowerCase().replace(/[^a-z0-9-]+/g, "-");
  return "klingelbox-" + (h || "klingelbox") + "-" +
         d.getFullYear() + p2(d.getMonth() + 1) + p2(d.getDate()) + ".json";
}

/* Assemble the bundle client-side. `onStep(done, total, text)` drives the bar. */
function buildBundle(onStep) {
  var bundle = {
    kind: BACKUP_KIND,
    version: BACKUP_VERSION,
    /* The BROWSER's clock, deliberately. The box may have no time source at
       all, which is why signals carry created_at: 0 -- and a date you can read
       beats a zero that is technically the device's own opinion. */
    exported_at: Math.floor(Date.now() / 1000),
    device: {},
    signals: [],
    graph: { nodes: [], links: [] }
  };
  var total = 1;
  var done = 0;
  function tick(text) { done++; if (onStep) onStep(done, total, text); }

  return api("/api/system").then(function (sys) {
    /* Descriptive only, and deliberately just these three: nothing here names a
       network, and nothing here authenticates to anything. */
    bundle.device = {
      hostname: sys.hostname || "",
      version: sys.version || "",
      idf: sys.idf || ""
    };
    return api("/api/radio").catch(function () { return null; });
  }).then(function (r) {
    /* Radio settings are device BEHAVIOUR, not identity, so they travel in
       their own clearly separated object and are optional on import. rssi_dbm
       is left out: it is a live measurement of the room, not a setting. */
    if (r) {
      bundle.radio = {
        freq_hz: r.freq_hz, modulation: r.modulation, datarate_bps: r.datarate_bps,
        bandwidth_hz: r.bandwidth_hz, tx_power_dbm: r.tx_power_dbm,
        tx_repeats: r.tx_repeats, tx_gap_us: r.tx_gap_us
      };
    }
    return api("/api/signals");
  }).then(function (res) {
    var list = res.signals || [];
    total = list.length + 1;
    return serialEach(list, function (meta) {
      return api("/api/signals/" + meta.id).then(function (sig) {
        bundle.signals.push({
          id: sig.id,
          name: sig.name,
          origin: sig.origin,
          first_level: numOr(sig.first_level, 0),
          durations_us: sig.durations_us || []
        });
        tick(t("Read “{name}”…", { name: signalLabel(meta) }));
      });
    });
  }).then(function () {
    return api("/api/graph");
  }).then(function (g) {
    /* Nodes travel VERBATIM. A node object is routing configuration end to end
       -- there is no credential-shaped field anywhere in it (see node_json() in
       http_api.c) -- so copying it whole is both safe and future-proof: a node
       field added to the firmware later is backed up without touching this. */
    bundle.graph.nodes = (g.nodes || []).slice();
    bundle.graph.links = (g.links || []).map(function (l) {
      return { from: l.from, to: l.to };
    });
    tick(t("Read the node graph."));
    return bundle;
  });
}

function downloadBundle(bundle, name) {
  var text = JSON.stringify(bundle);
  var url = URL.createObjectURL(new Blob([text], { type: "application/json" }));
  var a = document.createElement("a");
  a.href = url;
  a.download = name;
  /* This anchor exists for exactly one click and must never join the layout. */
  a.style.display = "none";
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  /* Safari needs the object URL to outlive the click by more than a tick. */
  setTimeout(function () { URL.revokeObjectURL(url); }, 10000);
  return text.length;
}

/* Refuse an unknown kind or a newer version with a sentence, rather than
   importing nonsense approximately. Returns null when the bundle is readable. */
function validateBundle(b) {
  if (!b || typeof b !== "object" || isArr(b))
    return t("That file is not a Klingelbox backup — it is not even a JSON object.");
  if (b.kind !== BACKUP_KIND)
    return t("That is not a Klingelbox backup. Its “kind” is {kind}, and a backup says “{expected}”.",
             { kind: (b.kind ? "“" + String(b.kind) + "”" : t("missing")),
               expected: BACKUP_KIND });
  var v = numOr(b.version, 0);
  if (v < 1) return t("That backup has no usable version number, so it cannot be read.");
  if (v > BACKUP_VERSION)
    return t("That backup is version {version} and this box understands version {supported}. " +
             "Update the firmware first — importing it half-understood would be worse than " +
             "not importing it at all.", { version: v, supported: BACKUP_VERSION });
  if (!isArr(b.signals) || !b.graph || !isArr(b.graph.nodes) || !isArr(b.graph.links))
    return t("That backup is missing its signals or its graph, so it cannot be read.");
  return null;
}

/*
 * THE ARITHMETIC THAT RUNS BEFORE ANYTHING IS DESTROYED.
 *
 * Returns { rows, error }. `rows` is [what, have, incoming, limit] for the
 * summary; `error` is a sentence when the import cannot fit, in which case the
 * caller must refuse — for Replace especially, because by the time a POST is
 * rejected the old configuration is already gone.
 */
function capacityCheck(b, mode) {
  var wipe = (mode === "replace");
  var rows = [
    ["Signals", wipe ? 0 : (S.signals || []).length, b.signals.length, LIM_SIGNALS],
    ["Nodes", wipe ? 0 : ((S.graph && S.graph.nodes) || []).length, b.graph.nodes.length, LIM_NODES],
    ["Links", wipe ? 0 : ((S.graph && S.graph.links) || []).length, b.graph.links.length, LIM_LINKS]
  ];
  var bad = rows.filter(function (r) { return r[1] + r[2] > r[3]; });
  if (!bad.length) return { rows: rows, error: null };

  var parts = bad.map(function (r) {
    return t("{count} {what} (the limit is {limit})",
             { count: r[1] + r[2], what: t(r[0].toLowerCase()), limit: r[3] });
  });
  if (wipe) {
    return { rows: rows, error:
      t("Even on an empty box this backup does not fit: it needs {needs}. Nothing has been " +
        "changed. Trim the backup on the box it came from and export it again.",
        { needs: parts.join(t(", and ")) }) };
  }
  return { rows: rows, error:
    t("Merging would need {needs}. Nothing has been changed. Delete what you no longer need " +
      "first, or use Replace, which clears this box before importing.",
      { needs: parts.join(t(", and ")) }) };
}

/*
 * Replay the bundle. Not atomic — see the section header — so every outcome is
 * recorded and reported rather than assumed.
 */
function runImport(bundle, mode, withRadio, onStep, onLog) {
  var rep = { signals: 0, nodes: 0, links: 0, unbound: 0, radio: false,
              clearedNodes: 0, clearedSignals: 0, skipped: [] };
  var sigMap = {};    /* old signal id -> new signal id */
  var nodeMap = {};   /* old node id   -> new node id   */
  var storeFull = false;

  var oldNodes = (mode === "replace") ? ((S.graph && S.graph.nodes) || []).slice() : [];
  var oldSigs = (mode === "replace") ? (S.signals || []).slice() : [];
  var total = oldNodes.length + oldSigs.length + bundle.signals.length +
              bundle.graph.nodes.length + bundle.graph.links.length + (withRadio ? 1 : 0);
  var done = 0;
  function tick(text) { done++; if (onStep) onStep(done, total, text); }
  function skip(text) { rep.skipped.push(text); if (onLog) onLog(text, "bad"); }
  function note(text) { if (onLog) onLog(text, "ok"); }

  /* 1. Replace only: clear. Nodes before signals, because deleting a node also
     drops its links and leaves nothing dangling in between. */
  function clearPhase() {
    if (mode !== "replace") return Promise.resolve();
    return serialEach(oldNodes, function (n) {
      return api("/api/graph/nodes/" + n.id, { method: "DELETE" }).then(function () {
        rep.clearedNodes++;
      }).catch(function (e) {
        skip(t("Could not delete node “{name}”: {error}", { name: n.name || n.id, error: e.message }));
      }).then(function () { tick(t("Clearing the graph…")); });
    }).then(function () {
      return serialEach(oldSigs, function (s) {
        return api("/api/signals/" + s.id, { method: "DELETE" }).then(function () {
          rep.clearedSignals++;
        }).catch(function (e) {
          skip(t("Could not delete signal “{name}”: {error}", { name: signalLabel(s), error: e.message }));
        }).then(function () { tick(t("Clearing the signal store…")); });
      });
    });
  }

  /* 2. Signals, recording old id -> new id as the DEVICE assigns them. */
  function signalsPhase() {
    return serialEach(bundle.signals, function (s) {
      var label = s.name || t("Signal {id}", { id: s.id });
      if (storeFull) {
        skip(t("“{label}” — no room left in the signal store.", { label: label }));
        tick("");
        return;
      }
      var pulses = isArr(s.durations_us) ? s.durations_us : [];
      if (!pulses.length) {
        skip(t("“{label}” — the backup carries no waveform for it.", { label: label }));
        tick("");
        return;
      }
      return postJSON("/api/signals/import", {
        name: label,
        first_level: numOr(s.first_level, 0),
        durations_us: pulses,
        origin: s.origin || "imported"
      }).then(function (created) {
        if (s.id) sigMap[s.id] = created.id;
        rep.signals++;
        note(t("Signal “{label}” → #{id}", { label: label, id: created.id }));
      }).catch(function (e) {
        /* 507 is "nothing more will ever fit", not "this one was wrong", so the
           rest are skipped with an honest reason instead of 30 identical
           failures scrolling past. */
        if (e.status === 507) storeFull = true;
        skip(t("“{label}” — {error}", { label: label, error: e.message }));
      }).then(function () { tick(t("Importing signals…")); });
    });
  }

  /* 3. Nodes, with signal_id rewritten through sigMap. */
  function nodesPhase() {
    return serialEach(bundle.graph.nodes, function (n) {
      var body = {};
      Object.keys(n).forEach(function (k) { if (k !== "id") body[k] = n[k]; });
      var wanted = numOr(n.signal_id, 0);
      var unbound = false;
      if (wanted) {
        if (sigMap[wanted]) {
          body.signal_id = sigMap[wanted];
        } else {
          /* NEVER carry a foreign signal_id through unchanged. On this box that
             number is a DIFFERENT doorbell, or nothing at all, and a node
             quietly bound to the wrong bell is the worst bug this feature could
             ship. The node is created UNBOUND and SWITCHED OFF rather than
             dropped: the wiring is the part that is laborious to rebuild by
             hand, and a node you can see and re-point beats a link that
             silently vanished. It is reported either way. */
          body.signal_id = 0;
          body.enabled = false;
          unbound = true;
        }
      }
      return postJSON("/api/graph/nodes", body).then(function (created) {
        if (n.id) nodeMap[n.id] = created.id;
        rep.nodes++;
        if (unbound) {
          rep.unbound++;
          skip(t("Node “{name}” was created unbound and switched off — its signal did not " +
                 "import. Pick a signal for it, then enable it.", { name: n.name || created.id }));
        } else {
          note(t("Node “{name}” → #{id}", { name: n.name || created.id, id: created.id }));
        }
      }).catch(function (e) {
        skip(t("Node “{name}” — {error}", { name: n.name || n.id, error: e.message }));
      }).then(function () { tick(t("Importing nodes…")); });
    });
  }

  /* 4. Links, with BOTH endpoints rewritten. */
  function linksPhase() {
    return serialEach(bundle.graph.links, function (l) {
      var from = nodeMap[numOr(l.from, 0)];
      var to = nodeMap[numOr(l.to, 0)];
      if (!from || !to) {
        skip(t("A link ({from} → {to}) was dropped: {which} could be created.",
               { from: l.from, to: l.to,
                 which: (!from && !to ? t("neither of its nodes")
                                      : (!from ? t("the node it starts at") : t("the node it ends at"))) }));
        tick("");
        return;
      }
      return postJSON("/api/graph/links", { from: from, to: to }).then(function () {
        rep.links++;
      }).catch(function (e) {
        skip(t("A link ({from} → {to}) — {error}", { from: l.from, to: l.to, error: e.message }));
      }).then(function () { tick(t("Importing links…")); });
    });
  }

  function radioPhase() {
    if (!withRadio || !bundle.radio) return Promise.resolve();
    return postJSON("/api/radio", bundle.radio).then(function () {
      rep.radio = true;
    }).catch(function (e) {
      skip(t("Radio settings — {error}", { error: e.message }));
    }).then(function () { tick(t("Applying radio settings…")); });
  }

  return clearPhase().then(signalsPhase).then(nodesPhase)
    .then(linksPhase).then(radioPhase).then(function () { return rep; });
}

function sectionBackup() {
  var s = section(t("Backup"),
    t("Save this box's signals and automations to a file, and restore them here or on another Klingelbox."));
  var body = s.bodyEl;

  add(body, el("div", "note",
    t("A backup holds the learned waveforms and the whole node graph. It deliberately holds " +
      "NO passwords: not the Wi-Fi passphrase, not the hotspot password, not the MQTT " +
      "credentials, not even their hashes. Set those up again on the new box.")));

  /* ---- export ---- */
  add(body, el("h3", null, t("Export")));
  var exMsg = el("div", "formmsg");
  var exProg = el("div", "progress hidden");
  var exBar = el("i");
  add(exProg, exBar);
  var exBtn = el("button", "btn primary", t("Export backup file"));
  exBtn.type = "button";
  exBtn.addEventListener("click", function () {
    exBtn.disabled = true;
    exProg.classList.remove("hidden");
    exBar.style.width = "0%";
    setMsg(exMsg, t("Reading the box…"));
    buildBundle(function (done, total, text) {
      exBar.style.width = Math.round(done * 100 / Math.max(1, total)) + "%";
      if (text) setMsg(exMsg, text);
    }).then(function (bundle) {
      var name = backupFilename(bundle.device && bundle.device.hostname);
      var bytes = downloadBundle(bundle, name);
      exBar.style.width = "100%";
      setMsg(exMsg, t("Saved {file} — {signals} signals, {nodes} nodes, {links} links, {kb} KB.",
                      { file: name, signals: bundle.signals.length,
                        nodes: bundle.graph.nodes.length, links: bundle.graph.links.length,
                        kb: Math.max(1, Math.round(bytes / 1024)) }), "ok");
    }).catch(function (e) {
      exProg.classList.add("hidden");
      setMsg(exMsg, t("Export failed: {error}", { error: e.message }), "err");
    }).then(function () { exBtn.disabled = false; });
  });
  var exFoot = el("div", "formfoot");
  add(exFoot, exBtn);
  add(body, exFoot, exProg, exMsg);

  /* ---- import ---- */
  add(body, el("div", "divider"));
  add(body, el("h3", null, t("Restore")));

  var file = el("input");
  file.type = "file";
  file.accept = ".json,application/json";
  file.style.fontSize = "1rem";
  file.style.padding = ".55rem 0";
  add(body, field(t("Backup file"), file, t("Nothing is written to the box until you choose Merge or Replace.")));

  var radioOpt = checkField(t("Also restore the radio settings"), false,
    t("Frequency, bandwidth, TX power and repeats. Off by default: those are this box's " +
      "behaviour, and the box you are restoring to may have a different antenna."));
  var preview = el("div");
  var log = el("div", "note hidden");
  /* One-off styles rather than a stylesheet rule, as elsewhere in this file:
     the running list is the only scrolling box in the UI, and a hundred lines
     of it must not push the summary off a phone screen. */
  log.style.maxHeight = "14rem";
  log.style.overflowY = "auto";
  var imProg = el("div", "progress hidden");
  var imBar = el("i");
  add(imProg, imBar);
  var imMsg = el("div", "formmsg");
  var loaded = null;   /* the validated bundle, or null */

  function logLine(text, kind) {
    log.classList.remove("hidden");
    add(log, el("div", "small" + (kind === "bad" ? " bad-text" : " muted"), text));
    log.scrollTop = log.scrollHeight;
  }

  function resetPreview() {
    loaded = null;
    clear(preview);
    clear(log);
    log.classList.add("hidden");
    imProg.classList.add("hidden");
    setMsg(imMsg, "");
  }

  function finish(rep, mode) {
    /* The truthful summary. A partial import that claims success is worse than
       a visible failure, so the counts come from what the device actually
       acknowledged and the skips are listed in full above. */
    var parts = [t("{n} signals", { n: rep.signals }), t("{n} nodes", { n: rep.nodes }),
                 t("{n} links", { n: rep.links })];
    if (rep.radio) parts.push(t("radio settings"));
    var line = (mode === "replace" ? t("Replaced. ") : t("Merged. ")) +
               t("Imported {items}.", { items: parts.join(", ") });
    if (rep.unbound) line += " " + t("{n} node(s) came in unbound and switched off.", { n: rep.unbound });
    if (rep.skipped.length) {
      line += " " + t("{n} item(s) were skipped — see the list above.", { n: rep.skipped.length });
      setMsg(imMsg, line, "warn");
    } else {
      setMsg(imMsg, line, "ok");
    }
    /* Drop the loaded bundle and the file input. The buttons are re-enabled by
       the caller, and leaving a one-tap "do it again" under a summary that says
       it already happened is how a merge becomes a double merge. */
    clear(preview);
    loaded = null;
    try { file.value = ""; } catch (e) { /* older browsers refuse this */ }
    return Promise.all([loadSignals(), loadGraph()]);
  }

  function start(mode) {
    var bundle = loaded;
    if (!bundle) return;
    /* Re-read the box first: Settings is built once, the user may have edited
       the graph on the Dashboard since, and the capacity arithmetic below is
       only worth anything if it is arithmetic about the CURRENT box. */
    setMsg(imMsg, t("Checking what will fit…"));
    Promise.all([loadSignals(), loadGraph()]).then(function () {
      var cap = capacityCheck(bundle, mode);
      if (cap.error) { setMsg(imMsg, cap.error, "err"); return; }

      var lines;
      if (mode === "replace") {
        lines = [
          t("This DELETES all {signals} stored signals and all {nodes} nodes with their " +
            "{links} links on this box, and cannot be undone.",
            { signals: (S.signals || []).length,
              nodes: ((S.graph && S.graph.nodes) || []).length,
              links: ((S.graph && S.graph.links) || []).length }),
          t("Then it imports {signals} signals, {nodes} nodes and {links} links.",
            { signals: bundle.signals.length, nodes: bundle.graph.nodes.length,
              links: bundle.graph.links.length }),
          t("Wi-Fi, hotspot and MQTT settings are not touched."),
          t("The import is not atomic: if it fails half way, what got in stays in.")
        ];
      } else {
        lines = [
          t("Nothing existing is deleted. {signals} signals, {nodes} nodes and {links} links " +
            "are ADDED, with new ids.",
            { signals: bundle.signals.length, nodes: bundle.graph.nodes.length,
              links: bundle.graph.links.length }),
          t("Signals identical to ones you already have will be duplicated — a merge " +
            "cannot tell a re-import from a second doorbell."),
          t("The import is not atomic: if it fails half way, what got in stays in.")
        ];
      }
      return confirmSheet(mode === "replace" ? t("Replace everything on this box?") : t("Merge this backup in?"),
                          lines, mode === "replace" ? t("Erase & import") : t("Merge"),
                          mode === "replace").then(function (ok) {
        if (!ok) { setMsg(imMsg, ""); return; }
        clear(log);
        log.classList.remove("hidden");
        imProg.classList.remove("hidden");
        imBar.style.width = "0%";
        setMsg(imMsg, t("Importing…"));
        $$("button", preview).forEach(function (b) { b.disabled = true; });
        file.disabled = true;
        return runImport(bundle, mode, !!radioOpt.input.checked, function (done, total, text) {
          imBar.style.width = Math.round(done * 100 / Math.max(1, total)) + "%";
          if (text) setMsg(imMsg, text + " " + done + "/" + total);
        }, logLine).then(function (rep) {
          imBar.style.width = "100%";
          return finish(rep, mode);
        });
      });
    }).catch(function (e) {
      setMsg(imMsg, t("Import failed: {error}", { error: e.message }), "err");
    }).then(function () {
      file.disabled = false;
      $$("button", preview).forEach(function (b) { b.disabled = false; });
    });
  }

  function showPreview(bundle) {
    clear(preview);
    var dev = bundle.device || {};
    var when = fmtEpoch(numOr(bundle.exported_at, 0));
    add(preview, el("div", "note ok",
      t("From “{host}”{firmware}{when}.", {
        host: dev.hostname || t("an unnamed box"),
        firmware: (dev.version ? t(", firmware {version}", { version: dev.version }) : ""),
        when: (when ? t(", exported {when}", { when: when }) : "")
      })));
    var dl = el("dl", "kv");
    [[t("Signals"), String(bundle.signals.length)],
     [t("Nodes"), String(bundle.graph.nodes.length)],
     [t("Links"), String(bundle.graph.links.length)],
     [t("Radio settings"), bundle.radio ? t("included (optional below)") : t("not in this file")]
    ].forEach(function (kv) {
      add(dl, el("dt", null, kv[0]), el("dd", null, kv[1]));
    });
    add(preview, dl);
    if (bundle.radio) add(preview, radioOpt);

    add(preview, el("p", "small muted",
      t("Signal and node numbers are re-assigned by this box, and the graph is rewritten to " +
        "match as it is imported. A node whose signal cannot be imported is created unbound " +
        "and switched off rather than pointed at whatever happens to have that number here.")));

    var row = el("div", "btnrow");
    var merge = el("button", "btn primary", t("Merge"));
    merge.type = "button";
    merge.addEventListener("click", function () { start("merge"); });
    var repl = el("button", "btn danger", t("Replace everything"));
    repl.type = "button";
    repl.addEventListener("click", function () { start("replace"); });
    add(row, merge, repl);
    add(preview, row);
    add(preview, el("div", "hint",
      t("Merge adds to what is here. Replace erases this box's signals and graph first — " +
        "capacity is checked before anything is deleted.")));
  }

  file.addEventListener("change", function () {
    resetPreview();
    var f = file.files && file.files[0];
    if (!f) return;
    if (f.size > 4 * 1024 * 1024) {
      setMsg(imMsg, t("That file is {mb} MB. A full backup is well under one — that is not a " +
                      "Klingelbox backup.", { mb: Math.round(f.size / 1048576) }), "err");
      return;
    }
    setMsg(imMsg, t("Reading {file}…", { file: f.name }));
    var fr = new FileReader();
    fr.onerror = function () { setMsg(imMsg, t("Could not read that file."), "err"); };
    fr.onload = function () {
      var b;
      try { b = JSON.parse(fr.result); }
      catch (e) { setMsg(imMsg, t("That file is not valid JSON, so it cannot be a backup."), "err"); return; }
      var bad = validateBundle(b);
      if (bad) { setMsg(imMsg, bad, "err"); return; }
      loaded = b;
      setMsg(imMsg, "");
      showPreview(b);
    };
    fr.readAsText(f);
  });

  /* The running list sits ABOVE the summary, so "see the list above" is true
     and the last thing on the page is the verdict. */
  add(body, preview, imProg, log, imMsg);
  return s;
}

function sectionReboot() {
  var s = section(t("Reboot"), t("Restart the box. Signals, automations and settings all survive."));
  var msg = el("div", "formmsg");
  var b = el("button", "btn danger", t("Reboot now"));
  b.type = "button";
  b.addEventListener("click", function () {
    confirmSheet(t("Reboot the box?"),
      [t("It is back in a few seconds. Nothing stored is lost."),
       t("A doorbell press during the reboot is missed.")], t("Reboot"), true).then(function (ok) {
      if (!ok) return;
      b.disabled = true;
      setMsg(msg, t("Rebooting…"));
      postJSON("/api/restart", {}).catch(function () { /* the box may drop the socket first */ });
      setTimeout(function () {
        setMsg(msg, t("Reboot requested — reload this page in a few seconds."), "ok");
        b.disabled = false;
      }, 1500);
    });
  });
  var foot = el("div", "formfoot");
  add(foot, b, msg);
  add(s.bodyEl, foot);
  return s;
}

/* ======================================================================
   DIAGNOSTICS
   ====================================================================== */

/* Severity and a short "so what do I do" line per state. `help` comes from the
   firmware and is authoritative; this only adds the next action. */
var DIAG_META = {
  CC1101_NOT_DETECTED: { sev: "bad", act: "Check 3V3 and GND on the module, then the four SPI wires (CSN, SCK, MOSI, MISO). A reading of 0x00 or 0xFF means the bus is floating or the module is unpowered." },
  SPI_ERROR: { sev: "bad", act: "The SPI transaction itself failed, which points at the bus rather than the module: a shorted pin, or another driver holding the peripheral." },
  CC1101_OK: { sev: "ok", act: "The radio answered with a plausible chip id. Anything still failing is downstream of the module." },
  RADIO_CONFIG_SUSPECT: { sev: "warn", act: "The chip is configured but the band looks wrong -- no carrier ever seen and an implausible noise floor. Re-check the frequency under Settings, and that the antenna is fitted." },
  RF_ENERGY_NO_PULSES: { sev: "warn", act: "Energy is arriving but nothing survives the filter. Usually a mis-tuned frequency or bandwidth, or a transmitter using a modulation this box is not set to." },
  PULSES_CAPTURED: { sev: "ok", act: "Raw frames are being captured. If presses still do nothing, the problem is matching or routing, not reception." },
  REPEAT_FRAME_DETECTED: { sev: "ok", act: "Several identical copies arrived together, which is what a real remote does and noise does not." },
  PROTOCOL_DECODED: { sev: "ok", act: "A decoder recognised the frame and extracted its identity." },
  UNKNOWN_PROTOCOL_RAW: { sev: "warn", act: "Captured but not decoded. This is fully supported: the exact timings are stored and replay works. Only the human-readable identity is missing." },
  TX_OK: { sev: "ok", act: "The pulses left the radio. This is a software-level claim only -- the box cannot know whether any receiver reacted." },
  TX_FAILED: { sev: "bad", act: "The transmit path did not complete. Check that no other task is holding the radio, and look for an SPI error above." }
};
var CAPTURE_HELP = {
  frames: "Complete raw frames handed to the decoders since boot.",
  dropped_short: "Bursts thrown away for being too short to be a real frame. A steadily rising number on a quiet band is normal -- that is the noise filter working.",
  dropped_full: "Frames lost because the capture queue was full. Anything other than zero means the box could not keep up.",
  overruns: "Hardware capture overruns. Should stay at zero; a rising count points at interrupt starvation."
};

var diagEls = {};

function buildDiagnostics() {
  if (S.built.diagnostics) return;
  S.built.diagnostics = true;
  var root = clear($("#tab-diagnostics"));

  var p = el("div", "panel");
  var h = el("div", "panel-head");
  add(h, el("h2", null, t("Diagnostics")));
  add(h, el("p", null,
    t("This page exists because “it does not work” has at least five different causes on an " +
    "RF box: a dead SPI bus, a mis-tuned radio, a noisy band, an unrecognised protocol, or a " +
    "transmit that never keyed the carrier. Each one shows up differently below.")));
  add(p, h);
  diagEls.verdict = el("div");
  add(p, diagEls.verdict);
  add(root, p);

  var p2 = el("div", "panel");
  add(p2, el("h2", null, t("Capture counters")));
  diagEls.counters = el("div", "counters");
  add(p2, diagEls.counters);
  add(root, p2);

  /* LISTENING LIVES HERE TOO. The counters immediately above are the ones that
     say "energy arrived and none of it survived"; this is the tool that shows
     you WHAT arrived. Putting the two anywhere but next to each other would
     make the reader hunt for the follow-up to a number they are already
     staring at. */
  var pr = el("div", "panel");
  add(pr, el("h2", null, t("Listen for a button")));
  add(pr, el("p", "hint",
    t("Those counters can tell you that bursts were rejected, but not what they looked "
    + "like. A listening session records everything the radio hears for a fixed time, "
    + "with the minimum frame length, the frame boundary and the signal-strength squelch "
    + "all turned down — then ranks what it heard, most likely first, and lets you look at "
    + "any of it, trim it, send it back out and keep it as a signal. Nothing is admitted or "
    + "rejected on how it looks, nothing is written to flash, and your node graph is "
    + "untouched.")));
  diagEls.raw = el("div");
  add(pr, diagEls.raw);
  add(root, pr);
  renderRawPanel();

  var p3 = el("div", "panel");
  add(p3, el("h2", null, t("States")));
  add(p3, el("p", "hint",
    t("Every layer of the firmware reports into this one list, so the serial log, the API and " +
    "this page can never drift apart. A state that has never fired is greyed out.")));
  diagEls.states = el("div", "diag");
  add(p3, diagEls.states);
  add(root, p3);

  renderDiagnostics();
}

/* Rebuilt whenever the probe lands, so a firmware without /api/raw removes the
   button instead of offering one that 404s. */
function renderRawPanel() {
  if (!diagEls.raw) return;
  var wrap = clear(diagEls.raw);
  if (!S.has.raw) {
    add(wrap, el("div", "note", t("This firmware does not have listening sessions. Update "
      + "it under Settings → Firmware to get the feature.")));
    return;
  }
  var st = S.raw;
  if (st && st.running) {
    add(wrap, el("div", "note warn", t("A session is listening right now — {used} of "
      + "{total} slots used, {left} s left.", { used: numOr(st.count, 0),
      total: numOr(st.capacity, 32), left: numOr(st.remaining_s, 0) })));
  } else if (st && st.held) {
    var cands = ((st.candidates) || []).length;
    add(wrap, el("div", "note", t("A finished session is still in memory: {frames}{cands}"
      + " you can still open, trim and save.", {
      frames: numOr(st.count, 0) === 1
        ? t("{n} frame", { n: numOr(st.count, 0) })
        : t("{n} frames", { n: numOr(st.count, 0) }),
      cands: cands ? (cands === 1
        ? t(", {c} ranked candidate", { c: cands })
        : t(", {c} ranked candidates", { c: cands })) : ""
    })));
    var fr = st.fragmentation;
    if (fr && fr.detected) {
      add(wrap, el("div", "note warn", t("{n} transmission(s) in that session were cut "
        + "into pieces by the frame boundary.", { n: numOr(fr.runs, 0) })));
    }
  }
  var row = el("div", "row");
  add(row, listenButton(t("\u{1F3A7} Listen for a button"), function (sig) {
    if (sig) { loadSignals(); if (S.graph) loadGraph(); }
    renderRawPanel();
  }));
  add(wrap, row);
}

function renderDiagnostics(err) {
  if (!diagEls.states) return;
  var v = clear(diagEls.verdict);

  if (err || !S.diag) {
    add(v, el("div", "note " + (S.has.diagnostics ? "warn" : "bad"),
      S.has.diagnostics
        ? t("Diagnostics are momentarily unavailable{detail}.", { detail: err ? ": " + err.message : "" })
        : t("This firmware does not expose /api/diagnostics.")));
    return;
  }

  var states = S.diag.states || [];
  var byName = {};
  states.forEach(function (s) { byName[s.name] = s; });
  function fired(n) { return byName[n] && numOr(byName[n].count, 0) > 0; }

  var verdict, kind;
  if (fired("CC1101_NOT_DETECTED") || fired("SPI_ERROR")) {
    kind = "bad";
    verdict = t("The radio module is not answering. Nothing can be received or transmitted until that is fixed — start with the wiring.");
  } else if (!fired("CC1101_OK")) {
    kind = "warn";
    verdict = t("The radio has not reported a successful probe yet. If the box only just booted, give it a moment.");
  } else if (!fired("PULSES_CAPTURED")) {
    kind = "warn";
    verdict = t("The radio is alive but has not captured a single frame yet. Press a 433 MHz remote within a few metres and watch this page.");
  } else if (fired("PROTOCOL_DECODED")) {
    kind = "ok";
    verdict = t("Receiving and decoding normally.");
  } else {
    kind = "ok";
    verdict = t("Receiving raw frames. No decoder has claimed one yet, which is fine — undecoded signals are still stored and replayable.");
  }
  add(v, el("div", "note " + kind, verdict));

  /* counters */
  var c = clear(diagEls.counters);
  var cap = S.diag.capture || {};
  ["frames", "dropped_short", "dropped_full", "overruns"].forEach(function (k) {
    var box = el("div", "counter");
    add(box, el("div", "c-num", String(numOr(cap[k], 0))));
    add(box, el("div", "c-lbl", k.replace(/_/g, " ")));
    add(box, el("div", "c-help", CAPTURE_HELP[k] ? t(CAPTURE_HELP[k]) : ""));
    add(c, box);
  });

  /* states */
  var list = clear(diagEls.states);
  var up = uptimeNow();
  states.forEach(function (st) {
    var meta = DIAG_META[st.name] || { sev: "ok", act: "" };
    var count = numOr(st.count, 0);
    var sev = count > 0 ? meta.sev : "idle";
    var item = el("div", "diagitem s-" + sev);
    var head = el("div", "d-head");
    add(head, el("span", "d-name", st.name));
    add(head, el("span", "chip" + (count ? (sev === "ok" ? " ok" : sev === "bad" ? " bad" : " warn") : ""),
      count === 0 ? t("never") : count + "x"));
    if (count > 0 && typeof st.last_us === "number" && st.last_us > 0 && up !== null) {
      add(head, el("span", "chip mono", t("{d} ago", { d: shortDur(up - st.last_us / 1e6) })));
    }
    add(item, head);
    add(item, el("div", "d-help", st.help || ""));
    if (count > 0 && meta.act) add(item, el("div", "d-help muted", t(meta.act)));
    if (st.detail) add(item, el("div", "d-detail", st.detail));
    add(list, item);
  });
  if (!states.length) add(list, el("div", "empty", t("The firmware reported no diagnostic states.")));
}

/* ======================================================================
   HANDBOOK -- the manual, shipped inside the box

   This tab exists because of how the box is reached. A fresh one hands you its
   own access point and a captive portal, and a phone joined to that AP has NO
   internet: docs/ on the web is unreachable at exactly the moment someone is
   trying to work out what a Group node does. So the manual is flashed with the
   UI, and it is written to be read on a 360 px screen in a hallway.

   EVERY SECTION IS A CLOSED <details>. Seven sections of prose as one page is
   a wall nobody reads; closed, the page opens as a seven-line table of
   contents that fits on one screen, and the summaries ARE the navigation --
   which is why they are full-height tap targets with an open/closed chevron,
   the same .sect-summary Settings uses.

   Which sections were open is remembered, so coming back from Diagnostics to
   check one more line does not re-collapse what you were reading.

   The node reference is GENERATED FROM NODE_TYPES. Anything else drifts: a node
   added to the palette and forgotten here would be a manual that lies. The
   per-type detail below is a lookup keyed by the same type string, and a type
   with no entry still gets its label, icon, ports and help -- so a new node is
   under-documented rather than invisible.
   ====================================================================== */

var HB_OPEN_KEY = "klingelbox-handbook-open";

function hbOpenList() {
  try {
    var v = JSON.parse(localStorage.getItem(HB_OPEN_KEY) || "[]");
    return (Object.prototype.toString.call(v) === "[object Array]") ? v : [];
  } catch (e) { return []; }   /* private mode, or a key someone hand-edited */
}
function hbRemember(key, open) {
  var list = hbOpenList().filter(function (k) { return k !== key; });
  if (open) list.push(key);
  try { localStorage.setItem(HB_OPEN_KEY, JSON.stringify(list)); } catch (e) { /* ignore */ }
}

/* A top-level handbook section: closed unless it was open last time. */
function hbSection(key, title, sub) {
  var d = el("details", "panel hb");
  if (hbOpenList().indexOf(key) !== -1) d.open = true;
  add(d, el("summary", "sect-summary", title));
  d.addEventListener("toggle", function () { hbRemember(key, d.open); });
  if (sub) add(d, el("p", "hint", sub));
  var body = el("div", "hb-body");
  add(d, body);
  d.bodyEl = body;
  return d;
}

function hbP(text) { return el("p", "hb-p", text); }
function hbH(text) { return el("h3", "hb-h", text); }
function hbNote(text, cls) { return el("div", "note" + (cls ? " " + cls : ""), text); }

/* A run of short paragraphs, which is what most of this page is. */
function hbPs(parent, list) {
  list.forEach(function (p) { add(parent, hbP(p)); });
  return parent;
}

function hbList(items, ordered) {
  var l = el(ordered ? "ol" : "ul", "hb-list");
  items.forEach(function (item) { add(l, el("li", null, item)); });
  return l;
}

/* term / value rows. Used for ports, settings and the topic map, so all three
   line up the same way down the page. */
function hbKV(pairs) {
  var dl = el("dl", "kv");
  pairs.forEach(function (kv) {
    add(dl, el("dt", null, kv[0]));
    add(dl, el("dd", kv[2] ? "mono" : null, kv[1]));
  });
  return dl;
}

/* A literal topic, command or code, on its own line and horizontally
   scrollable rather than wrapped: a broken topic string is worse than a
   scrollbar, because half of one looks like a whole one. */
function hbCode(text) {
  var w = el("div", "hb-code");
  add(w, el("code", null, text));
  return w;
}

/* ---------------------------------------------------- the node reference --

   Keyed by the SAME type string as NODE_TYPES. `ports` is derived, not stored:
   the group already decides it and two sources of truth would eventually
   disagree. Everything here is from the node editors, docs/automations.md and
   docs/API.md -- no invented defaults. */
var NODE_DOC = {
  "signal.rx": {
    what: "One stored 433 MHz code, listening. When the receiver hears that code on air, this " +
          "node fires and everything wired after it runs.",
    settings: [
      ["Signal", "Which stored code it watches for. Learn one from a remote, or enter one by hand."]
    ],
    notes: ["Output only. It never transmits — not when a burst matches it, not when you fire it " +
            "by hand from its card. Its ▶ button means “pretend the code just arrived”."]
  },
  "signal.tx": {
    what: "One stored 433 MHz code, sending. Whenever something linked into it fires, the code " +
          "goes out over the radio.",
    settings: [
      ["Signal", "Which stored code it transmits."],
      ["Repeats", "How many copies go out per firing. Defaults to the radio policy: 6."],
      ["Gap", "Microseconds between copies. Defaults to 8000 µs."]
    ],
    notes: ["Repeats are not cosmetic. A real remote transmits for as long as the button is held, " +
            "and real receivers integrate several consistent copies before they act — a single " +
            "replay is routinely ignored.",
            "Input only. It never listens, so hearing its own code on air does not start it. That " +
            "is what stops the box echoing back everything it hears."]
  },
  "source.gpio": {
    what: "Fires when a button wired to one of the board's pins is pressed. Optional — a box with " +
          "no wired button never needs one.",
    settings: [
      ["Pin", "Chosen here, not compiled in. The picker lists what is free; the six pins the radio " +
              "uses are not offered."],
      ["Active low", "On by default: the internal pull-up is enabled and the button pulls the pin " +
                     "to GND, so an unconnected pin reads as “not pressed” rather than floating."],
      ["Debounce", "50 ms by default. Without it one press fires the chain several times."]
    ],
    notes: ["If this box's firmware has no wired-button support the node type does not appear at " +
            "all — the palette is built from what the box answers, not from this manual."]
  },
  "source.virtual": {
    what: "A button Home Assistant can press. It appears in your dashboard by itself \u2014 no YAML, " +
          "no automation \u2014 and pressing it starts whatever is wired after it. The same button is " +
          "reachable from any MQTT client, from the \u25b6 here, and from the REST API.",
    settings: [
      ["Topic", "A suffix. The box subscribes to <base>/trigger/<suffix>; ANY message on it fires " +
                "the node, whatever the payload says. Left empty it does NOT mean \u201cno MQTT\u201d \u2014 it " +
                "follows a slug of the node's name, so a button called \u201cRing the chime\u201d answers on " +
                "ring_the_chime either way."],
      ["Expose to MQTT", "On by default. Clear it and this node disappears from MQTT entirely \u2014 nothing subscribed, nothing published, and its Home Assistant entity removed rather than left behind unavailable. It keeps working inside the graph."]
    ],
    notes: ["Four things fire it: a Home Assistant button entity, any MQTT client publishing to its " +
            "topic, the \u25b6 on its card or on the map, and POST /api/graph/nodes/{id}/fire. The payload " +
            "is never inspected \u2014 arriving IS the press.",
            "It is called source.virtual on the wire and in the API, which is what it was named " +
            "before anybody noticed that nobody could find it under that name.",
            "Two of these on the same topic share ONE Home Assistant button, and pressing it fires " +
            "both chains \u2014 which is how you get a single \u201cring everything\u201d button with no special " +
            "node type."]
  },
  "source.any_rf": {
    what: "A wildcard. It fires on every burst the receiver hears, registered or not — including " +
          "the neighbour's car remote.",
    settings: [["—", "No parameters."]],
    notes: ["It fires IN ADDITION to any matching Signal receiver. A recognised burst legitimately " +
            "drives both the specific chain and the wildcard chain in one pass. That is intended — " +
            "but wire both to the same chime and it rings twice.",
            "On a busy band, put a Rate limit between it and whatever it drives."]
  },
  "logic.group": {
    what: "Two genuinely different jobs under one node, chosen by its mode.",
    settings: [
      ["Mode — ANY", "A merge point. Anything arriving is passed straight on, immediately. It does " +
                     "not wait, does not compare its inputs, and never looks at the window at all."],
      ["Mode — ALL", "A coincidence detector. Nothing passes until every inbound link has carried " +
                     "an event inside the window. Then it fires once, forgets them all and re-arms."],
      ["Window", "Seconds. Applies to ALL only — the editor hides it in ANY mode for that reason."]
    ],
    notes: ["An ALL group with no inbound links can never be satisfied, so it never fires.",
            "ANY is the one to reach for far more often: it is how several remotes end up ringing " +
            "one chime with no special node type."]
  },
  "logic.throttle": {
    what: "A cooldown. The first event passes through immediately; everything inside the window " +
          "after it is dropped.",
    settings: [["Window", "Seconds of cooldown after each event that got through."]],
    notes: ["Leading edge, so the bell rings at once and then stays quiet — someone leaning on the " +
            "button gets one ring, not silence followed by a ring.",
            "Source-agnostic: an RF remote, a wired button and an MQTT trigger are all limited " +
            "identically."]
  },
  "logic.repeat": {
    what: "One event in, several out. It passes the event on immediately, then emits it again a " +
          "few more times at an interval.",
    settings: [
      ["Repeats", "Total firings, including the immediate one. 1–20, default 3."],
      ["Interval", "Seconds between them. Default 5."]
    ],
    notes: ["A new event restarts the run rather than queueing a second one, so pressing again " +
            "mid-sequence starts the count over."]
  },
  "logic.switch": {
    what: "A switch in the wire. While it is ON events pass through untouched; while it is OFF " +
          "nothing gets past it and everything wired after it is dead until it is switched back.",
    settings: [
      ["Position", "ON or OFF. This IS the node's enabled flag — there is no second field, which " +
                   "is why a switched-off Switch draws its wire broken on the map."],
      ["React to a signal", "Optional, none by default. Name a stored signal and every press of " +
                "that button flips this switch — on, off, on again. It is a CONTROL input, not a " +
                "wire: nothing is transmitted and nothing travels down the switch's own output, " +
                "only the position changes."],
      ["Topic", "A suffix. With one set, Home Assistant gets a real switch entity for it. Left " +
                "empty it does NOT mean “no MQTT” — it follows a slug of the node's name instead, " +
                "so a switch called “Outside bell” answers on outside_bell either way."],
      ["Expose to MQTT", "On by default. Clear it and this node disappears from MQTT entirely \u2014 nothing subscribed, nothing published, and its Home Assistant entity removed rather than left behind unavailable. It keeps working inside the graph."]
    ],
    notes: ["Four ways to move it: the ON/OFF button here, the REST API, MQTT, and — if you give " +
            "it a signal to react to — a 433 MHz remote.",
            "“React to a signal” is what makes a switch reachable from the doorstep: a fob in your " +
            "pocket becomes the on/off for a path, with no phone and no Home Assistant in it. It " +
            "toggles, so the press that silences the outside chime is the press that brings it " +
            "back next time.",
            "It is not the same as wiring a Signal receiver INTO the switch. A wire carries events " +
            "THROUGH the switch while it is on; this MOVES the switch. That is why it is a field " +
            "on the node and not a line on the map.",
            "One press is one toggle, however many times the remote repeats its code — the box " +
            "folds a press's repeats into a single burst before the graph ever sees it.",
            "The same signal may also drive a Signal receiver. Both then happen from one press: " +
            "the receiver runs its chain AND the switch flips. That is intended, and it looks " +
            "like double-firing in Activity when it is two nodes reacting to one code.",
            "A remote-driven move is published retained on MQTT straight away, so Home Assistant " +
            "follows the fob as fast as it follows its own toggle. The reaction itself works with " +
            "MQTT switched off entirely — it is between the radio and the graph.",
            "The topic it answers on is the one you typed, or a slug of its name if you did not " +
            "— “All Bells Switch” answers on all_bells_switch. The same answer decides the " +
            "subscription, the Home Assistant entity, the routing of a command and the reported " +
            "state, so a switch that appears in Home Assistant is a switch that can be commanded.",
            "Clearing “Expose to MQTT” is how you keep a switch off Home Assistant, because a " +
            "blank topic no longer does it. The retained entity and its retained position are " +
            "both cleared when you do, so nothing is left haunting the dashboard.",
            "Several Switch nodes may share one topic. A command then moves all of them, and Home " +
            "Assistant shows one entity per topic rather than one per node — one wall switch " +
            "feeding several lamps."]
  },
  "sink.mqtt": {
    what: "Publishes to your broker when the chain reaches it, so Home Assistant or anything else " +
          "on the LAN learns that something happened.",
    settings: [["Topic", "A suffix. Published to <base>/<suffix>."],
      ["Expose to MQTT", "On by default. Clear it and this node disappears from MQTT entirely \u2014 nothing subscribed, nothing published, and its Home Assistant entity removed rather than left behind unavailable. It keeps working inside the graph."]
    ],
    notes: ["With “Expose to MQTT” cleared this node publishes nothing at all — not on its own " +
            "topic and not into the <base>/event stream either. The chain still reaches it.",
            "The message carries the signal id, the label, the fingerprint, the RSSI, the repeat " +
            "count and the decode when there is one. For an unregistered burst arriving through " +
            "Any RF signal the signal id is 0 and the decode may be absent."]
  },
  "sink.monitor": {
    what: "The one node that does nothing. It exists to be looked at: a lamp that lights whenever " +
          "the chain reaches it, and a rolling timeline of when it did.",
    settings: [["Hold", "How long the lamp stays lit per hit. 1–60 seconds, default 3."]],
    notes: ["Nothing is sent, nothing is published, no pin moves. That is what makes it safe to " +
            "leave wired beside a real sink forever.",
            "Its hits live in RAM only — capped at 64 per node, pruned to the last 10 minutes, and " +
            "gone after a reboot. Nothing is written to flash."]
  }
};

var PORT_TEXT = {
  source: "Output only — it starts chains and has nothing to link into it.",
  logic:  "Input and output — it sits in the middle of a chain.",
  sink:   "Input only — it is the end of a chain and acts."
};

function hbNodeReference(body) {
  add(body, hbP(t(
    "Every node this box offers, generated from the palette itself so it cannot fall behind the " +
    "firmware. A node's group decides its ports, with no exceptions: sources start chains, sinks " +
    "end them, logic sits in between.")));

  NODE_TYPES.forEach(function (ty) {
    var doc = NODE_DOC[ty.t] || {};
    var d = el("details", "hb-node");
    var sum = el("summary", "hb-nodesum");
    add(sum, el("span", "hb-nodeico", ty.ico));
    var txt = el("span", "hb-nodetext");
    add(txt, el("span", "hb-nodename", t(ty.label)));
    add(txt, el("span", "hb-nodetype", ty.t));
    add(sum, txt);
    add(d, sum);

    add(d, hbP(t(doc.what || ty.help)));
    add(d, el("div", "hb-ports", t(PORT_TEXT[ty.g] || "")));
    if (doc.settings) {
      add(d, hbH(t("Settings")));
      add(d, hbKV(doc.settings.map(function (kv) { return [t(kv[0]), t(kv[1]), kv[2]]; })));
    }
    (doc.notes || []).forEach(function (n) { add(d, hbP(t(n))); });
    add(body, d);
  });

  add(body, hbNote(t(
    "Limits, so a graph does not fail in a way you have to guess at: 24 nodes, a chain at most 8 " +
    "nodes deep, node names up to 32 characters, MQTT topic suffixes up to 47, and any time " +
    "window between 1 and 6000 seconds.")));
}

/* ------------------------------------------------------------- recipes --
   Moved here from the Dashboard word for word. They are reference material,
   not a control, and reference material belongs in the manual. */
var HB_RECIPES = [
  ["Repeat a doorbell to a second chime",
   "Signal receiver (Front door) → Signal sender (Virtual chime 1). Two nodes, one each way: " +
   "the receiver fires when the doorbell is heard, the sender puts the chime's code on air."],
  ["Press a button in Home Assistant, ring the chime",
   "MQTT button (name it “Ring the chime”) → Signal sender (chime). That is the whole recipe: " +
   "the button appears in Home Assistant by itself, with no YAML and no automation, and " +
   "pressing it puts the code on air. The topic follows the node's name unless you type one, " +
   "so nothing else has to be filled in."],
  ["Several buttons, one chime",
   "Signal receiver (Front) + Signal receiver (Back) → Group (mode: ANY) → Signal sender " +
   "(Virtual chime 1). ANY is a merge point — it passes on whatever reaches it — so this is " +
   "how you fold several remotes into a single virtual signal with no special node."],
  ["Two buttons pressed together",
   "Same as above but set the Group to mode ALL and give it a window (e.g. 3 s). Now nothing " +
   "passes until BOTH receivers have fired inside that window."],
  ["Ring several times from one press",
   "Signal receiver (Front door) → Repeat (3 times, 5 s) → Signal sender (chime). The chime " +
   "rings at once and then twice more, five seconds apart — the one Repeat node does it all. " +
   "Press again mid-run and the count starts over."],
  ["Stop a stuck button ringing forever",
   "Signal receiver (Front door) → Rate limit (10 s cooldown) → Signal sender (chime)."],
  ["Relay a code you hear as a different code",
   "Signal receiver (neighbour's remote) → Signal sender (your chime). Two nodes, so the map " +
   "reads left to right and you can put a Rate limit or a Switch between them. The same code " +
   "on both sides is legal too — the box ignores its own transmission for a second afterwards, " +
   "so it does not hear itself and go round again."],
  ["Turn the inside bell off from Home Assistant, keep the outside one",
   "Signal receiver (Front door) → Switch “Outside bell” (topic: outside_bell) → Signal sender " +
   "(outside chime), and the SAME receiver → Switch “Inside bell” (topic: inside_bell) → " +
   "Signal sender (inside chime). Two toggles appear in Home Assistant. Switch one off and that branch goes dead — " +
   "on the map its wire is drawn broken — while the other still rings. Nothing is deleted and " +
   "nothing has to be re-wired to put it back."],
  ["One toggle, several paths",
   "Give two or more Switch nodes the SAME topic. They become one Home Assistant switch that " +
   "gates every one of them at once — the way one wall switch feeds several lamps."],
  ["Tell Home Assistant someone rang",
   "Signal receiver (Front door) → MQTT publish (topic: front)."],
  ["Proxy the whole band to Home Assistant",
   "Any RF signal → MQTT publish. Every press within range reaches HA, including buttons you " +
   "never registered. Add a Rate limit in the middle if the band is busy."],
  ["Test any chain without ringing anything",
   "MQTT button (▶) → Monitor (💡). Tap ▶ on the button — on its card or straight on the " +
   "map — and watch the monitor's lamp light and a mark land on its timeline. No transmitter, " +
   "no doorbell press, nothing audible."],
  ["Check a chain fires without hearing the chime",
   "… → Signal sender (chime)  and  … → Monitor. Link the SAME upstream node into both. The monitor " +
   "changes nothing, so it can sit beside a real sink permanently — its lamp tells you the " +
   "chain reached that point even when the chime is unplugged or you are three rooms away."]
];

var DIAG_PLAIN = [
  ["CC1101_NOT_DETECTED", "The radio module did not answer at all. Nothing can be received or sent " +
   "until it does — start with 3V3, GND and the four SPI wires."],
  ["SPI_ERROR", "The bus transaction itself failed, which points at the wiring rather than the " +
   "module: a shorted pin, or something else holding the peripheral."],
  ["CC1101_OK", "The radio answered with a plausible chip id. Anything still wrong is downstream " +
   "of the module."],
  ["RADIO_CONFIG_SUSPECT", "The chip is configured but the band looks wrong — no carrier ever seen " +
   "and an implausible noise floor. Check the frequency in Settings, and that an antenna is fitted."],
  ["RF_ENERGY_NO_PULSES", "Energy is arriving but nothing survives the filter: usually a mis-tuned " +
   "frequency or bandwidth, or a transmitter using a modulation this box is not set to."],
  ["PULSES_CAPTURED", "Raw frames are being captured. If presses still do nothing, the problem is " +
   "matching or routing — not reception."],
  ["REPEAT_FRAME_DETECTED", "Several identical copies arrived together, which is what a real remote " +
   "does and noise does not."],
  ["PROTOCOL_DECODED", "A decoder recognised the frame and read its identity out of it."],
  ["UNKNOWN_PROTOCOL_RAW", "Captured but not decoded. Fully supported: the exact timings are stored " +
   "and replay works. Only the human-readable identity is missing."],
  ["TX_OK", "The pulses left the radio. A software claim only — the box cannot know whether any " +
   "receiver reacted."],
  ["TX_FAILED", "The transmit path did not complete. Check that nothing else is holding the radio, " +
   "and look for an SPI error above it."]
];

function rebuildHandbook() {
  if (!S.built.handbook) return;
  S.built.handbook = false;
  buildHandbook();
}

function buildHandbook() {
  if (S.built.handbook) return;
  S.built.handbook = true;
  var root = clear($("#tab-handbook"));
  var base = mqttBase();

  var intro = el("div", "panel");
  var ih = el("div", "panel-head");
  add(ih, el("h2", null, t("Handbook")));
  add(ih, el("p", null, t(
    "The manual, flashed into the box. It works with no internet — which matters, because the " +
    "first time you set this thing up your phone is joined to its access point and has none. " +
    "Tap a heading to open it.")));
  add(intro, ih);
  add(root, intro);

  /* ------------------------------------------------------ 1. how it works */
  var s1 = hbSection("how", t("How it works"),
    t("Three ideas, and everything else follows from them."));
  hbPs(s1.bodyEl, [
    t("The receiver is always listening. It has to be: a button you have already registered must " +
    "ring the moment it is pressed, so there is no “receive mode” to switch on and nothing to " +
    "remember to turn off."),
    t("A signal is a stored waveform, not a decoded code. When something recognises the protocol " +
    "you get a readable identity as a bonus, but an undecodable capture is a first-class " +
    "citizen — the raw pulse timings are what is stored, matched and replayed, so a remote no " +
    "decoder has ever seen still works completely."),
    t("Nodes route those events. A press arrives at a source, optional logic decides whether it " +
    "passes, and a sink acts on it — transmits a code, publishes to MQTT, lights a monitor. " +
    "Left to right, one direction, and the map on the Dashboard is the whole of it.")
  ]);
  add(s1.bodyEl, hbNote(t(
    "The radio ignores what the box itself has just transmitted for about a second afterwards, " +
    "so a sender never feeds its own receiver and goes round in a loop.")));

  /* Off the old Dashboard intro. It is the one thing about this graph that is
     genuinely non-obvious, and it was three paragraphs above the map. */
  add(s1.bodyEl, hbH(t("One code, two nodes")));
  hbPs(s1.bodyEl, [
    t("A 433 MHz code gets one node per direction. A Signal receiver fires when that code is " +
    "heard on air; a Signal sender puts a code on air when something triggers it. They draw " +
    "from the same stored codes, so a doorbell that rings a chime is simply a receiver wired " +
    "to a sender — and every wire on the map then runs the one way events actually travel."),
    t("Two more are worth knowing early: Any RF signal is a wildcard that fires on every burst " +
    "on the band, and a Group lets several buttons drive one action.")
  ]);

  /* Off the top of the canvas, where it was a nine-line legend permanently
     covering the picture it described. */
  add(s1.bodyEl, hbH(t("Reading the map")));
  hbPs(s1.bodyEl, [
    t("Drag a node to rearrange it; tap it to edit. Nodes are linked by tapping, never by " +
    "dragging a wire."),
    t("A wire drawn broken and faded carries nothing: the node at one of its ends is switched " +
    "off. Everything on the map can also be done from the list view, which is the only view " +
    "below 900 px.")
  ]);
  add(s1.bodyEl, hbH(t("The badges on a node")));
  add(s1.bodyEl, hbKV([
    ["✕", t("Deletes the node, after a confirmation.")],
    ["▶", t("Does that node's own thing, now. What that MEANS differs by type and they are " +
          "opposite directions, so each ▶ says which in its tooltip: an MQTT button fires, " +
          "a Signal receiver pretends its code was just heard, and a Signal sender actually " +
          "TRANSMITS its code over the air.")],
    ["💡", t("A Monitor's lamp. It lights whenever the chain reaches it.")],
    ["I / O", t("Flips a Switch node between conducting and not.")]
  ]));
  add(root, s1);

  /* --------------------------------------------------- 2. node reference */
  var s2 = hbSection("nodes", t("Node reference"),
    t("Every type, what it does, its ports and its settings."));
  hbNodeReference(s2.bodyEl);
  add(root, s2);

  /* ------------------------------------------------------------ 3. recipes */
  var s3 = hbSection("recipes", t("Recipes"),
    t("Patterns that need no special node type — just links."));
  HB_RECIPES.forEach(function (r) {
    var b = el("div", "hb-recipe");
    add(b, el("div", "hb-recipe-t", t(r[0])));
    add(b, el("div", "hint mono", t(r[1])));
    add(s3.bodyEl, b);
  });
  add(root, s3);

  /* ------------------------------- 4. listening and virtual signals */
  var s4 = hbSection("learn", t("Listening and virtual signals"),
    t("Registering a real remote, and inventing a code of your own."));
  var b4 = s4.bodyEl;
  add(b4, hbH(t("What a listening session actually does")));
  hbPs(b4, [
    t("The receiver is ALWAYS listening — it has to be, or a button you already registered could " +
    "not ring the instant it is pressed. A listening session changes exactly one thing: the fate " +
    "of a signal the box does NOT recognise. Normally such a burst is dropped with one line in " +
    "Activity. During a session it is kept. Signals you already know behave identically either " +
    "way."),
    t("Nothing is admitted or rejected on the basis of what it looks like. Every frame the radio " +
    "hands up becomes a candidate, and the box RANKS them instead of filtering them. The ranking " +
    "runs on evidence that needs no knowledge of any protocol: a real remote repeats itself and " +
    "band noise does not, so the number of times a waveform was heard dominates the order."),
    t("A recognised protocol and a clean timing estimate push a candidate further up, but only " +
    "ever as a tie-break. An undecoded candidate is a first-class citizen — it can sit at the " +
    "top of the list, it saves, it replays, it matches. The exact timings are what get stored; " +
    "only the human-readable name is missing."),
    t("A session always stops on its own — after its countdown, or as soon as its 32 slots are " +
    "full — so the box is never left recording behind your back.")
  ]);
  add(b4, hbNote(t(
    "This is the part that changed. There used to be a separate “learn mode” which would only " +
    "offer a candidate that repeated twice AND decoded at 65 % confidence. Both numbers were " +
    "measured on 1527-family remotes, which quietly made it a mode for one protocol: anything " +
    "else produced no candidate at all and no explanation either. Ranking replaced the gate, and " +
    "the two flows became one.")));
  add(b4, hbH(t("Registering a button")));
  add(b4, hbList([
    t("Add a Signal receiver node, or open one you already have, and choose to listen for a signal."),
    t("The box starts listening and counts down. Press the button on your remote several times, " +
    "within a few metres. Pressing repeatedly is not politeness — repetition is the evidence the " +
    "ranking runs on."),
    t("Candidates appear, most likely first, each saying why it is where it is: “seen 5 times, " +
    "decoded ev1527, 92 % confidence”. Tap the top one."),
    t("Transmit it while standing at the bell. That is the only test that means anything — the box " +
    "can confirm the pulses left the radio, never that a receiver reacted. If it does not ring, " +
    "close it and try the next candidate."),
    t("Trim it if it carries something either side of the transmission, then give it a name and " +
    "save. It becomes an ordinary stored signal and the node is wired to it.")
  ], true));
  add(b4, hbH(t("When one press turns into several rows")));
  hbPs(b4, [
    t("A recording ends after a fixed silence — the frame boundary. If a remote pauses for longer " +
    "than that in the middle of its own transmission, the boundary fires early and one press " +
    "arrives as several dissimilar pieces, none of which rings anything on its own."),
    t("The box detects that shape and says so, because otherwise the only way through it is to " +
    "transmit each scrap in turn and hope. Where the pieces fit back together it also offers the " +
    "rejoined whole as a candidate of its own, marked 🧩 — stitched using the silence actually " +
    "measured between the pieces, not a plausible-looking guess, because a wrong gap produces a " +
    "waveform that looks perfectly reasonable and rings nothing.")
  ]);
  add(b4, hbNote(t(
    "There is also a one-tap “try again with a longer gap”, which raises the frame boundary past " +
    "the widest cut that was measured. That is the real fix; the rejoin is the shortcut.")));

  add(b4, hbH(t("Virtual signals")));
  hbPs(b4, [
    t("A virtual signal is a brand-new EV1527 code that no remote in the world is using yet. It " +
    "exists so you can pair YOUR OWN receivers to this box: a plug-in chime, a relay, a socket."),
    t("A hand-made code does nothing on its own. Nothing on the band answers to an address that was " +
    "invented ten seconds ago — a Signal receiver carrying it stays silent because that code is " +
    "never heard, and a Signal sender carrying it transmits into a world where nothing is " +
    "listening. That is expected, and it is not a fault.")
  ]);
  add(b4, hbNote(t(
    "This is the single most reported “the virtual signal is broken”. It is not broken. It has " +
    "not been paired yet."), "warn"));
  add(b4, hbH(t("Pairing one to a chime")));
  add(b4, hbP(t("The order is the whole trick, and almost everyone tries it the other way round.")));
  add(b4, hbList([
    t("Put your receiver into its learning mode — usually hold its button until it beeps or its " +
    "LED blinks."),
    t("Within a few seconds, tap “Pair now” on the signal here in the UI. It sends the code 20 " +
    "times in about three quarters of a second, which no receiver that has just entered " +
    "learning mode can miss."),
    t("The receiver stores the code and rings for it from then on. Test it with Transmit.")
  ], true));
  add(b4, hbP(t(
    "If nothing happened, the receiver almost certainly was not in learning mode when the code " +
    "went out. Put it back and tap Pair now again — sending it twice is harmless.")));
  add(root, s4);

  /* ------------------------------------ 4b. When the box will not hear it
     Its own section rather than a footnote inside Listening, because reaching
     for it is a decision ("the defaults are not going to work for this
     remote"), and a decision needs a heading you can find from the table of
     contents. */
  var s4b = hbSection("raw", t("When the box will not hear your remote"),
    t("The filters that can hide a transmitter, and how to look behind them."));
  var b4b = s4b.bodyEl;

  add(b4b, hbH(t("The problem it solves")));
  hbPs(b4b, [
    t("The box's founding promise is that a signal NOBODY can decode is still recorded and still " +
    "replayable. The ordinary receiver does not quite keep that promise on its own, because four " +
    "filters sit in front of it — and every one of them is a correct guess about the cheap " +
    "1527-family remotes this box was built around, and a possible lie about anything else."),
    t("A transmitter that breaks one of those guesses is not merely mis-decoded. It is invisible: " +
    "nothing in Activity, nothing anywhere. A listening session turns three of those filters " +
    "into numbers you control and reports honestly on the fourth, so you can see what actually " +
    "arrived.")
  ]);
  add(b4b, hbKV([
    [t("Signal-strength squelch"),
      t("Bursts quieter than −75 dBm are discarded as amplifier noise. A distant or weak " +
      "transmitter is thrown away with them.")],
    [t("Minimum frame length"),
      t("Anything shorter than 32 pulses is discarded as noise. A protocol with a short frame never " +
      "appears at all.")],
    [t("Frame boundary"),
      t("8 ms of silence is what ends one recording. Too short for a protocol with long gaps and it " +
      "is chopped into fragments; too long and several repeats are glued into one frame — and " +
      "anything over 512 pulses is thrown away whole.")],
    [t("Repeat merging"),
      t("Near-identical frames within 250 ms are folded into a single event. A remote whose repeats " +
      "differ from each other confuses this.")]
  ]));

  add(b4b, hbH(t("The worked example: a remote whose protocol we do not decode")));
  hbPs(b4b, [
    t("The case this was built for, and it is worth stating precisely because the shape of it is " +
    "the diagnosis. One wireless bell button registered first time. A second button — a " +
    "different make, on the same chime — would not register at all, even though the chime itself " +
    "rings for both. So the second button certainly transmits, and certainly works."),
    t("That combination is the signature of a FILTER, not a fault. If the radio were mis-tuned or " +
    "the antenna were off, the first button would fail too. So something about the second " +
    "button's waveform is being thrown away before anyone sees it, and the only way to find out " +
    "which of the four is doing it is to look at what arrives with the filters turned down."),
    t("Nothing here depends on what the protocol is or where the remote was made. The interesting " +
    "property is simply that no decoder on this box claims it — and that has never been a reason " +
    "for a signal not to work.")
  ]);
  add(b4b, hbList([
    t("Open a listening session — from a Signal node, from Settings → Stored signals, or from " +
    "Diagnostics."),
    t("Leave the settings alone for the first run. The squelch floor is already pre-set a few dB " +
    "above whatever the band is doing right now, which is the only value that is right in both " +
    "a quiet room and a busy one."),
    t("Press the button four or five times within a couple of metres of the box."),
    t("Read the top of the list. Repetition is what puts a candidate there, so the thing you just " +
    "pressed five times should be at or near the top whether or not anything decoded it."),
    t("If the list is empty, read the verdict line. It never says “no results”: it says whether " +
    "the radio heard nothing at all, or heard something and threw it away, and which threshold " +
    "did the throwing.")
  ], true));
  add(b4b, hbNote(t(
    "That distinction is the whole point. “Nothing was received” means the radio, the antenna or " +
    "the frequency — no threshold here can help. “Something was received but did not fit” means " +
    "a number on this screen, and it is fixable in one attempt."), "warn"));

  add(b4b, hbH(t("Reading what comes back")));
  add(b4b, hbKV([
    [t("A candidate seen several times, at the top"),
      t("That is your remote. Tap it, transmit it at the bell, trim if needed, save. Whether " +
      "anything decoded it makes no difference to any of that.")],
    [t("Everything seen once, nothing repeated"),
      t("Either you pressed once, or each press is arriving slightly differently. Press several " +
      "more times in one session — repetition is what the ranking runs on.")],
    [t("“N transmissions were cut into pieces”"),
      t("The frame boundary is too SHORT: one press is being chopped up. Take the offered “try " +
      "again with a longer gap”, or use the 🧩 rejoined candidate, which is the pieces put back " +
      "together with the measured silence between them.")],
    [t("One enormous frame, or “too long”"),
      t("The frame boundary is too LONG: repeats are being glued together, and past 512 pulses the " +
      "whole thing is discarded. Lower it toward 4000 µs.")],
    [t("All 32 slots fill in a couple of seconds, everything quiet"),
      t("That is the receiver's own amplifier noise, not your remote. Take the offered “try again " +
      "at …” button, which raises the squelch floor above the measured band.")],
    [t("Nothing at all, and no carrier either"),
      t("Check the antenna is fitted, then Settings → Radio: frequency (433.92 MHz for almost " +
      "everything, but 868 MHz exists) and modulation.")]
  ]));

  add(b4b, hbH(t("Trimming, and why it matters")));
  hbPs(b4b, [
    t("Because the frame boundary is relaxed, a recording sometimes contains more than the one " +
    "transmission you want — a tail of noise, or two presses in a row. Tap a candidate and you " +
    "get its waveform with a start and an end handle: drag them, or type exact pulse numbers, " +
    "and the shaded part is exactly what will be sent and exactly what will be saved."),
    t("Transmit the selection first, standing next to the bell. That loop — send, listen, adjust, " +
    "send — is far faster than saving a signal for every attempt, and nothing is stored until " +
    "you decide it works."),
    t("Once it rings, Save as signal. It becomes an ordinary stored signal: bind it to a Signal " +
    "receiver node to make that button ring your chime, or to a Signal sender node to have the " +
    "box press the button itself. Being undecoded costs it nothing.")
  ]);
  add(b4b, hbNote(
    t("A session keeps 32 frames in RAM and stops on its own — after its countdown, or as soon as " +
    "the slots are full. Nothing is written to flash, and your node graph is untouched: while a " +
    "session runs, only signals that would have passed the NORMAL filters are still routed, so " +
    "recorded noise can never ring anything.")));
  add(root, s4b);

  /* ------------------------------------------ 5. MQTT and Home Assistant */
  var s5 = hbSection("mqtt", "MQTT & Home Assistant",
    t("The topic map, and what discovery puts in your dashboard."));
  var b5 = s5.bodyEl;
  hbPs(b5, [
    t("MQTT is off until you turn it on under Settings. There is no TLS: this is a trusted-LAN " +
    "appliance, and it talks to a broker on your own network.")
  ]);
  add(b5, hbP(
    t("Every topic below starts with the base topic, which on this box is currently:")));
  add(b5, hbCode(base));
  add(b5, hbP(t("Written “<base>” from here on. The examples use this box's real value.")));

  add(b5, hbH(t("What the box publishes")));
  add(b5, hbKV([
    [base + "/status", t("online or offline. Retained, and also the Last Will — it flips to offline " +
     "by itself if the box drops off the network without saying goodbye. Every discovered entity " +
     "uses it to decide whether it is available.")],
    [base + "/button/<name>/state", t("One recognised press. The name is a slug of the signal's " +
     "name: “Front door” becomes front_door.")],
    [base + "/unknown/state", t("A burst matching no stored signal.")],
    [base + "/unknown", t("Retained: the last unregistered burst, so you can go and look up the code " +
     "of the remote you are about to learn.")],
    [base + "/event", t("Every node firing, plus system events.")],
    [base + "/radio", t("Retained radio telemetry — noise floor, whether a CC1101 is present, the " +
     "last press. Refreshed every 10 seconds.")],
    [base + "/<topic>", t("Whatever an MQTT publish node has been given as its topic.")]
  ]));
  add(b5, hbNote(
    t("Presses are never retained, on purpose. A press is a moment, not a condition — a retained " +
    "press payload would ring every chime in the house each time Home Assistant restarts.")));

  add(b5, hbH(t("What the box listens to")));
  add(b5, hbKV([
    [base + "/button/<name>/press", t("Any message transmits that stored signal.")],
    [base + "/trigger/<topic>", t("Any message fires every MQTT button node on that topic. " +
     "The payload is ignored entirely \u2014 arriving IS the press.")],
    [base + "/switch/<topic>/set", t("Moves every Switch node on that topic. Accepts ON, OFF, 1, 0, " +
     "true, false, open, close in any case, or a JSON object.")]
  ]));
  add(b5, hbP(t("A Switch reports back, retained, on:")));
  add(b5, hbCode(base + "/switch/<topic>/state    ->  ON | OFF"));
  add(b5, hbP(
    t("It is republished when the switch moves, on every reconnect and after a reboot, so Home " +
    "Assistant never shows a stale position.")));

  add(b5, hbH(t("From the command line")));
  add(b5, hbCode("mosquitto_sub -h <broker> -t '" + base + "/#' -v"));
  add(b5, hbCode("mosquitto_pub -h <broker> -t '" + base + "/trigger/chime' -m ''"));
  add(b5, hbCode("mosquitto_pub -h <broker> -t '" + base + "/switch/outside_bell/set' -m OFF"));

  add(b5, hbH(t("Home Assistant discovery")));
  add(b5, hbP(
    t("With discovery on, everything below arrives by itself as ONE device — identified by the " +
    "box's MAC address rather than its hostname, so renaming the box does not orphan your " +
    "automations.")));
  add(b5, hbKV([
    [t("Per stored signal"), t("TWO things: a device trigger that fires on each press (for receiving) " +
     "and a button entity that replays it (for transmitting).")],
    [t("Per MQTT button topic"), t("A button entity, so a press in a Home Assistant dashboard " +
     "starts the chain \u2014 one per topic, not one per node.")],
    [t("Per Switch topic"), t("A real switch entity — one per topic, not one per node, named after the " +
     "first node on it.")],
    [t("Unregistered presses"), t("A device trigger that fires on any burst matching no stored signal.")],
    [t("Last unknown code"), t("A diagnostic sensor holding the fingerprint of the last unknown burst.")],
    [t("Radio RSSI"), t("A diagnostic sensor: the live noise floor, in dBm.")],
    [t("Radio"), t("A diagnostic binary sensor: is a CC1101 actually there.")]
  ]));
  add(b5, hbP(
    t("Presses are deliberately NOT binary sensors. A binary sensor has to be reset, the reset can " +
    "be lost, and the entity then sits “on” forever with nothing to clear it. A trigger cannot " +
    "get stuck.")));
  add(b5, hbP(
    t("Deleting a signal tells Home Assistant to forget its entities. Renaming one changes its " +
    "topic but not its entity, because entities are keyed on the signal's number.")));

  add(b5, hbH(t("Keeping one node off MQTT")));
  add(b5, hbP(
    t("Every MQTT button, Switch and MQTT publish node has an \u201cExpose to Home Assistant / " +
    "MQTT” checkbox in its editor, ticked by default. Clear it and that one node becomes " +
    "invisible to the broker: nothing is subscribed for it, nothing is published for it, and " +
    "it gets no Home Assistant entity.")));
  add(b5, hbKV([
    [t("MQTT button"), t("Loses its {topic} subscription and its HA button.",
     { topic: base + "/trigger/<topic>" })],
    ["Switch", t("Loses its {topic} subscription, its HA toggle and its " +
     "retained position. An MQTT command no longer moves it.", { topic: base + "/switch/<topic>/set" })],
    [t("MQTT publish"), t("Publishes nothing at all — not on its own topic, and not into " +
     "{topic} either.", { topic: base + "/event" })]
  ]));
  add(b5, hbP(
    t("What it had already announced is CLEARED rather than abandoned: the box publishes an empty " +
    "retained payload over the entity's config, and over a switch topic's retained state when no " +
    "exposed node carries that topic any more. Home Assistant removes the entity instead of " +
    "showing it unavailable for ever — exactly what deleting the node would have done.")));
  add(b5, hbP(
    t("Nothing inside the graph changes. A Switch still gates its wire, an MQTT button still " +
    "fires from its ▶ button and from the API, and an MQTT publish node is still reached by the " +
    "chain. The checkbox answers one question only: can anything outside the box see it.")));
  add(b5, hbNote(
    t("It exists because a blank topic stopped meaning “no MQTT” on a Switch and on an MQTT " +
    "button alike — both follow the node's name instead, so one called “Outside bell” answers on " +
    "outside_bell whether you asked for a topic or not. A magic topic value was considered and " +
    "rejected: “-” is a perfectly legal MQTT topic level, so any sentinel would collide with a " +
    "topic somebody could legitimately want.")));

  add(b5, hbH(t("What makes a topic valid")));
  add(b5, hbP(
    t("Every topic you can type — a node's, the base topic and the discovery prefix under Settings " +
    "— is checked by the same rule, as you type and again on the box. A rejected topic is never " +
    "stored and never quietly repaired; the message names the field and the character.")));
  add(b5, hbKV([
    [t("# and +"), t("MQTT wildcards. Publishing to a topic containing one is illegal — the broker " +
     "refuses the message or drops the connection. In the base topic that takes the whole bridge " +
     "down rather than one entity.")],
    [t("Control characters"), t("Anything unprintable, which is usually a newline pasted out of a " +
     "config file. A topic you cannot see is a topic you cannot debug.")],
    [t("A leading or trailing /"), t("Legal MQTT, but it means an empty first or last level and here " +
     "it is always a mistake. The box puts the separators in itself.")],
    [t("An empty level (a//b)"), t("Same reasoning — refused rather than silently making a topic " +
     "nobody can read.")],
    [t("Over 47 characters"), t("The field holds 47. Checked before the value is cut short, so you " +
     "are told rather than quietly given a different topic.")]
  ]));
  add(b5, hbP(
    t("Leaving a topic EMPTY is always fine. On a node it means “no topic”; under Settings it " +
    "means “use the default”.")));
  add(root, s5);

  /* -------------------------------------------------------- 6. wired button */
  var s6 = hbSection("wired", t("Wired button"),
    t("A physical button on a pin, for when there is no radio in the loop at all."));
  var b6 = s6.bodyEl;
  hbPs(b6, [
    t("Any free GPIO will do, and the pin is chosen here in the UI rather than compiled in — no " +
    "rebuild, no reflash. Add a Wired button node and its picker lists what is available on this " +
    "board; the six pins the radio uses are never offered.")
  ]);
  add(b6, hbP(t("The wiring is one wire and a button:")));
  /* The only hbCode block that is not a command. GPIO/GND are pin names and stay,
     but the two annotations are prose and are translated with the rest. */
  add(b6, hbCode(
    t("GPIO  ---+--- button --- GND\n" +
      "         |\n" +
      "         +--- internal pull-up, enabled by the firmware")));
  hbPs(b6, [
    t("That is all of it. The pull-up is internal, so there is no resistor to add, and an " +
    "unpressed — or entirely unconnected — pin reads as “not pressed” rather than floating and " +
    "firing at random."),
    t("A 50 ms debounce is applied by default. Without it a single press fires the chain several " +
    "times, because a mechanical contact bounces.")
  ]);
  add(root, s6);

  /* --------------------------------------------- 7. when it does not work */
  var s7 = hbSection("trouble", t("When something does not work"),
    t("Start at Diagnostics. It tells the five causes apart."));
  var b7 = s7.bodyEl;
  hbPs(b7, [
    t("“It does not work” has at least five different causes on an RF box: a dead SPI bus, a " +
    "mis-tuned radio, a noisy band, an unrecognised protocol, or a transmit that never keyed the " +
    "carrier. The Diagnostics tab shows which one you have, because every layer of the firmware " +
    "reports into the same list of named states — the serial log, the API and that page can never " +
    "drift apart."),
    t("A state that has never fired is greyed out. The named states mean:")
  ]);
  add(b7, hbKV(DIAG_PLAIN.map(function (kv) { return [kv[0], t(kv[1]), kv[2]]; })));
  add(b7, hbH(t("Two more things worth checking first")));
  add(b7, hbList([
    t("No antenna. Sensitivity collapses without one — a 17.3 cm piece of wire is a quarter wave " +
    "at 433 MHz and is enough to judge by."),
    t("A weak USB supply. A transmitting CC1101 draws tens of milliamps in bursts, so a marginal " +
    "supply browns out during transmit and not during receive. If captures work but replays " +
    "fail, suspect the power before the wiring.")
  ]));
  add(b7, hbP(
    t("If the chime rings twice for one press, look for an Any RF signal node and a Signal receiver " +
    "both reaching the same sink. Both are firing, both are correct, and one of them is one too " +
    "many.")));
  add(b7, hbP(
    t("And if ONE remote will not register while others do — nothing in Activity, however often " +
    "you press it — that is not a fault, it is a filter. Open a listening session and read the " +
    "verdict line: it says which of the four thresholds threw your remote away. The section " +
    "above walks through it.")));
  add(root, s7);

  /* ------------------------------------------------- 8. moving to a new box */
  var s8 = hbSection("backup", t("Moving a box's configuration"),
    t("Backup, restore, and replacing a box without teaching it everything again."));
  var b8 = s8.bodyEl;
  hbPs(b8, [
    t("Settings › Backup writes one file that holds everything this box has LEARNED: every " +
    "stored waveform, and the whole node graph with its wiring. Restore it here to undo a bad " +
    "afternoon, or on a second Klingelbox to move a working setup across."),
    t("The file holds no passwords. Not the Wi-Fi passphrase, not the hotspot password, not the " +
    "MQTT credentials — not even hashes of them. A backup is a thing you email to yourself and " +
    "leave in a cloud folder, so it carries what the doorbell KNOWS and never what it can LOG " +
    "IN TO. Network and broker settings are typed in again on the new box, once.")
  ]);

  add(b8, hbH(t("Moving to a new box, in order")));
  add(b8, hbList([
    t("On the old box: Settings › Backup › Export backup file. It downloads as " +
    "klingelbox-<name>-<date>.json."),
    t("Flash and set up the new box far enough that you can reach its web UI — Wi-Fi, and a " +
    "hostname if you want one."),
    t("On the new box: Settings › Backup › Restore, choose the file, and read what it says the " +
    "file contains."),
    t("Choose Replace on a fresh box (it has nothing to lose), or Merge to add this backup " +
    "alongside what is already there."),
    t("Set up MQTT again if you use it, and re-pair any of your own chimes that were paired to a " +
    "synthesized code — the code itself moved, but the chime is still listening for the old box.")
  ], true));

  add(b8, hbH(t("Merge or Replace")));
  add(b8, hbKV([
    [t("Merge"), t("Adds the backup to what is already here. Nothing is deleted. Signals you " +
              "already have come in a second time — a merge cannot tell a re-import from a " +
              "second doorbell.")],
    [t("Replace"), t("Deletes every stored signal and every node on this box first, then imports. " +
                "It cannot be undone, and it asks first, naming exactly what goes.")]
  ]));
  add(b8, hbNote(
    t("Whether it will FIT is checked before anything is deleted. A box holds 32 signals, 24 " +
    "nodes and 48 links; if the arithmetic does not work the import is refused with the numbers " +
    "and your box is left exactly as it was.")));

  add(b8, hbH(t("Numbers change, and that is handled")));
  hbPs(b8, [
    t("Signals and nodes are numbered by whichever box they live on, so a graph carried over " +
    "from another box would point at the wrong things — or at nothing. The restore renumbers " +
    "as it goes: signals first, then nodes pointed at their new signal numbers, then links " +
    "pointed at their new node numbers."),
    t("If a signal could not be imported, the nodes that used it are still created — unbound and " +
    "switched off, and listed in the summary. Their wiring is the laborious part to rebuild by " +
    "hand, and a node you can see and re-point is far better than one silently bound to " +
    "whatever else happened to have that number here.")
  ]);
  add(b8, hbNote(
    t("A restore is not one single operation — it cannot be, on a box with this little memory, " +
    "because the whole file never fits in it at once. So it reports as it runs and tells you " +
    "the truth at the end: how many signals, nodes and links went in, and what was skipped and " +
    "why. If it stops half way, what got in stays in."), "warn"));

  add(b8, hbH(t("Radio settings")));
  add(b8, hbP(
    t("Frequency, bandwidth, transmit power and repeat counts travel in the file too, but they " +
    "are OFF by default when you restore. They describe how a particular box behaves, with a " +
    "particular antenna, and the new one may not be the same build. Tick the box only if you " +
    "meant to copy them.")));
  add(root, s8);
}

/* ======================================================================
   RECOVERY FIRST-RUN WIZARD
   Replaces the entire page. This is served through a captive portal on a
   phone, so it must be perfect at 360 px and must never depend on a tab bar.
   ====================================================================== */

function enterRecovery(sys) {
  S.recovery = true;
  document.body.classList.add("recovery");
  stopTabPolls();
  $$(".tabpane").forEach(function (p) { p.classList.toggle("active", p.id === "tab-recovery"); });
  renderHeader();
  buildRecovery(sys);
}

function buildRecovery(sys) {
  var root = clear($("#tab-recovery"));
  var panel = el("div", "panel");
  var h = el("div", "panel-head");
  add(h, el("h2", null, t("Set up Wi-Fi")));
  add(h, el("p", null,
    t("Welcome to your Klingelbox. It has no home network yet, so it opened its own hotspot " +
    "({ssid}) and you are connected to it now. " +
    "Pick your home Wi-Fi, enter its password, and the box reboots onto your network.",
    { ssid: (sys && sys.ap_ssid) || "Klingelbox-XXXX" })));
  add(panel, h);

  /* step 1 -- scan */
  var s1 = el("div", "wizstep");
  var t1 = el("h3");
  add(t1, el("span", "stepnum", "1"), document.createTextNode(t("Pick your network")));
  add(s1, t1);
  var scanRow = el("div", "btnrow");
  var scanBtn = el("button", "btn", t("Scan again"));
  scanBtn.type = "button";
  add(scanRow, scanBtn);
  add(s1, scanRow);
  var scanMsg = el("div", "formmsg");
  add(s1, scanMsg);
  var list = el("ul", "list");
  add(s1, list);
  add(panel, s1);

  /* step 2 -- credentials */
  var s2 = el("div", "wizstep hidden");
  var t2 = el("h3");
  add(t2, el("span", "stepnum", "2"), document.createTextNode(t("Enter the password")));
  add(s2, t2);
  var ssidIn = inputEl("text", "", { maxlength: "32", placeholder: t("Network name") });
  add(s2, field(t("Network (SSID)"), ssidIn, t("Picked from the list above, or typed in for a hidden network.")));
  var passField = field(t("Password"), inputEl("password", "", { maxlength: "63" }));
  var passIn = $("input", passField);
  passIn.autocomplete = "current-password";
  var showPass = checkField(t("Show password"), false);
  showPass.input.addEventListener("change", function () {
    passIn.type = showPass.input.checked ? "text" : "password";
  });
  add(s2, passField, showPass);
  var slotSel = selectEl([
    { value: 0, label: t("Slot 1 (tried first)") },
    { value: 1, label: "Slot 2" },
    { value: 2, label: "Slot 3" }
  ], 0);
  add(s2, field(t("Save into"), slotSel, t("The box tries its three saved networks in order.")));
  var saveMsg = el("div", "formmsg");
  var saveFoot = el("div", "formfoot");
  var saveBtn = el("button", "btn primary block", t("Save and connect"));
  saveBtn.type = "button";
  add(saveFoot, saveBtn, saveMsg);
  add(s2, saveFoot);
  add(panel, s2);

  add(panel, el("div", "note",
    t("Nothing here leaves the box: the passphrase is written straight into its own flash.")));
  add(root, panel);

  var selectedAuth = null;

  function showStep2(ssid, auth) {
    ssidIn.value = ssid || "";
    selectedAuth = (typeof auth === "number") ? auth : null;
    passField.classList.toggle("hidden", selectedAuth === 0);
    showPass.classList.toggle("hidden", selectedAuth === 0);
    if (selectedAuth === 0) passIn.value = "";
    s2.classList.remove("hidden");
    s2.scrollIntoView({ block: "start" });
    if (selectedAuth !== 0) setTimeout(function () { passIn.focus(); }, 60);
  }
  ssidIn.addEventListener("input", function () {
    selectedAuth = null;
    passField.classList.remove("hidden");
    showPass.classList.remove("hidden");
  });

  function doScan() {
    scanBtn.disabled = true;
    setMsg(scanMsg, t("Scanning…"));
    clear(list);
    api("/api/wifi/scan").then(function (res) {
      scanBtn.disabled = false;
      var nets = dedupeNetworks(res.networks || []);
      if (!nets.length) {
        setMsg(scanMsg, t("No networks found. Move the box closer to your router and scan again — or type the name in below."), "err");
        showStep2("", null);
        return;
      }
      setMsg(scanMsg, t("{n} network(s) found. Tap yours.", { n: nets.length }), "ok");
      nets.forEach(function (nw) {
        var li = el("li");
        var b = el("button", "listitem");
        b.type = "button";
        add(b, el("span", "li-ico", nw.auth === 0 ? "🔓" : "🔒"));
        var main = el("div", "li-main");
        add(main, el("div", "li-title", nw.ssid));
        add(main, el("div", "li-sub",
          (nw.known ? t("already saved on this box") + "  ·  " : "") +
          (nw.auth === 0 ? t("open network") : t("password protected")) +
          (typeof nw.channel === "number" ? "  ·  " + t("ch {ch}", { ch: nw.channel }) : "")));
        add(b, main);
        add(b, el("div", "li-meta", signalBars(nw.rssi) + " " + numOr(nw.rssi, -100) + " dBm"));
        b.addEventListener("click", function () {
          $$(".listitem", list).forEach(function (x) { x.classList.remove("sel"); });
          b.classList.add("sel");
          showStep2(nw.ssid, nw.auth);
        });
        add(li, b);
        add(list, li);
      });
      /* Offer manual entry for hidden networks. */
      var li2 = el("li");
      var mb = el("button", "listitem");
      mb.type = "button";
      add(mb, el("span", "li-ico", "✎"));
      var m2 = el("div", "li-main");
      add(m2, el("div", "li-title", t("Type the name myself")));
      add(m2, el("div", "li-sub", t("For a hidden network.")));
      add(mb, m2);
      mb.addEventListener("click", function () { showStep2("", null); ssidIn.focus(); });
      add(li2, mb);
      add(list, li2);
    }).catch(function (e) {
      scanBtn.disabled = false;
      setMsg(scanMsg, t("Scan failed: {msg}", { msg: e.message }), "err");
      showStep2("", null);
    });
  }

  scanBtn.addEventListener("click", doScan);
  doScan();

  saveBtn.addEventListener("click", function () {
    var ssid = trimOf(ssidIn);
    if (!ssid) { setMsg(saveMsg, t("Pick a network, or type its name."), "err"); ssidIn.focus(); return; }
    saveBtn.disabled = true;
    setMsg(saveMsg, t("Saving…"));
    postJSON("/api/wifi", {
      slot: intOf(slotSel, 0),
      ssid: ssid,
      password: passIn.value
    }).then(function () {
      showRebooting(root, ssid, (sys && sys.hostname) || "klingelbox");
    }).catch(function (e) {
      saveBtn.disabled = false;
      setMsg(saveMsg, e.message, "err");
    });
  });
}

function signalBars(rssi) {
  var r = numOr(rssi, -100);
  if (r >= -55) return "████";
  if (r >= -67) return "███░";
  if (r >= -78) return "██░░";
  return "█░░░";
}

function showRebooting(root, ssid, hostname) {
  clear(root);
  var panel = el("div", "panel");
  var done = el("div", "done-big");
  add(done, el("div", "db-ico", "📶"));
  add(done, el("h3", null, t("Saved — the box is restarting")));
  add(done, el("p", "muted",
    t("It is joining “{ssid}” now. This hotspot disappears in a moment, which is " +
    "exactly what should happen.", { ssid: ssid })));
  add(panel, done);
  var pr = el("div", "progress indet");
  add(pr, el("i"));
  add(panel, pr);

  var steps = el("div", "wizstep");
  add(steps, el("h3", null, t("What to do next")));
  var ol = el("ol");
  ol.style.paddingLeft = "1.2rem";
  ol.style.fontSize = ".9rem";
  [
    t("Reconnect this phone to your home Wi-Fi — it may do that by itself."),
    t("Open http://{host}.local in your browser.", { host: hostname }),
    t("If that name does not resolve, look up the box's address in your router's device list.")
  ].forEach(function (line) { add(ol, el("li", null, line)); });
  add(steps, ol);
  add(steps, el("div", "note",
    t("If it does not appear, the password was probably wrong. The box notices, reopens this " +
    "setup hotspot at its next boot, and you can try again.")));
  add(panel, steps);
  add(root, panel);

  /* Keep watching: on a box that stays reachable we can confirm success. */
  var tries = 0;
  var timer = setInterval(function () {
    tries++;
    if (tries > 40) { clearInterval(timer); return; }
    api("/api/system").then(function (s) {
      if (s && s.sta_connected) {
        clearInterval(timer);
        pr.classList.add("hidden");
        add(panel, el("div", "note ok",
          t("Connected to “{ssid}” at {ip}" +
          ". Reconnect this phone to your home Wi-Fi and open http://" +
          "{host}.local", { ssid: s.sta_ssid || ssid, ip: s.sta_ip || "?",
            host: s.hostname || hostname })));
      }
    }).catch(function () { /* the hotspot dropping is the expected outcome */ });
  }, 3000);
}

/* ======================================================================
   Boot
   ====================================================================== */

loadSystem().then(function () {
  if (S.recovery) return;
  /* Radio parameters feed the dashboard status chips; a failure here is not
     fatal, it only removes a chip. */
  api("/api/radio").then(function (r) { S.radio = r; renderStatusChips(); })
    .catch(function () { S.has.radioCfg = false; });
  /* One cheap probe for listening sessions. It has to happen at boot rather
     than on the Diagnostics tab, because the other entry points (a Signal node
     and Settings → Stored signals) offer a button and must not offer one that
     404s on a firmware without the feature. */
  loadRaw().then(renderRawPanel).catch(renderRawPanel);
  onTabEnter(S.tab, false);
}).catch(function () {
  /* Even with /api/system down the shell must stay navigable: the user needs
     to be able to reach Diagnostics and Settings to work out why. */
  onTabEnter(S.tab, false);
});
poll("system", 10000, function () { loadSystem().catch(function () { /* badge already shows it */ }); });

/* The only always-on 1 Hz timer, and it is not a poll — nothing goes on the
   wire. /api/system arrives every 10 s and uptime is interpolated locally
   between samples (see uptimeNow), so the header's "up 2m 39s" has to be
   relabelled far more often than it is fetched. One string assignment, guarded
   against no-op writes, and skipped entirely while the document is hidden. It
   is deliberately outside the `timers` map: stopTabPolls() must not reach it,
   because the header is on every tab. */
setInterval(function () { if (!document.hidden) tickUptimeChip(); }, 1000);

})();
