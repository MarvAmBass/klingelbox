/* Klingelbox web UI -- "Die Klingel, lokal und ohne Cloud."
 *
 * The product name is German; the interface is English on purpose.
 *
 * Vanilla ES5-ish JavaScript, no framework, no build step, no external assets:
 * this file is flashed into a SPIFFS image on a microcontroller, so every byte
 * is paid for once in flash and again on every page load over a softAP.
 *
 * Four ideas run through the whole file.
 *
 * 0. THE GRAPH IS THE PRODUCT. There are three screens -- Dashboard, Settings,
 *    Diagnostics -- and the Dashboard is the node graph, with the live activity
 *    feed under it. There is no Signals screen and no Learn screen: a signal is
 *    learned, synthesized, inspected and rebound from inside the node that uses
 *    it. The store itself outlives the graph (deleting a node never deletes a
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
    var err = new Error("No answer from the box (connection lost).");
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
  learn: null,            /* only while a learn flow sheet is open */
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
  has: { gpio: true, diagnostics: true, radioCfg: true, ap: true, config: true, monitor: true },
  txBlock: null,          /* reason string when transmit is unavailable */

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
  return shortDur(up - ts) + " ago";
}

/* ======================================================================
   Theme toggle: Auto / Light / Dark
   Only "light" or "dark" is ever stored; an absent key means Auto, so the OS
   media query decides. The click cycle starts with the OPPOSITE of what the
   system currently shows, so the very first tap always visibly does something.
   ====================================================================== */
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
    if (mode === "light") { btn.textContent = "☀️"; btn.title = "Theme: Light (tap for Dark)"; }
    else if (mode === "dark") { btn.textContent = "🌙"; btn.title = "Theme: Dark (tap for Auto)"; }
    else { btn.textContent = "🖥"; btn.title = "Theme: Auto (tap for Light/Dark)"; }
  }
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
  x.type = "button"; x.setAttribute("aria-label", "Close");
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
    (lines || []).forEach(function (t) { add(sh.body, el("p", "small muted", t)); });
    var foot = el("div", "formfoot");
    var yes = el("button", "btn " + (danger ? "danger" : "primary"), confirmLabel || "Confirm");
    yes.type = "button";
    var no = el("button", "btn", "Cancel"); no.type = "button";
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
    add(sh.body, el("div", "empty", "Nothing to choose from."));
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
      poll("events", 2000, loadEvents);
      poll("dashclock", 1000, tickDashClock);
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
    } else if (S.txBlock && S.txBlock.indexOf("No CC1101") === 0) {
      S.txBlock = null;
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
     Dashboard already ticks at 1 Hz for the age labels, so this adds no new
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

/* /api/learn has no loader of its own any more: it is polled only for as long
   as a learn flow sheet is open, from inside openLearnFlow(). */

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
    badge.textContent = "setup mode";
    badge.className = "status-badge warn";
    dot.className = "brand-dot";
    return;
  }
  if (err || !S.sys) {
    badge.textContent = "offline";
    badge.className = "status-badge bad";
    dot.className = "brand-dot bad";
    return;
  }
  var sys = S.sys;
  if (ver && sys.version) ver.textContent = "v" + sys.version;
  if (foot && sys.version) foot.textContent = " · v" + sys.version;

  var radioOk = !(sys.radio && sys.radio.present === false);
  if (!radioOk) {
    badge.textContent = "no radio";
    badge.className = "status-badge bad";
    dot.className = "brand-dot bad";
  } else if (!sys.sta_connected) {
    badge.textContent = sys.ap_ssid ? "AP only" : "no Wi-Fi";
    badge.className = "status-badge warn";
    dot.className = "brand-dot";
  } else {
    badge.textContent = "ready";
    badge.className = "status-badge ok";
    dot.className = "brand-dot ok";
  }
  badge.title = (sys.sta_connected ? ("Wi-Fi: " + (sys.sta_ssid || "?") + " @ " + (sys.sta_ip || "?"))
                                   : "Not on a home network")
    + (radioOk ? "" : " -- CC1101 not detected");
}

/* ======================================================================
   Transmit -- one path, one place where 409/503 is turned into a reason
   ====================================================================== */

function txAvailable() { return !S.txBlock; }

/* `opts` is optional and exists so the pairing burst (a longer repeat train
   with its own wording) still goes through THIS function rather than growing a
   second transmit path that would have to re-learn what a 409/503 means:
     .body    extra transmit parameters, e.g. { repeats: 20, gap_us: 8000 }
     .sending / .sent / .ok   wording for a non-default use */
function transmit(signalId, btn, msgNode, opts) {
  opts = opts || {};
  if (!txAvailable()) { if (msgNode) setMsg(msgNode, S.txBlock, "err"); return Promise.resolve(); }
  var old = btn ? btn.textContent : null;
  if (btn) { btn.disabled = true; btn.textContent = opts.sending || "Sending…"; }
  if (msgNode) setMsg(msgNode, "");
  return postJSON("/api/signals/" + signalId + "/transmit", opts.body || {}).then(function () {
    if (btn) { btn.textContent = opts.sent || "Sent ✓"; setTimeout(function () { btn.textContent = old; btn.disabled = false; }, 1400); }
    if (msgNode) setMsg(msgNode, opts.ok || "Transmitted. This only confirms the pulses left the radio -- it cannot know a receiver reacted.", "ok");
  }).catch(function (e) {
    if (btn) { btn.textContent = old; btn.disabled = false; }
    if (e.status === 503 || e.status === 409) {
      S.txBlock = e.message || "The radio is unavailable, so nothing can be transmitted.";
      renderTxNote();
    }
    if (msgNode) setMsg(msgNode, e.message, "err");
  });
}

function txBlockNote() {
  if (txAvailable()) return null;
  return el("div", "note bad", S.txBlock);
}

/* ======================================================================
   Waveform plot -- durations_us drawn as a HIGH/LOW square wave
   ====================================================================== */

function waveform(durations, firstLevel) {
  if (!durations || !durations.length) return null;
  var MAXP = 800;
  var n = Math.min(durations.length, MAXP);
  var total = 0, i;
  for (i = 0; i < n; i++) total += Math.max(0, durations[i] || 0);
  if (!total) return null;

  var W = 1000, HI = 7, LO = 33, H = 40;
  var lvl = firstLevel ? 1 : 0;
  var x = 0;
  var d = "M0," + (lvl ? HI : LO);
  for (i = 0; i < n; i++) {
    x += (durations[i] || 0) * W / total;
    d += "H" + x.toFixed(2);
    lvl = lvl ? 0 : 1;
    d += "V" + (lvl ? HI : LO);
  }

  var wrap = el("div", "wave-wrap");
  var svg = svgEl("svg", "wave");
  svg.setAttribute("viewBox", "0 0 " + W + " " + H);
  svg.setAttribute("preserveAspectRatio", "none");
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", n + " pulses over " + Math.round(total / 1000) + " milliseconds");
  var mid = svgEl("path", "mid");
  mid.setAttribute("d", "M0,20 H" + W);
  add(svg, mid);
  var p = svgEl("path");
  p.setAttribute("d", d);
  add(svg, p);
  /* Long frames get a real pixel width and scroll horizontally instead of
     being squeezed into an unreadable comb on a phone. */
  if (n > 110) svg.style.width = Math.round(n * 7) + "px";
  add(wrap, svg);

  var box = el("div");
  add(box, wrap);
  add(box, el("div", "wave-cap",
    n + (durations.length > n ? " of " + durations.length : "") + " pulses · " +
    (total / 1000).toFixed(1) + " ms · starts " + (firstLevel ? "HIGH" : "LOW")));
  return box;
}

/* ======================================================================
   SIGNALS -- reached THROUGH nodes, never beside them

   There is no Signals screen and no Learn screen any more. A signal is not a
   thing you keep a list of; it is what a node listens for or transmits, so
   everything a signal needs happens where the signal is used:

     * a node that needs one is CREATED through the choice of that signal --
       learn it off the air, pick one already stored, or synthesize a virtual
       one -- in a single flow with a single confirmation,
     * a node that HAS one shows it inline in its editor: decode, waveform,
       pairing, transmit-to-test, rename, re-learn, swap.

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
  if (!sig) return "signal";
  return sig.name || ("Signal " + sig.id);
}
/* One line of identity: the decoder's sentence when there is one, otherwise the
   honest "raw pulses" wording -- an undecoded signal is a supported state. */
function signalIdent(sig) {
  if (sig && sig.decoded && sig.decoded.text) return sig.decoded.text;
  return "raw waveform, " + numOr(sig && sig.pulse_count, 0) + " pulses";
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
  if (!users.length) return "Not used by any node";
  return "Used by " + users.map(function (n) { return nodeName(n.id); }).join(", ");
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
  add(box, el("b", null, "Pair with a receiver"));

  var steps = el("ol");
  steps.style.margin = ".45rem 0 .55rem";
  steps.style.paddingLeft = "1.25rem";

  var s1 = el("li");
  add(s1, el("span", null, "Put your receiver into "), el("b", null, "learning mode"),
      el("span", null, " — usually hold its button until it beeps or its LED blinks."));
  var s2 = el("li");
  add(s2, el("span", null, "Tap "), el("b", null, "Pair now"),
      el("span", null, " below within a few seconds."));
  var s3 = el("li", null, "The receiver stores this code and rings for it from then on.");
  add(steps, s1, s2, s3);
  add(box, steps);

  add(box, el("div", "small muted",
    "This code is new — nothing responds to it until a receiver has learned it. " +
    "That is expected."));

  var msg = el("div", "formmsg");
  var row = el("div", "formfoot");
  var b = el("button", "btn primary", "🔗 Pair now");
  b.type = "button";
  b.disabled = !txAvailable();
  if (!txAvailable()) b.title = S.txBlock;
  b.addEventListener("click", function () {
    transmit(sig.id, b, msg, {
      body: { repeats: PAIR_REPEATS, gap_us: PAIR_GAP_US },
      sending: "Pairing…",
      sent: "Sent ✓",
      ok: "Code sent " + PAIR_REPEATS + " times. If the receiver was in learning mode it " +
          "has stored it — test it with Transmit below. If not, put it back into learning " +
          "mode and tap Pair now again."
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
  else add(chips, el("span", "chip warn", "Unknown protocol — stored as raw pulses"));
  if (sig.origin) add(chips, el("span", "chip", sig.origin));
  if (typeof sig.seen_count === "number") add(chips, el("span", "chip", "seen " + sig.seen_count + "x"));
  add(box, chips);

  if (!sig.decoded) {
    add(box, el("div", "note",
      "No decoder claimed this waveform. That is a fully supported state: the exact pulse " +
      "timings are stored and replay works normally. Only the human-readable identity is missing."));
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
    add(crow, el("span", "small muted", "Decode confidence"));
    add(crow, el("span", "small mono", sig.confidence + "%"));
    add(cwrap, crow);
    var m = el("div", "meter " + (sig.confidence >= 65 ? "ok" : sig.confidence >= 45 ? "warn" : "bad"));
    var fill = el("i");
    fill.style.width = Math.max(2, Math.min(100, sig.confidence)) + "%";
    add(m, fill);
    add(cwrap, m);
    add(cwrap, el("div", "hint",
      "Measured on this bench: real presses score 67-92%, band noise 24-48%."));
    add(box, cwrap);
  }

  /* facts */
  var kv = el("dl", "kv");
  function kvAdd(k, v) { if (v === null || v === undefined || v === "") return; add(kv, el("dt", null, k)); add(kv, el("dd", "mono", String(v))); }
  kvAdd("Id", sig.id);
  kvAdd("Fingerprint", sig.fingerprint);
  kvAdd("Base pulse", typeof sig.base_us === "number" ? sig.base_us + " us" : null);
  kvAdd("Pulses", sig.pulse_count);
  kvAdd("RSSI", typeof sig.rssi_dbm === "number" ? sig.rssi_dbm + " dBm" : null);
  kvAdd("Seen", typeof sig.seen_count === "number" ? sig.seen_count + " times" : null);
  kvAdd("Last seen", typeof sig.last_seen_s === "number" ? agoText(sig.last_seen_s) : null);
  kvAdd("Created", fmtEpoch(sig.created_at) || null);
  if (sig.decoded) {
    kvAdd("Protocol", sig.decoded.protocol);
    kvAdd("Address", typeof sig.decoded.id === "number"
      ? sig.decoded.id + " (0x" + sig.decoded.id.toString(16).toUpperCase() + ")" : null);
    kvAdd("Button", sig.decoded.button);
  }
  add(box, kv);

  /* waveform */
  var wf = waveform(sig.durations_us, sig.first_level);
  if (wf) {
    var wh = el("div", "field");
    add(wh, el("span", null, "Pulse train"));
    add(wh, wf);
    add(wh, el("span", "hint",
      "High = carrier on, low = carrier off. This is what gets replayed, verbatim, on transmit."));
    /* Why a virtual signal does not look like a captured one. Read as a bug
       otherwise -- and it has been. */
    if (sig.origin === "synthesized") {
      add(wh, el("span", "hint",
        "A synthesized frame ends with the ~9 ms sync gap the protocol puts between words, " +
        "so it has one pulse more than the same code captured off the air (50 vs 49). A " +
        "capture can never contain that gap: it is longer than the 8 ms idle threshold, so it " +
        "is exactly what ENDS the recording. Repeated on air the two are the same waveform."));
    }
    add(box, wh);
  }

  /* rename */
  var nameIn = inputEl("text", sig.name || "", { maxlength: "40" });
  add(box, field("Signal name", nameIn,
    "Shown in the activity feed, on the node card and in the picker."));

  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");

  var txb = el("button", "btn primary", "📡 Transmit");
  txb.type = "button";
  txb.disabled = !txAvailable();
  if (!txAvailable()) txb.title = S.txBlock;
  txb.addEventListener("click", function () { transmit(sig.id, txb, msg); });

  var save = el("button", "btn", "Save name");
  save.type = "button";
  save.addEventListener("click", function () {
    var n = trimOf(nameIn);
    if (!n) { setMsg(msg, "A name cannot be empty.", "err"); return; }
    save.disabled = true;
    setMsg(msg, "Saving…");
    postJSON("/api/signals/" + sig.id, { name: n }).then(function () {
      save.disabled = false;
      sig.name = n;
      setMsg(msg, "Renamed.", "ok");
      loadSignals();
      if (S.graph) loadGraph();
      if (opts.onChanged) opts.onChanged("renamed", n);
    }).catch(function (e) { save.disabled = false; setMsg(msg, e.message, "err"); });
  });

  add(foot, txb, save, msg);
  add(box, foot);

  /* Suppressed in Settings -> Stored signals, where the note would point at the
     very sheet you are reading and Delete is right underneath it anyway. */
  if (opts.storeNote !== false) {
    var others = nodesUsingSignal(sig.id, opts.node ? opts.node.id : 0);
    add(box, el("div", "note",
      "This recording belongs to the box, not to this node. Deleting the node, unlinking it or " +
      "pointing it at a different signal leaves it in the store under its name — you never have " +
      "to walk to the door and learn a button twice." +
      (others.length ? " " + usedByText(others) + " as well." : "") +
      " To remove it for good: Settings → Stored signals."));
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
     .onLearn      optional fn() -- offered as "learn one instead"
     .onVirtual    optional fn() -- offered as "create one instead"
   ---------------------------------------------------------------------- */
var pickerRefresh = null;   /* set while a picker sheet is open */

function openSignalPicker(opts) {
  opts = opts || {};
  var sh = openSheet(opts.title || "Choose a signal",
    "Everything this box has stored. Signals already used by a node are marked — you can " +
    "still pick one, two nodes may share a signal.",
    function () { pickerRefresh = null; });

  var listWrap = el("div");
  add(sh.body, listWrap);

  function alts(where) {
    if (!opts.onLearn && !opts.onVirtual) return;
    var row = el("div", "btnrow");
    row.style.marginTop = ".7rem";
    if (opts.onLearn) {
      var lb = el("button", "btn", "🎓 Learn a new button");
      lb.type = "button";
      lb.addEventListener("click", function () { sh.close(); opts.onLearn(); });
      add(row, lb);
    }
    if (opts.onVirtual) {
      var vb = el("button", "btn", "✨ Configure by hand");
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
        "Could not read the signal store: " + S.signalsErr.message));
    }

    var list = S.signals || [];
    if (!list.length) {
      add(listWrap, el("div", "empty", "No signals stored yet."));
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
      if (typeof sig.base_us === "number") subParts.push(sig.base_us + " us base");
      add(main, el("div", "li-sub", subParts.join("  ·  ")));
      add(main, el("div", "li-sub", users.length ? usedByText(users) : "Not used by any node"));
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
   Learn, as a flow rather than a screen.

   Everything the old Learn tab said is here: the countdown, the "the receiver
   is always listening" explanation, the admission thresholds with the bench
   numbers, the candidate with its confidence, repeats and RSSI, and the name
   field. It resolves with the created signal, so the caller can bind it to a
   node in the same breath -- one flow, one confirmation.

   On a phone this is a bottom sheet: countdown and candidate sit at the top,
   the explanation is collapsed underneath so the sheet still fits a 360 px
   screen without scrolling past the thing you are waiting for.
   ---------------------------------------------------------------------- */
function openLearnFlow(opts) {
  opts = opts || {};
  return new Promise(function (resolve) {
    var done = false;
    var armed = false;

    var sh = openSheet(opts.title || "Learn a button",
      opts.sub || "Arm the receiver, then press the button on your remote a few times.",
      function () {
        stopPoll("learn");
        if (done) return;
        done = true;
        /* Closing the sheet must not leave the box armed behind your back. */
        if (armed) postJSON("/api/learn/cancel", {}).catch(function () { /* ignore */ });
        resolve(null);
      });

    function finish(sig) {
      if (done) return;
      done = true;
      stopPoll("learn");
      sh.close();
      resolve(sig || null);
    }

    var count = el("div", "countdown idle", "—");
    var state = el("div", "listening");
    add(sh.body, count, state);

    var candPanel = el("div", "candidate hidden");
    add(sh.body, candPanel);

    var timeoutSel = selectEl([
      { value: 30, label: "30 seconds" },
      { value: 60, label: "1 minute" },
      { value: 120, label: "2 minutes" },
      { value: 300, label: "5 minutes" }
    ], 60);
    add(sh.body, field("Stay armed for", timeoutSel,
      "Learn mode always expires on its own, so the box never sits armed forever."));

    var msg = el("div", "formmsg");
    var ctl = el("div", "btnrow");
    var armBtn = el("button", "btn primary", "Arm learn mode");
    armBtn.type = "button";
    var cancelBtn = el("button", "btn", "Cancel");
    cancelBtn.type = "button";
    add(ctl, armBtn, cancelBtn);
    add(sh.body, ctl, msg);

    armBtn.addEventListener("click", function () {
      armBtn.disabled = true;
      setMsg(msg, "Arming…");
      postJSON("/api/learn/arm", { timeout_s: intOf(timeoutSel, 60) }).then(function () {
        armed = true;
        setMsg(msg, "Armed. Press your remote button now — several times.", "ok");
        poll("learn", 1000, tick);
      }).catch(function (e) {
        setMsg(msg, e.message, "err");
      }).then(function () { armBtn.disabled = false; });
    });
    cancelBtn.addEventListener("click", function () {
      cancelBtn.disabled = true;
      postJSON("/api/learn/cancel", {}).then(function () { armed = false; return tick(); })
        .catch(function (e) { setMsg(msg, e.message, "err"); })
        .then(function () { cancelBtn.disabled = false; });
    });

    if (opts.note) add(sh.body, el("div", "note", opts.note));

    /* The single most misunderstood part of the box, kept verbatim from the
       screen this flow replaces -- collapsed, because it is read once. */
    var ep = section("What learn mode actually does", null, false);
    add(ep.bodyEl, el("p", "small",
      "The receiver is ALWAYS listening. It has to be: a button you already registered must " +
      "ring the moment it is pressed, so there is no on-demand receive mode to switch on."));
    add(ep.bodyEl, el("p", "small",
      "Learn mode changes exactly one thing — the fate of a signal the box does NOT recognise. " +
      "Normally such a signal is dropped with one line in the activity feed. While armed, it is " +
      "offered to you here for registration instead. Signals you already know behave identically " +
      "either way."));
    add(ep.bodyEl, el("div", "note",
      "A candidate must repeat at least twice and decode with at least 65% confidence before it " +
      "is offered. Both thresholds come from bench measurements: a real remote always sends " +
      "several copies (67-92% confidence), while band noise scores 24-48% and rarely repeats. " +
      "Without those filters the box would happily register the amplifier's own noise as a doorbell."));
    add(sh.body, ep);

    function tick() {
      return api("/api/learn").then(function (res) {
        S.learn = res;
        render(res, null);
      }).catch(function (e) {
        S.learn = null;
        render(null, e);
      });
    }

    function render(L, err) {
      if (err) {
        count.textContent = "—";
        count.className = "countdown idle";
        clear(state);
        add(state, el("span", null, "Learn mode is not available: " + err.message));
        armBtn.disabled = true;
        cancelBtn.disabled = true;
        candPanel.classList.add("hidden");
        stopPoll("learn");
        return;
      }
      var active = !!(L && L.active);
      armed = active;
      armBtn.classList.toggle("hidden", active);
      cancelBtn.classList.toggle("hidden", !active);

      if (active) {
        var rem = numOr(L.remaining_s, 0);
        count.textContent = Math.floor(rem / 60) + ":" + ("0" + (rem % 60)).slice(-2);
        count.className = "countdown";
        clear(state);
        add(state, el("span", "pulse"), el("span", null,
          L.candidate ? "Candidate captured — review it below." : "Armed — press your remote button"));
      } else {
        count.textContent = "—";
        count.className = "countdown idle";
        clear(state);
        add(state, el("span", null,
          "Not armed. The receiver is still listening — known buttons work as usual."));
        /* Deliberately NOT re-scheduling the poll from here: poll() fires its
           function immediately, so calling it from inside a render that was
           itself triggered by that poll would recurse. */
      }
      renderCandidate(L && L.candidate);
    }

    function renderCandidate(c) {
      if (!c) { candPanel.classList.add("hidden"); clear(candPanel); return; }
      /* Do not rebuild while the user is typing the name. */
      if (candPanel.dataset.fp === (c.fingerprint || "") &&
          $(".cand-name", candPanel) === document.activeElement) return;
      candPanel.dataset.fp = c.fingerprint || "";
      candPanel.classList.remove("hidden");
      clear(candPanel);

      var h = el("div", "panel-head");
      add(h, el("h2", null, "New button detected"));
      add(h, el("p", null, "Give it a name and accept it, or ignore it and press a different button."));
      add(candPanel, h);

      var chips = el("div", "chiprow");
      if (c.decoded && c.decoded.text) add(chips, el("span", "chip accent mono", c.decoded.text));
      else add(chips, el("span", "chip warn", "Unknown protocol — will be stored as raw pulses"));
      if (typeof c.repeats === "number") add(chips, el("span", "chip ok", c.repeats + " repeats"));
      if (typeof c.rssi_dbm === "number") {
        add(chips, el("span", "chip " + (c.rssi_dbm > -60 ? "ok" : "warn"), c.rssi_dbm + " dBm"));
      }
      add(candPanel, chips);

      if (typeof c.confidence === "number") {
        var m = el("div", "meter " + (c.confidence >= 65 ? "ok" : "warn"));
        var f = el("i");
        f.style.width = Math.max(2, Math.min(100, c.confidence)) + "%";
        add(m, f);
        var cwrap = el("div");
        cwrap.style.margin = ".6rem 0";
        var r = el("div", "row");
        r.style.justifyContent = "space-between";
        add(r, el("span", "small muted", "Confidence"), el("span", "small mono", c.confidence + "%"));
        add(cwrap, r, m);
        add(candPanel, cwrap);
      }

      var kv = el("dl", "kv");
      if (typeof c.base_us === "number") { add(kv, el("dt", null, "Base pulse")); add(kv, el("dd", "mono", c.base_us + " us")); }
      if (typeof c.pulse_count === "number") { add(kv, el("dt", null, "Pulses")); add(kv, el("dd", "mono", String(c.pulse_count))); }
      if (c.fingerprint) { add(kv, el("dt", null, "Fingerprint")); add(kv, el("dd", "mono", c.fingerprint)); }
      add(candPanel, kv);

      var nameIn = inputEl("text", "", { maxlength: "40", placeholder: "e.g. Front door" });
      nameIn.className = "cand-name";
      add(candPanel, field("Name this button", nameIn,
        "Shown in the activity feed and on the node that uses it."));

      var cmsg = el("div", "formmsg");
      var foot = el("div", "formfoot");
      var acc = el("button", "btn primary", opts.acceptLabel || "Accept and use it");
      acc.type = "button";
      acc.addEventListener("click", function () {
        var n = trimOf(nameIn);
        if (!n) { setMsg(cmsg, "Give the button a name first.", "err"); nameIn.focus(); return; }
        acc.disabled = true;
        setMsg(cmsg, "Saving…");
        postJSON("/api/learn/accept", { name: n }).then(function (created) {
          armed = false;
          return loadSignals().then(function (list) {
            var made = (created && created.id) ? (signalById(created.id) || created)
                                               : list[list.length - 1];
            finish(made || null);
          });
        }).catch(function (e) {
          acc.disabled = false;
          setMsg(cmsg, e.message, "err");
        });
      });
      add(foot, acc, cmsg);
      add(candPanel, foot);
      if (!nameIn.value) setTimeout(function () { nameIn.focus(); }, 40);
    }

    poll("learn", 1000, tick);
    /* Straight into listening: someone who opened this flow has already said
       they want to learn a button, and should not have to say it twice. */
    if (opts.autoArm !== false) setTimeout(function () { armBtn.click(); }, 60);
  });
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
  var t = (raw || "").trim().replace(/\s+/g, "");
  if (!t) return { ok: true, blank: true, value: 0 };
  var v = NaN;
  if (/^0x[0-9a-f]+$/i.test(t)) v = parseInt(t.slice(2), 16);
  else if (/^[0-9]+$/.test(t)) v = parseInt(t, 10);
  else if (/^[0-9a-f]+$/i.test(t)) v = parseInt(t, 16);
  else return { ok: false, msg: "Use hex (0xA685A or A685A) or plain digits — nothing else." };
  if (!isFinite(v) || v < 0) return { ok: false, msg: "That is not a number." };
  if (v > ID20_MAX) {
    return { ok: false, msg: "An EV1527 address is 20 bits: 0 to 0xFFFFF (1048575). " +
      "0x" + v.toString(16).toUpperCase() + " is too large." };
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
    var sh = openSheet("Enter a code",
      isNode
        ? "A code for this node to use."
        : "A brand-new code, so you can pair your own receivers to this box.",
      function () { if (!done) { done = true; resolve(null); } });

    if (isNode) {
      add(sh.body, el("p", "small",
        "Use this when you already KNOW the code — from another Klingelbox, from a remote you " +
        "decoded elsewhere — or when you are inventing a fresh one for a receiver of your own. " +
        "If the remote is in your hand, Learn is easier and cannot get a digit wrong."));
      add(sh.body, el("div", "note warn",
        "A code on its own does nothing. A Signal sender puts it on air when something " +
        "triggers it — which is also how you pair a chime, relay or socket to it. A Signal " +
        "receiver stays quiet until that code is actually heard on air."));
    } else {
      add(sh.body, el("p", "small",
        "A virtual signal is a brand-new EV1527 code that no remote in the world is using yet. " +
        "It exists so you can pair YOUR OWN receivers to this box: put a plug-in chime, a relay " +
        "or a socket into its learning mode, then transmit this signal. From then on the receiver " +
        "obeys this box, and any node can ring it."));
    }

    var grid = el("div", "formgrid");
    var vName = inputEl("text", isNode ? "Hand-entered code" : "Virtual chime 1",
      { maxlength: "40", placeholder: isNode ? "Hand-entered code" : "Virtual chime 1" });
    var vBtn = selectEl([1, 2, 4, 8].map(function (b) { return { value: b, label: "Button " + b }; }), 8);
    var vBase = inputEl("number", "350", { min: "100", max: "1500", step: "10", inputmode: "numeric" });
    /* text, not number: "0xA685A" is the form this UI displays everywhere. */
    var vId = inputEl("text", "", { maxlength: "12", placeholder: "0xA685A or blank",
      autocapitalize: "off", autocorrect: "off" });

    var idRow = el("div", "row");
    idRow.style.gap = ".4rem";
    vId.style.flex = "1 1 8rem";
    var rnd = el("button", "btn small", "🎲 Randomize");
    rnd.type = "button";
    rnd.style.minHeight = "2.75rem";
    rnd.style.flex = "0 0 auto";
    add(idRow, vId, rnd);

    var idField = field("20-bit address", idRow,
      "Hex (0xA685A or A685A) or plain digits. Leave it blank — or tap Randomize — and the box " +
      "picks a free address itself.", "full");

    /* The preview is what makes hand-entry checkable: it is formatted exactly
       like the decoded identity shown on every signal elsewhere in the UI. */
    var preview = el("div", "hint mono");
    add(idField, preview);

    add(grid,
      field("Name", vName, null, "full"),
      field("Button code", vBtn, "The 4-bit button nibble sent with the address."),
      field("Base pulse (us)", vBase, "350 us suits most EV1527 receivers."),
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
        ? ("EV1527 id=0x????? btn=" + hex20(btn) + "   — a random address, chosen by the box")
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
    var save = el("button", "btn primary", isNode ? "Use this code" : "Create virtual signal");
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
    var anyway = el("button", "btn", "Create it anyway");
    anyway.type = "button";
    anyway.style.display = "none";
    var cancel = el("button", "btn", "Cancel");
    cancel.type = "button";
    cancel.addEventListener("click", sh.close);
    add(foot, save, anyway, cancel, msg);
    add(sh.body, foot);

    add(sh.body, el("div", "note", isNode
      ? "The node is created carrying exactly this code. It gets a “Pair with a receiver” " +
        "panel for teaching your chime the code, and anything sending that code over the air " +
        "shows up in the Activity feed under the graph."
      : "Next: the signal you are creating gets a “Pair with a receiver” panel. Put your receiver " +
        "into pairing mode and tap Pair now there — the receiver stores this code and answers to " +
        "it from then on."));

    function submit(allowDuplicate) {
      var r = syncPreview();
      if (!r.ok) { setMsg(msg, r.msg, "err"); vId.focus(); return; }
      var body = {
        name: trimOf(vName) || (isNode ? "Hand-entered code" : "Virtual signal"),
        button: intOf(vBtn, 8),
        base_us: intOf(vBase, 350),
        id20: r.blank ? 0 : r.value
      };
      if (allowDuplicate) body.allow_duplicate = true;
      save.disabled = true;
      anyway.disabled = true;
      setMsg(msg, "Creating…");
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
   DASHBOARD -- the node graph IS the product

   One screen, four things, in this order:
     1. the view switch (Map / List) and "Add node",
     2. the graph itself,
     3. the live activity feed -- watching presses arrive while you wire is
        exactly when it is needed, so it sits under the thing being wired,
     4. the recipes.
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
  { t: "source.virtual", g: "source", label: "Virtual trigger", ico: "✨",
    help: "Fires from this page, from the REST API, or from an MQTT topic." },
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
function nodeType(t) {
  for (var i = 0; i < NODE_TYPES.length; i++) if (NODE_TYPES[i].t === t) return NODE_TYPES[i];
  return { t: t, g: "logic", label: t || "unknown", ico: "⚙", help: "" };
}
function nodeById(id) {
  var ns = (S.graph && S.graph.nodes) || [];
  for (var i = 0; i < ns.length; i++) if (ns[i].id === id) return ns[i];
  return null;
}
function nodeName(id) {
  var n = nodeById(id);
  return n ? (n.name || nodeType(n.type).label) : ("node " + id);
}
function signalName(id) {
  var list = S.signals || [];
  for (var i = 0; i < list.length; i++) if (list[i].id === id) return list[i].name || ("Signal " + id);
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
  setMsg(msgNode, on ? "Switching on…" : "Switching off…");
  return postJSON("/api/graph/nodes/" + n.id + "/switch", { on: !!on })
    .then(function () {
      setMsg(msgNode, on ? "On — this path conducts again." : "Off — this path is blocked.", "ok");
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
  return confirmSheet("Delete “" + (n.name || ty.label) + "”?",
    ["The node and every link to or from it are removed.",
     "Stored signals are NOT touched — nothing has to be learned again."],
    "Delete node", true).then(function (ok) {
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
  s.setAttribute("aria-label", "Monitor indicator");
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
    hits.length ? (hits.length + " hits in the last " + Math.round(win / 60) + " minutes")
                : ("No hits in the last " + Math.round(win / 60) + " minutes"));

  /* One tick a minute: enough to read "about four minutes ago" off the strip
     without turning it into graph paper. */
  for (var m = 60; m < win; m += 60) {
    var gl = svgEl("line", "montl-grid");
    gl.setAttribute("x1", win - m); gl.setAttribute("x2", win - m);
    gl.setAttribute("y1", "0"); gl.setAttribute("y2", H);
    add(svg, gl);
  }

  var drawn = 0;
  hits.forEach(function (t) {
    var age = now - t;
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
    tip.textContent = shortDur(age) + " ago";
    add(r, tip);
    add(svg, r);
    drawn++;
  });
  add(box, svg);

  var ax = el("div", "montl-axis");
  add(ax, el("span", null, Math.round(win / 60) + " min ago"));
  add(ax, el("span", null,
    drawn ? (drawn + (hits.length > drawn ? "+" : "") + (drawn === 1 ? " hit" : " hits")) : ""));
  add(ax, el("span", null, "now"));
  add(box, ax);

  if (!drawn) {
    add(box, el("div", "montl-empty",
      S.monitor ? "Nothing yet — this monitor has not fired in the last "
                  + Math.round(win / 60) + " minutes."
                : "Waiting for the box…"));
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
  setMsg(msgNode, "Firing…");
  return postJSON("/api/graph/nodes/" + id + "/fire", {}).then(function () {
    setMsg(msgNode, "Fired. Watch the monitors and the Activity feed for what it triggered.", "ok");
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
var dashEls = {};   /* the activity feed and the box-status chips under it */

function buildDashboard() {
  if (S.built.dashboard) return;
  S.built.dashboard = true;
  var root = clear($("#tab-dashboard"));

  var p = el("div", "panel");
  var h = el("div", "panel-head");
  add(h, el("h2", null, "Your box, as a flow"));
  add(h, el("p", null,
    "A press travels left to right: something hears it, optional LOGIC decides whether it " +
    "passes, and something at the far end acts — send a signal, publish to MQTT. " +
    "Nodes are linked by tapping, never by dragging."));
  add(h, el("p", null,
    "A 433 MHz code gets one node per direction. A “Signal receiver” fires when that code is " +
    "heard on air; a “Signal sender” puts a code on air when something triggers it. They share " +
    "the same stored codes, so a doorbell that rings a chime is a receiver wired to a sender — " +
    "and every wire on the map then runs the one way events actually travel."));
  add(h, el("p", null,
    "Two more are worth knowing about: “Any RF signal” is a wildcard that fires on every " +
    "burst on the band, and a “Group” lets several buttons drive one action."));
  add(p, h);

  /* The two things that make the whole screen a lie if they are wrong: no
     radio, or no home network. They used to head the old Dashboard. */
  dashEls.statusNote = el("div");
  add(p, dashEls.statusNote);
  dashEls.txNote = el("div");
  add(p, dashEls.txNote);

  var topRow = el("div", "row");
  var addBtn = el("button", "btn primary", "➕ Add node");
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
  var bList = el("button", null, "List"); bList.type = "button";
  var bMap = el("button", null, "Map"); bMap.type = "button";
  autoEls.bList = bList; autoEls.bMap = bMap;
  bList.addEventListener("click", function () { setGraphView("list", bList, bMap, true); });
  bMap.addEventListener("click", function () { setGraphView("map", bList, bMap, true); });
  /* Map first: it is the default on any screen wide enough to show this
     switch at all, and the leading position should match the default. */
  add(autoEls.viewSwitch, bMap, bList);
  add(topRow, autoEls.viewSwitch);
  add(p, topRow);
  add(root, p);

  autoEls.listWrap = el("div", "stack");
  add(root, autoEls.listWrap);
  autoEls.canvasWrap = el("div", "panel hidden");
  add(root, autoEls.canvasWrap);
  autoEls.empty = el("div", "empty",
    "No nodes yet. Add two “Signal” nodes — your doorbell and your chime — then link the " +
    "first one's output to the second one's input.");
  add(root, autoEls.empty);

  /* --- live activity ---
     Directly under the graph on purpose: the moment you want to see presses
     arrive is the moment you are wiring the node that should catch them. */
  var ap = el("div", "panel");
  var ah = el("div", "panel-head");
  add(ah, el("h2", null, "Activity"));
  add(ah, el("p", null,
    "Everything the receiver hears and everything the nodes do about it, as it happens. " +
    "The radio listens continuously; learning a button only decides what happens to a " +
    "signal it does not recognise."));
  add(ap, ah);
  dashEls.statusRow = el("div", "chiprow");
  add(ap, dashEls.statusRow);
  dashEls.feed = el("ul", "feed");
  add(ap, dashEls.feed);
  dashEls.feedEmpty = el("div", "empty", "Nothing heard yet. Press a doorbell button.");
  add(ap, dashEls.feedEmpty);
  add(root, ap);

  /* Recipes: the group pattern is the least obvious capability here, and it is
     precisely what "make several buttons ring one chime" needs. */
  var rp = el("div", "panel");
  var rh = el("div", "panel-head");
  add(rh, el("h2", null, "Recipes"));
  add(rh, el("p", null, "Patterns that need no special node type — just links."));
  add(rp, rh);
  [
    ["Repeat a doorbell to a second chime",
     "Signal receiver (Front door) → Signal sender (Virtual chime 1). Two nodes, one each way: " +
     "the receiver fires when the doorbell is heard, the sender puts the chime's code on air."],
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
    ["Ring the chime from Home Assistant",
     "Virtual trigger (topic: front_gate) → Signal sender (chime). Publish anything to the " +
     "trigger topic and the code goes out."],
    /* The shape the split was asked for. Worth spelling out: it is the one
       pattern that used to need the same node at both ends of itself. */
    ["Relay a code you hear as a different code",
     "Signal receiver (neighbour's remote) → Signal sender (your chime). Two nodes, so the map " +
     "reads left to right and you can put a Rate limit or a Switch between them. The same code " +
     "on both sides is legal too — the box ignores its own transmission for a second afterwards, " +
     "so it does not hear itself and go round again."],
    /* The case this node type was asked for, in the user's own shape. */
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
     "Virtual trigger (▶) → Monitor (💡). Tap ▶ on the trigger — on its card or straight on the " +
     "map — and watch the monitor's lamp light and a mark land on its timeline. No transmitter, " +
     "no doorbell press, nothing audible."],
    ["Check a chain fires without hearing the chime",
     "… → Signal sender (chime)  and  … → Monitor. Link the SAME upstream node into both. The monitor " +
     "changes nothing, so it can sit beside a real sink permanently — its lamp tells you the " +
     "chain reached that point even when the chime is unplugged or you are three rooms away."]
  ].forEach(function (r) {
    var b = el("div");
    b.style.marginBottom = ".6rem";
    add(b, el("div", null, r[0]));
    add(b, el("div", "hint mono", r[1]));
    add(rp, b);
  });
  add(root, rp);

  setGraphView(defaultGraphView(), bList, bMap, false);
  watchGraphViewport();

  renderGraph();
  renderDashStatus();
  renderFeed();
}

/* ------------------------------------------------------------- activity --
   The live feed, the box-status chips beside it and the 1 Hz relabelling of
   ages: all of it came off the old Dashboard unchanged, including the serial
   guard in loadEvents() that leaves the DOM alone when nothing happened. */

function tickDashClock() {
  if (!S.built.dashboard) return;
  renderDashStatus();
  /* refresh only the age column of the feed -- cheap, no re-render */
  $$(".fi-age", dashEls.feed).forEach(function (n) {
    var ts = parseFloat(n.dataset.ts);
    if (isFinite(ts)) n.textContent = agoText(ts);
  });
}

function renderTxNote() {
  if (!dashEls.txNote) return;
  clear(dashEls.txNote);
  var warn = txBlockNote();
  if (warn) add(dashEls.txNote, warn);
}

function renderDashStatus() {
  if (!dashEls.statusRow) return;
  var row = clear(dashEls.statusRow);
  var sys = S.sys;

  renderTxNote();

  if (!sys) { add(row, el("span", "chip bad", "Box not reachable")); return; }

  var radio = sys.radio || {};
  if (radio.present === false) {
    add(row, el("span", "chip bad", "● Radio not detected"));
  } else {
    add(row, el("span", "chip ok", "● Radio listening"));
    if (typeof radio.version === "number") {
      add(row, el("span", "chip mono",
        "CC1101 v0x" + (radio.version || 0).toString(16) + " p" + numOr(radio.partnum, 0)));
    }
  }
  if (S.radio && typeof S.radio.freq_hz === "number") {
    add(row, el("span", "chip mono", fmtHz(S.radio.freq_hz)));
  }
  if (S.radio && typeof S.radio.rssi_dbm === "number") {
    add(row, el("span", "chip mono", S.radio.rssi_dbm + " dBm floor"));
  }
  add(row, el("span", "chip " + (sys.sta_connected ? "ok" : "warn"),
    sys.sta_connected ? ("Wi-Fi " + (sys.sta_ssid || "")) : "AP only"));
  var up = uptimeNow();
  if (up !== null) add(row, el("span", "chip", "up " + durText(up)));

  var note = clear(dashEls.statusNote);
  if (radio.present === false) {
    add(note, el("div", "note bad",
      "The CC1101 module did not answer on SPI, so this box can neither receive nor " +
      "transmit. Check the 3V3/GND and the four SPI wires, then open Diagnostics for " +
      "the exact probe result."));
  } else if (!sys.sta_connected) {
    add(note, el("div", "note warn",
      "No home Wi-Fi connection. The box works normally over its own access point " +
      (sys.ap_ssid ? "(" + sys.ap_ssid + ") " : "") +
      "but is not reachable from your LAN. Add a network under Settings."));
  }
}

var EVENT_KINDS = {
  rf_unmatched: { ico: "❓", label: "Unrecognised signal" },
  button_press: { ico: "🔔", label: "Button press" },
  wired_press: { ico: "🔌", label: "Wired button" },
  node_fired: { ico: "↳", label: "Node fired" },
  transmit: { ico: "📡", label: "Transmit" },
  learn: { ico: "🎓", label: "Learn" },
  system: { ico: "⚙", label: "System" }
};

function renderFeed() {
  if (!dashEls.feed) return;
  var ul = clear(dashEls.feed);
  dashEls.feedEmpty.classList.toggle("hidden", S.events.length > 0);
  S.events.slice(0, 40).forEach(function (ev) {
    var kind = EVENT_KINDS[ev.kind] || { ico: "·", label: ev.kind || "event" };
    var li = el("li", "feeditem k-" + (ev.kind || "other"));
    add(li, el("div", "fi-ico", kind.ico));
    var body = el("div", "fi-body");
    add(body, el("div", "fi-text", ev.text || kind.label));
    var meta = [];
    if (ev.kind && !ev.text) meta.push(kind.label);
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
  if (persist) {
    try { localStorage.setItem(GRAPH_VIEW_KEY, v); } catch (e) { /* private mode */ }
  }
  if (v === "map") renderCanvas();
}

function renderGraph(err) {
  if (!autoEls.listWrap) return;
  var wrap = clear(autoEls.listWrap);

  if (err) {
    add(wrap, el("div", "note bad", "Could not read the node graph: " + err.message));
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
      if (!rn) return n.signal_id ? "Signal " + n.signal_id + " (missing from the store)"
                                  : "No signal chosen yet";
      return rn + " · fires when this code is heard on air";
    }
    case "signal.tx": {
      var tn = signalName(n.signal_id);
      if (!tn) return n.signal_id ? "Signal " + n.signal_id + " (missing from the store)"
                                  : "No signal chosen yet";
      return tn + " · sends this code when triggered (" +
             numOr(n.repeats, 6) + "x, " + numOr(n.gap_us, 8000) + " us gap)";
    }
    case "source.gpio":
      return (n.gpio_pin >= 0 ? "GPIO " + n.gpio_pin : "No pin chosen") +
        " · " + (n.gpio_active_low === false ? "active high" : "active low") +
        " · " + numOr(n.gpio_debounce_ms, 50) + " ms debounce";
    case "source.virtual":
      return n.topic
        ? ("MQTT trigger: " + mqttTriggerTopic(n.topic))
        : "Fired from this page or the REST API only";
    case "source.any_rf":
      return "Fires on every received burst — registered buttons and strangers alike";
    /* The window is named only where it is used. Printing "window 1 s" beside
       mode ANY described a rule the firmware does not have: ANY passes the
       first thing through immediately and never looks at the clock. */
    case "logic.group":
      return n.group_mode === "all"
        ? ("ALL — fires only when every input has fired within " + numOr(n.window_s, 1) + " s")
        : "ANY — a merge point: anything arriving passes straight through";
    case "logic.throttle":
      return "Rings once, then ignores presses for " + numOr(n.window_s, 10) + " s";
    case "logic.repeat": {
      /* "1x" would read as a setting the user got wrong; it is a legal
         pass-through, so say what it actually does. */
      var times = numOr(n.repeats, 3);
      return times <= 1
        ? "Passes through once (no repeat)"
        : "Rings " + times + "x total, " + numOr(n.window_s, 5) + " s apart";
    }
    case "logic.switch":
      return (switchOn(n) ? "ON — events pass through" : "OFF — everything past it is blocked") +
             (n.topic ? (" · " + mqttSwitchTopic(n.topic, "set")) : " · no MQTT topic");
    case "sink.mqtt":
      return n.topic ? ("Publishes to " + mqttPublishTopic(n.topic)) : "No topic set";
    case "sink.monitor":
      return "Watches only — nothing is sent or published. Lamp stays lit " +
             numOr(n.window_s, 3) + " s per hit.";
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
                       switchOn(n) ? "ON" : "OFF"));
  else if (n.enabled === false) add(head, el("span", "chip warn", "disabled"));
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

  if (hasInput(n)) add(c, linkGroup("Inputs", ins, n, "in"));
  if (hasOutput(n)) add(c, linkGroup("Outputs", outs, n, "out"));

  var msg = el("div", "formmsg");
  var row = el("div", "btnrow");
  var edit = el("button", "btn", "Edit");
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
  /* On a Virtual trigger this is not a debug affordance, it is the node's
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
              switchOn(n) ? "Switch OFF" : "Switch ON");
    fire.type = "button";
    fire.addEventListener("click", function () { setSwitch(n, !switchOn(n), fire, msg); });
  } else {
    fire = el("button", "btn" + (virt ? " primary" : ""),
      virt ? "▶ Trigger"
        : isSignalRx(n) ? "📥 Simulate heard"
        : isSignalTx(n) ? "📡 Transmit now"
        : "Test fire");
    fire.type = "button";
    fire.addEventListener("click", function () { fireNode(n.id, fire, msg); });
  }
  var del = el("button", "btn danger", "Delete");
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
    chip.setAttribute("aria-label", "Remove link " + (dir === "in" ? "from " : "to ") + nodeName(otherId));
    chip.addEventListener("click", function () {
      confirmSheet("Remove this link?",
        [(dir === "in" ? nodeName(otherId) + "  →  " + nodeName(n.id)
                       : nodeName(n.id) + "  →  " + nodeName(otherId)),
         "Both nodes stay; only the connection between them goes away."],
        "Remove link", true).then(function (ok) {
        if (!ok) return;
        delJSON("/api/graph/links", { from: l.from, to: l.to }).then(loadGraph)
          .catch(function (e) { alertSheet("Could not remove the link", e.message); });
      });
    });
    add(row, chip);
  });

  var addChip = el("button", "linkchip add");
  addChip.type = "button";
  add(addChip, el("span", "lc-text", dir === "in" ? "＋ Add input" : "＋ Add output"));
  addChip.addEventListener("click", function () { openLinkPicker(n, dir); });
  add(row, addChip);
  add(g, row);
  return g;
}

function alertSheet(title, text) {
  var sh = openSheet(title);
  add(sh.body, el("p", "small", text));
  var foot = el("div", "formfoot");
  var ok = el("button", "btn primary", "OK"); ok.type = "button";
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
      sub: dup ? "Already linked" : nodeSummary(o),
      meta: ty.label,
      disabled: dup
    });
  });
  if (!items.length) {
    alertSheet(dir === "in" ? "Nothing can feed this node" : "Nothing this node can feed",
      dir === "in"
        ? "Add a source or a logic node first — sinks have no output to connect from."
        : "Add a logic or sink node first — sources have no input to connect to.");
    return;
  }
  pickerSheet(dir === "in" ? "Pick the node that feeds " + (n.name || nodeType(n.type).label)
                           : "Pick the node to send to",
    dir === "in" ? "It will fire this node when it triggers."
                 : "This node will fire it when it triggers.",
    items, function (otherId) {
      var body = dir === "in" ? { from: otherId, to: n.id } : { from: n.id, to: otherId };
      postJSON("/api/graph/links", body).then(loadGraph)
        .catch(function (e) { alertSheet("Could not add the link", e.message); });
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
  var items = NODE_TYPES.filter(function (t) {
    return !(t.t === "source.gpio" && !S.has.gpio) &&
           !(t.t === "sink.monitor" && !S.has.monitor);
  }).map(function (t) {
    return {
      value: t.t, icon: t.ico, label: t.label, sub: t.help, meta: t.g
    };
  });
  pickerSheet("Add a node",
    "Sources hear things, logic shapes, sinks act. A 433 MHz code needs a receiver to " +
    "hear it and a sender to say it — pick whichever end you are wiring.",
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
    topic: "", ui_x: pos.x, ui_y: pos.y
  };
  return postJSON("/api/graph/nodes", body).then(function (created) {
    return loadGraph().then(function () {
      var n = created && created.id ? nodeById(created.id) : null;
      if (n) openNodeEditor(n);
    });
  }).catch(function (e) { alertSheet("Could not add the node", e.message); });
}

/* The fork that replaces the old Learn tab and the old Signals tab in one
   question: where does this node's signal come from? Every branch ends with a
   node that already works, in one flow and one confirmation. */
function openSignalNodeFlow(type) {
  var rx = (type !== "signal.tx");
  function learn() {
    openLearnFlow({
      title: "Learn a signal",
      sub: rx
        ? "Arm the receiver, then press the remote button. The code is captured, named and " +
          "stored — this node then fires whenever it is heard again."
        : "Arm the receiver, then press the remote button. The code is captured, named and " +
          "stored — this node then sends that same code whenever it is triggered."
    }).then(function (sig) { if (sig) createNode(type, sig); });
  }
  function virtual() {
    openVirtualFlow({ mode: "signal" })
      .then(function (sig) { if (sig) createNode(type, sig); });
  }
  function existing() {
    openSignalPicker({
      title: "Signal for this node",
      onPick: function (sig) { createNode(type, sig); },
      onLearn: learn,
      onVirtual: virtual
    });
  }

  /* Learn · Select · Configure, in that order: the common case first, the
     store second, and hand-entry last for the case where the code is known but
     the remote is not in reach. */
  var items = [
    { value: "learn", icon: "🎓", label: "Learn a new button",
      sub: "Arm the receiver and press your remote. The node picks up exactly that code.",
      meta: "capture" },
    { value: "existing", icon: "📚", label: "Use a signal you already have",
      sub: "Everything this box has stored, whether a node uses it or not.",
      meta: "stored" },
    { value: "virtual", icon: "✨", label: "Configure by hand",
      sub: "Type an EV1527 code you know, or roll a random one to pair your own chime to.",
      meta: "by hand" }
  ];
  pickerSheet(rx ? "Which code should this node listen for?"
                 : "Which code should this node send?",
    rx ? "A Signal receiver IS its signal: it fires whenever that code is heard on air, and " +
         "sends nothing. Pick where the code comes from."
       : "A Signal sender IS its signal: it puts that code on air whenever something " +
         "triggers it, and listens for nothing. Pick where the code comes from.",
    items, function (choice) {
      if (choice === "learn") learn();
      else if (choice === "virtual") virtual();
      else existing();
    });
}

function openNodeEditor(n) {
  var ty = nodeType(n.type);
  var sh = openSheet(n.name || ty.label, ty.help);
  var patch = {};   /* only what the user touched is sent */

  var nameIn = inputEl("text", n.name || "", { maxlength: "40" });
  add(sh.body, field("Name", nameIn));

  /* On a Switch node this field IS the position, and it already has a control
     of its own further down that says so in the right words and moves it
     through the endpoint built for it. Offering "Enabled" as well would be two
     controls for one flag, and the wrong one would win on Save. */
  var enabled = isSwitch(n) ? null
    : checkField("Enabled", n.enabled !== false,
                 "A disabled node stays in the graph but never fires.");
  if (enabled) add(sh.body, enabled);

  var ctl = {};   /* type-specific controls */

  /* The signal lives HERE, in full, and ABOVE the node's own parameters: this
     is where the deleted Signals screen went, and "which signal" is the first
     question anyone has about a signal node. Decoded identity, waveform,
     pairing, transmit-to-test, rename, and the ways to point the node
     somewhere else. */
  if (isSignalNode(n)) {
    var sigSec = el("div", "sigsec");
    add(sh.body, el("div", "lg-label", "Signal"));
    add(sh.body, el("div", "hint", isSignalRx(n)
      ? "This node fires when this code is heard on air. It never transmits — to send a " +
        "code, add a Signal sender node and wire something into it."
      : "This node transmits this code whenever something linked into it fires. It never " +
        "listens — to react to a code, add a Signal receiver node for it."));
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
    add(grid, field("Repeats when sending", ctl.repeats,
      "Many cheap receivers need several identical copies before they act."));
    add(grid, field("Gap between copies (us)", ctl.gap));
    if (!txAvailable()) add(sh.body, el("div", "note bad", S.txBlock));
  }

  if (n.type === "source.gpio") {
    var opts = [{ value: -1, label: "— choose a pin —" }];
    var g = S.gpio;
    if (g) {
      (g.suggested || []).forEach(function (p) {
        opts.push({ value: p, label: "GPIO " + p + "  (recommended)" });
      });
      (g.available || []).forEach(function (p) {
        if ((g.suggested || []).indexOf(p) >= 0) return;
        opts.push({ value: p, label: "GPIO " + p });
      });
      (g.in_use || []).forEach(function (p) {
        opts.push({ value: p, label: "GPIO " + p + "  (in use — unavailable)", disabled: true });
      });
    } else if (n.gpio_pin >= 0) {
      opts.push({ value: n.gpio_pin, label: "GPIO " + n.gpio_pin });
    }
    ctl.pin = selectEl(opts, numOr(n.gpio_pin, -1));
    add(grid, field("Pin", ctl.pin,
      g ? "Recommended pins are listed first; pins already used by the radio are disabled." :
          "The pin list is unavailable, so only the current pin is offered.", "full"));
    ctl.activeLow = checkField("Button pulls the pin to ground (active low)", n.gpio_active_low !== false);
    add(grid, ctl.activeLow);
    ctl.pin.parentNode.classList.add("full");
    ctl.activeLow.classList.add("full");
    ctl.debounce = inputEl("number", numOr(n.gpio_debounce_ms, 50), { min: "0", max: "1000", step: "5", inputmode: "numeric" });
    add(grid, field("Debounce (ms)", ctl.debounce, "Ignores contact bounce. 50 ms suits an ordinary push button."));
    add(sh.body, el("div", "note",
      "Wiring: one leg of the button to the chosen GPIO, the other leg to GND — the " +
      "internal pull-up does the rest, so no resistor and no extra supply is needed."));
  }

  if (n.type === "source.virtual") {
    /* First, above the settings: the reason this node exists is to be fired by
       hand, so the button that does it comes before the topic it may never
       need. Same action and the same words as the card and the canvas ▶. */
    var fireMsg = el("div", "formmsg");
    var fireBtn = el("button", "btn primary block", "▶ Trigger");
    fireBtn.type = "button";
    fireBtn.addEventListener("click", function () { fireNode(n.id, fireBtn, fireMsg); });
    sh.body.insertBefore(fireBtn, grid);
    sh.body.insertBefore(fireMsg, grid);

    ctl.topic = inputEl("text", n.topic || "", { maxlength: "48", placeholder: "front_gate" });
    var topicPreview = el("div", "hint mono");
    function syncTopic() {
      var t = trimOf(ctl.topic);
      topicPreview.textContent = t
        ? ("Full topic:  " + mqttTriggerTopic(t))
        : "No topic — this node can only be fired from this page or the REST API.";
    }
    ctl.topic.addEventListener("input", syncTopic);
    syncTopic();
    var tf = field("MQTT trigger topic (optional)", ctl.topic, null, "full");
    add(tf, topicPreview);
    add(grid, tf);
    add(sh.body, el("div", "note",
      "Publishing ANY message to that topic fires this node — that is how a virtual input " +
      "becomes reachable from Home Assistant, Node-RED or a shell one-liner, with no RF involved. " +
      "Leave it empty and the node still works from the Trigger button below and from " +
      "POST /api/graph/nodes/" + n.id + "/fire."));
    if (S.has.config && !mqttEnabled()) {
      add(sh.body, el("div", "note warn",
        "MQTT is currently disabled, so the topic is stored but nothing subscribes to it yet. " +
        "Enable MQTT under Settings and it starts working — no change needed here."));
    }
  }

  if (n.type === "source.any_rf") {
    /* Deliberately no parameters: the whole point is that it is a wildcard. */
    add(sh.body, el("div", "note",
      "This node has nothing to configure. It fires on every burst the receiver hears, " +
      "including signals from buttons you never registered. Wire it to an MQTT publish sink " +
      "and Home Assistant sees every press on the band."));
    add(sh.body, el("div", "note",
      "It fires IN ADDITION to any matching Signal receiver — a registered press drives both its " +
      "own chain and this wildcard chain. That is intended, not double-firing."));
    add(sh.body, el("div", "note warn",
      "If the band around you is busy, put a Rate limit between this node and its sink. " +
      "A chatty neighbouring remote will otherwise spam your broker."));
  }

  /* TWO MODES, TWO DIFFERENT NODES really, and the editor stops pretending
     otherwise. The window belongs to ALL alone -- in ANY the firmware returns
     "passes" before it ever looks at a clock -- so showing a Window field on an
     ANY group offered a setting that changes nothing, which is worse than
     offering none. It is hidden rather than disabled: the value is still there,
     still saved, and comes straight back if the mode is switched to ALL. */
  if (n.type === "logic.group") {
    ctl.mode = selectEl([
      { value: "any", label: "ANY — merge: pass everything straight through" },
      { value: "all", label: "ALL — coincidence: wait until every input has fired" }
    ], n.group_mode === "all" ? "all" : "any");
    ctl.windowS = inputEl("number", numOr(n.window_s, 1), { min: "1", max: "6000", step: "1", inputmode: "numeric" });
    ctl.windowDflt = 1;
    add(grid, field("Mode", ctl.mode, null, "full"));
    /* The API's field is window_s, in SECONDS -- this said "ms" and passed the
       wrong variable, so the input never reached the DOM at all. */
    var winField = field("Window (seconds)", ctl.windowS,
      "How long an input is remembered while the group waits for the others.");
    add(grid, winField);
    var modeNote = el("div", "note");
    add(sh.body, modeNote);
    function syncGroupMode() {
      var all = ctl.mode.value === "all";
      winField.classList.toggle("hidden", !all);
      modeNote.textContent = all
        ? "ALL is a coincidence detector. Nothing passes until EVERY link into this node has " +
          "carried an event inside the window; when that happens it fires once, forgets them " +
          "all and starts over. Use it for “both buttons within 10 seconds means something”. " +
          "A group with no inputs can never be satisfied."
        : "ANY is a merge point, not a filter. Whatever arrives is passed straight on, " +
          "immediately — the window is not used at all in this mode. Its value is that " +
          "several buttons can meet at one node, so the chain after it is wired and edited " +
          "in a single place.";
    }
    ctl.mode.addEventListener("change", syncGroupMode);
    syncGroupMode();
  }

  if (n.type === "logic.throttle") {
    ctl.windowS = inputEl("number", numOr(n.window_s, 10), { min: "1", max: "6000", step: "1", inputmode: "numeric" });
    add(grid, field("Cooldown (seconds)", ctl.windowS,
      "The first press passes straight through; anything within the cooldown after it " +
      "is dropped. Set 30 and the bell rings at most once every 30 seconds, no matter " +
      "how often the button is pressed."));
    add(grid, noteRow("Works the same for every input — a 433 MHz remote, a wired button " +
      "or an MQTT trigger. It limits whatever is linked into it."));
  }

  if (n.type === "logic.repeat") {
    ctl.times = inputEl("number", numOr(n.repeats, 3), { min: "1", max: "20", step: "1", inputmode: "numeric" });
    ctl.windowS = inputEl("number", numOr(n.window_s, 5), { min: "1", max: "6000", step: "1", inputmode: "numeric" });
    ctl.windowDflt = 5;
    add(grid, field("Times", ctl.times,
      "How many times in total, counting the first one. 3 rings the chime three times."));
    add(grid, field("Interval (seconds)", ctl.windowS,
      "The gap between rings. 3 times at 5 seconds rings at 0 s, 5 s and 10 s."));
    add(grid, noteRow("The first ring is immediate — nobody waits at the door for a chime. " +
      "Set Times to 1 and the node simply passes the press through unchanged."));
    add(sh.body, el("div", "note",
      "Pressing again while it is still running starts the count over rather than adding a " +
      "second run, so leaning on the button cannot queue up a dozen chimes."));
  }

  if (n.type === "logic.switch") {
    /* The position leads, above everything else and above the grid: opening a
       switch is nearly always "is it on, and make it the other thing". */
    var swMsg = el("div", "formmsg");
    var swBtn = el("button", "btn block " + (switchOn(n) ? "danger" : "primary"),
                   switchOn(n) ? "Switch OFF — block this path"
                               : "Switch ON — let this path conduct");
    swBtn.type = "button";
    var swState = el("div", "note " + (switchOn(n) ? "" : "warn"),
      switchOn(n)
        ? "Currently ON. Events reaching this node pass straight through, unchanged."
        : "Currently OFF. Nothing gets past this node — everything wired after it is dead " +
          "until it is switched back on. The nodes and links are all still there.");
    swBtn.addEventListener("click", function () {
      setSwitch(n, !switchOn(n), swBtn, swMsg).then(function (ok) {
        if (ok) sh.close();
      });
    });
    sh.body.insertBefore(swState, grid);
    sh.body.insertBefore(swBtn, grid);
    sh.body.insertBefore(swMsg, grid);

    ctl.topic = inputEl("text", n.topic || "", { maxlength: "48", placeholder: "outside_bell" });
    var swPreview = el("div", "hint mono");
    function syncSw() {
      var t = trimOf(ctl.topic);
      clear(swPreview);
      if (!t) {
        swPreview.textContent =
          "No topic — this switch can only be moved from this page or the REST API.";
        return;
      }
      add(swPreview, el("div", null, "Home Assistant sets:  " + mqttSwitchTopic(t, "set")));
      add(swPreview, el("div", null, "Box reports (retained):  " + mqttSwitchTopic(t, "state")));
    }
    ctl.topic.addEventListener("input", syncSw);
    syncSw();
    var sf = field("MQTT topic (optional)", ctl.topic, null, "full");
    add(sf, swPreview);
    add(grid, sf);
    add(sh.body, el("div", "note",
      "With a topic set, Home Assistant discovers this as a real switch entity on the " +
      "Klingelbox device — a toggle, not a workaround. ON, OFF, 1, 0, true and false are all " +
      "accepted on the set topic; the box answers on the state topic, retained, so the toggle " +
      "is never stale after a restart."));
    add(sh.body, el("div", "note",
      "Several Switch nodes may share ONE topic on purpose: give “Outside bell” to two switches " +
      "and a single Home Assistant toggle gates both paths at once. The reported state is ON if " +
      "any of them is conducting."));
    if (S.has.config && !mqttEnabled()) {
      add(sh.body, el("div", "note warn",
        "MQTT is currently disabled, so the topic is stored but nothing subscribes to it yet. " +
        "The switch still works from this page. Enable MQTT under Settings and Home Assistant " +
        "picks it up — no change needed here."));
    }
  }

  if (n.type === "sink.mqtt") {
    ctl.topic = inputEl("text", n.topic || "", { maxlength: "48", placeholder: "front" });
    var pubPreview = el("div", "hint mono");
    function syncPub() {
      var t = trimOf(ctl.topic);
      pubPreview.textContent = t ? ("Publishes to:  " + mqttPublishTopic(t)) : "No topic set — nothing is published.";
    }
    ctl.topic.addEventListener("input", syncPub);
    syncPub();
    var pf = field("Topic", ctl.topic, null, "full");
    add(pf, pubPreview);
    add(grid, pf);
    if (S.has.config && !mqttEnabled()) {
      add(sh.body, el("div", "note warn",
        "MQTT is disabled under Settings, so this node stores its topic but publishes nothing yet."));
    }
  }

  if (n.type === "sink.monitor") {
    /* The timeline is the node, so it leads — before the one setting it has. */
    var monHead = el("div", "montl-head");
    add(monHead, el("span", "lg-label", "Last 10 minutes"), monitorLamp(n.id, "lamp-lg"));
    sh.body.insertBefore(monHead, grid);
    sh.body.insertBefore(monitorTimeline(n.id), grid);

    ctl.windowS = inputEl("number", numOr(n.window_s, 3),
      { min: "1", max: "60", step: "1", inputmode: "numeric" });
    ctl.windowDflt = 3;
    add(grid, field("Lamp stays lit (seconds)", ctl.windowS,
      "How long the indicator glows after each hit, and how wide each mark is drawn " +
      "on the timeline. 1–60 s."));
    add(sh.body, el("div", "note",
      "This node acts on nothing. It transmits no signal, publishes no message and touches " +
      "no pin — it only records that the chain reached it, in RAM, for the last 10 minutes. " +
      "Nothing is written to flash and nothing survives a reboot."));
    add(sh.body, el("div", "note",
      "Wire one alongside a real sink to prove a chain fires without ringing anything: " +
      "link the same node into both the Signal sender you would use and this Monitor, then " +
      "trigger the chain and watch the lamp instead of listening for the chime."));
  }

  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");
  /* Delete belongs here too, not only on the list card: opening a node from the
     map was a one-way trip with Save as the only exit. */
  var delBtn = el("button", "btn danger", "Delete node");
  delBtn.type = "button";
  delBtn.addEventListener("click", function () {
    deleteNodeConfirmed(n,
      function (e) { setMsg(msg, e.message, "err"); },
      function () { sh.close(); });
  });

  var save = el("button", "btn primary", "Save");
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
    save.disabled = true;
    setMsg(msg, "Saving…");
    postJSON("/api/graph/nodes/" + n.id, patch).then(function () {
      sh.close();
      loadGraph();
    }).catch(function (e) { save.disabled = false; setMsg(msg, e.message, "err"); });
  });
  var cancel = el("button", "btn", "Cancel");
  cancel.type = "button";
  cancel.addEventListener("click", sh.close);
  add(foot, save, cancel, delBtn, msg);
  add(sh.body, foot);
}

/* The signal section of a node editor, rebuilt in place whenever the binding
   changes. Async because the waveform only comes with GET /api/signals/{id}. */
function renderNodeSignal(wrap, n) {
  clear(wrap);
  var sid = numOr(n.signal_id, 0);

  if (!sid) {
    add(wrap, el("div", "note warn", (isSignalTx(n)
      ? "This node has no signal yet, so there is nothing for it to send. "
      : "This node has no signal yet, so nothing on air can ever fire it. ") +
      "Learn the button it should stand for, pick one you already have, or invent a code " +
      "your own receiver can be paired to."));
    add(wrap, signalChooser(wrap, n));
    return;
  }

  add(wrap, el("div", "empty", "Loading signal…"));
  api("/api/signals/" + sid).then(function (sig) {
    clear(wrap);
    add(wrap, signalBlock(sig, { node: n }));
    add(wrap, signalChooser(wrap, n));
  }).catch(function (e) {
    clear(wrap);
    add(wrap, el("div", "note bad",
      "This node points at signal " + sid + ", which the box cannot produce: " + e.message +
      " Pick another one below."));
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
    setMsg(msg, "Linking…");
    postJSON("/api/graph/nodes/" + n.id, { signal_id: sig.id }).then(function () {
      n.signal_id = sig.id;
      loadGraph();
      renderNodeSignal(wrap, n);
    }).catch(function (e) { setMsg(msg, e.message, "err"); });
  }

  var pick = el("button", "btn", bound ? "🔀 Use a different signal" : "📚 Choose a stored signal");
  pick.type = "button";
  pick.addEventListener("click", function () {
    openSignalPicker({
      title: "Signal for this node",
      node: n,
      onPick: bind,
      onLearn: function () { relearn(); },
      onVirtual: function () { makeVirtual(); }
    });
  });

  function relearn() {
    openLearnFlow({
      title: bound ? "Learn a replacement" : "Learn a button",
      sub: "Press the button this node should stand for from now on.",
      note: bound
        ? "The signal this node uses today stays in the store under its name — re-learning " +
          "only changes what this node points at."
        : null
    }).then(bind);
  }
  var learn = el("button", "btn", bound ? "🎓 Re-learn" : "🎓 Learn a new button");
  learn.type = "button";
  learn.addEventListener("click", relearn);
  add(row, pick, learn);

  /* The third path: a code entered by hand rather than learned or picked.
     openVirtualFlow does the explaining about what a made-up code means. */
  function makeVirtual() { openVirtualFlow({ mode: "signal" }).then(bind); }
  var virt = el("button", "btn", "✨ Configure by hand");
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
  if (!nodes.length) { add(wrap, el("div", "empty", "No nodes to draw.")); return; }

  add(wrap, el("p", "hint",
    "Drag a node to rearrange it; tap it to edit. The badges along its top edge: " +
    "✕ deletes it (with a confirmation) and ▶ does that node's own thing now — a " +
    "Virtual trigger fires, a Signal receiver pretends its code was just heard, a " +
    "Signal sender TRANSMITS its code over the air. Each ▶ says which in its tooltip, " +
    "because those are opposite directions. 💡 lights when a Monitor is hit, and I / O " +
    "flips a Switch. A wire drawn broken and faded carries nothing: the node at one of " +
    "its ends is switched off. Everything here is also doable from the list view."));
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

  var zbar = el("div", "row canvas-tools");
  var zOut = el("button", "btn small", "\u2212"); zOut.type = "button";
  var zLbl = el("span", "hint mono zoom-lbl");
  var zIn = el("button", "btn small", "+"); zIn.type = "button";
  var zFit = el("button", "btn small", "Fit"); zFit.type = "button";
  add(zbar, zOut, zLbl, zIn, zFit);
  add(wrap, zbar);
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
        confirmSheet("Remove this link?",
          [nodeName(l.from) + "  \u2192  " + nodeName(l.to),
           "Both nodes stay; only the connection between them goes away."],
          "Remove link", true).then(function (ok) {
          if (!ok) return;
          delJSON("/api/graph/links", { from: l.from, to: l.to }).then(loadGraph)
            .catch(function (e) { alertSheet("Could not remove the link", e.message); });
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
        alertSheet("A node cannot feed itself",
          nodeName(from) + " would be its own input, which is a loop of one and would " +
          "never settle. Link it to another node instead.");
      return;
    }
    if (!hasIn(nodeType(target.type))) {
      alertSheet("That node has no input",
                 nodeName(target.id) + " is a source, so nothing can feed into it.");
      return;
    }
    var dup = links.some(function (l) { return l.from === from && l.to === target.id; });
    if (dup) return;
    postJSON("/api/graph/links", { from: from, to: target.id }).then(loadGraph)
      .catch(function (e) { alertSheet("Could not create the link", e.message); });
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
      var t = svgEl("text", "nbadge-gl " + cls);
      t.setAttribute("x", cx); t.setAttribute("y", cy + 3.5);
      t.setAttribute("text-anchor", "middle");
      t.textContent = glyph;
      add(g, t);
    }
    return c;
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
        tip: "Trigger “" + nodeName(n.id) + "” now (fires its output; nothing is sent on air)",
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
      var what = rsid ? ("“" + (signalName(rsid) || ("signal " + rsid)) + "”")
                      : "this node's code";
      return {
        tip: "Simulate hearing " + what + " — fires this node's output and runs the chain " +
             "after it. NOTHING is transmitted; no chime rings from this.",
        why: null,
        run: function (flash) {
          flash();
          setMsg(canvasMsg, "Simulating " + what + " — nothing was sent on air.");
          fireNode(n.id, null, canvasMsg);
        }
      };
    }
    if (n.type === "signal.tx") {
      var sid = numOr(n.signal_id, 0);
      var why = !sid ? "This sender has no code bound to it yet, so there is nothing to send."
                     : (!txAvailable() ? S.txBlock : null);
      return {
        /* Says OVER THE AIR in as many words. One click here can ring a chime
           in someone's house, so the tooltip must not be coy about it. */
        tip: why || ("Transmit “" + (signalName(sid) || ("signal " + sid)) +
                     "” OVER THE AIR now — this rings anything paired to it"),
        why: why,
        run: function (flash) {
          flash();
          setMsg(canvasMsg, "Sending “" + (signalName(sid) || ("signal " + sid)) + "”…");
          transmit(sid, null, canvasMsg, {
            body: { repeats: numOr(n.repeats, 6), gap_us: numOr(n.gap_us, 8000) },
            ok: "Sent “" + (signalName(sid) || ("signal " + sid)) + "” over the air ✓ " +
                "(that the pulses left the radio — it cannot know a receiver reacted)"
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

    /* ▶ — do this node's own thing, now. On a Virtual trigger and a Signal
       receiver that is firing its output and nothing more; on a Signal sender
       it is transmitting the code OVER THE AIR, which is audible in someone's
       house. Same glyph, opposite directions, so every one of them names what
       it does in its tooltip and reports back on the message line — see the
       badge notes above. Deliberately no confirmation: it is not destructive,
       and the user asked for a button, not a dialog.

       With a Monitor downstream this is also the whole verification loop for a
       Virtual trigger: click ▶, watch 💡 light, see the mark land on the
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
      hitDisc(g, BADGE_X_ACT, BADGE_Y, BADGE_HIT,
        (on ? "Switch “" + (n.name || ty.label) + "” OFF — blocks everything wired after it"
            : "Switch “" + (n.name || ty.label) + "” ON — lets this path conduct again"),
        function () {
          if (sbg.classList) sbg.classList.add("fired");
          setSwitch(n, !on, null, canvasMsg);
        });
    }

    /* ✕ — delete, through the SAME confirmation the editor's Delete uses. The
       confirmation is what makes a bare ✕ on the map safe; a second dialog or a
       second delete path would only be a second thing to keep in step. */
    badge(g, BADGE_X_DEL, BADGE_Y, "ndel", "✕");
    hitDisc(g, BADGE_X_DEL, BADGE_Y, BADGE_HIT, "Delete " + (n.name || ty.label), function () {
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
  add(root, sectionReboot());
}

function sectionIdentity() {
  var s = section("Name on the network", "Sets the mDNS hostname: http://<name>.local", true);
  var input = inputEl("text", (S.sys && S.sys.hostname) || "klingelbox", { maxlength: "31" });
  add(s.bodyEl, field("Hostname", input, "Letters, digits and hyphens. Applies after a reboot."));
  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");
  var save = el("button", "btn primary", "Save hostname");
  save.type = "button";
  save.addEventListener("click", function () {
    var h = trimOf(input);
    if (!h) { setMsg(msg, "The hostname cannot be empty.", "err"); return; }
    save.disabled = true;
    setMsg(msg, "Saving…");
    postJSON("/api/system/hostname", { hostname: h }).then(function () {
      setMsg(msg, "Saved. It takes effect on the next reboot: http://" + h + ".local", "ok");
    }).catch(function (e) { setMsg(msg, e.message, "err"); })
      .then(function () { save.disabled = false; });
  });
  add(foot, save, msg);
  add(s.bodyEl, foot);
  return s;
}

function sectionWifi() {
  var s = section("Wi-Fi networks", "Up to three home networks, tried in order.");
  var body = s.bodyEl;
  add(body, el("div", "note",
    "Losing the LAN at runtime never drops this box into setup mode — it keeps retrying " +
    "in the background and stays usable over its own access point."));
  var slots = [];
  var dl = el("datalist");
  dl.id = "wifi-seen";
  add(body, dl);

  for (var i = 0; i < 3; i++) slots.push(wifiSlot(i, dl));
  slots.forEach(function (sl) { add(body, sl.wrap); });

  var scanRow = el("div", "row");
  var scanBtn = el("button", "btn", "Scan for networks");
  scanBtn.type = "button";
  var scanMsg = el("div", "formmsg");
  scanBtn.addEventListener("click", function () {
    scanBtn.disabled = true;
    setMsg(scanMsg, "Scanning…");
    api("/api/wifi/scan").then(function (res) {
      var nets = dedupeNetworks(res.networks || []);
      clear(dl);
      nets.forEach(function (nw) { var o = el("option"); o.value = nw.ssid; add(dl, o); });
      setMsg(scanMsg, nets.length + " network(s) found — tap an SSID box to pick one.", "ok");
    }).catch(function (e) { setMsg(scanMsg, e.message, "err"); })
      .then(function () { scanBtn.disabled = false; });
  });
  add(scanRow, scanBtn);
  add(body, scanRow, scanMsg);

  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");
  var save = el("button", "btn primary", "Save networks");
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
    setMsg(msg, "Saving…");
    postJSON("/api/config", { sta: { networks: nets } }).then(function () {
      setMsg(msg, "Saved. The box connects on the next reboot, or when the current network drops.", "ok");
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
  add(fs, el("legend", null, "Network " + (idx + 1) + (idx === 0 ? " (tried first)" : "")));
  var ssid = inputEl("text", "", { maxlength: "32", list: dl.id, placeholder: "SSID" });
  var pass = inputEl("password", "", { maxlength: "63", placeholder: "leave empty to keep" });
  pass.autocomplete = "new-password";
  var passHint = el("span", "hint", "");
  add(fs, field("Network name", ssid));
  var pf = field("Passphrase", pass);
  add(pf, passHint);
  add(fs, pf);
  function refresh() {
    var nets = (S.config && S.config.sta && S.config.sta.networks) || [];
    var n = nets[idx] || {};
    ssid.value = n.ssid || "";
    passHint.textContent = n.has_pass
      ? "A passphrase is stored. Leave empty to keep it; type a new one to replace it."
      : "No passphrase stored (open network, or slot unused).";
  }
  refresh();
  return { wrap: fs, ssid: ssid, pass: pass, refresh: refresh };
}

function sectionAp() {
  var s = section("Access point", "The box's own hotspot -- how you reach it with no LAN.");
  var body = s.bodyEl;
  var msg = el("div", "formmsg");
  var loading = el("div", "empty", "Loading…");
  add(body, loading);

  api("/api/ap").then(function (ap) {
    S.ap = ap;
    loading.remove();
    var grid = el("div", "formgrid");
    var ssid = inputEl("text", ap.ssid || "", { maxlength: "32" });
    var chan = inputEl("number", numOr(ap.channel, 6), { min: "1", max: "13", step: "1", inputmode: "numeric" });
    var enabled = checkField("Access point enabled", ap.enabled !== false);
    var fallback = checkField("Raise a setup hotspot at boot when no network connects",
      ap.fallback_enabled !== false,
      "This is what puts a factory-fresh box on the air. Turning it off can lock you out.");
    var rpass = inputEl("password", "", { maxlength: "63",
      placeholder: ap.has_recovery_pass ? "set - leave empty to keep" : "empty = open hotspot" });
    rpass.autocomplete = "new-password";
    add(grid, field("Hotspot name (SSID)", ssid, null, "full"));
    add(grid, field("Channel", chan));
    var ipRow = field("IP address", inputEl("text", ap.ip || "", {}), "Read-only.");
    $("input", ipRow).disabled = true;
    add(grid, ipRow);
    add(body, grid);
    add(body, enabled);
    add(body, fallback);
    add(body, field("Setup hotspot passphrase", rpass,
      "8-63 characters, or leave empty to keep the current setting. An open hotspot is fine on a trusted site."));

    var foot = el("div", "formfoot");
    var save = el("button", "btn primary", "Save access point");
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
      setMsg(msg, "Saving…");
      postJSON("/api/ap", body2).then(function () {
        setMsg(msg, "Saved. Changes to the hotspot apply on the next reboot.", "ok");
        rpass.value = "";
      }).catch(function (e) { setMsg(msg, e.message, "err"); })
        .then(function () { save.disabled = false; });
    });
    add(foot, save, msg);
    add(body, foot);
  }).catch(function (e) {
    loading.remove();
    S.has.ap = false;
    add(body, el("div", "note warn", "Access-point settings are not available on this firmware (" + e.message + ")."));
  });
  return s;
}

function sectionMqtt() {
  var s = section("MQTT / Home Assistant", "Publish presses to a broker and get discovered by HA.");
  var body = s.bodyEl;
  var loading = el("div", "empty", "Loading…");
  add(body, loading);

  loadConfig().then(function (cfg) {
    loading.remove();
    if (!cfg || !cfg.mqtt) {
      add(body, el("div", "note warn", "This firmware does not expose MQTT settings."));
      return;
    }
    var m = cfg.mqtt;
    var enabled = checkField("MQTT enabled", !!m.enabled);
    add(body, enabled);
    var grid = el("div", "formgrid");
    var host = inputEl("text", m.host || "", { maxlength: "63", placeholder: "192.168.1.10" });
    var port = inputEl("number", numOr(m.port, 1883), { min: "1", max: "65535", step: "1", inputmode: "numeric" });
    var user = inputEl("text", m.user || "", { maxlength: "63" });
    var pass = inputEl("password", "", { maxlength: "63", placeholder: "leave empty to keep" });
    pass.autocomplete = "new-password";
    var base = inputEl("text", m.base_topic || "klingelbox", { maxlength: "48" });
    var disc = inputEl("text", m.discovery_prefix || "homeassistant", { maxlength: "48" });
    var ha = checkField("Home Assistant discovery", m.homeassistant !== false);
    add(grid, field("Broker host", host, null, "full"));
    add(grid, field("Port", port));
    add(grid, field("Username", user));
    add(grid, field("Password", pass, "Never sent back to this page.", "full"));
    add(grid, field("Base topic", base,
      "Everything is published under this prefix, and virtual triggers listen on <base>/trigger/<topic>."));
    add(grid, field("Discovery prefix", disc));
    add(body, grid, ha);

    var msg = el("div", "formmsg");
    var foot = el("div", "formfoot");
    var save = el("button", "btn primary", "Save MQTT");
    save.type = "button";
    save.addEventListener("click", function () {
      var mm = {
        enabled: enabled.input.checked,
        host: trimOf(host),
        port: intOf(port, 1883),
        user: trimOf(user),
        base_topic: trimOf(base) || "klingelbox",
        homeassistant: ha.input.checked,
        discovery_prefix: trimOf(disc) || "homeassistant"
      };
      if (pass.value) mm.password = pass.value;
      save.disabled = true;
      setMsg(msg, "Saving…");
      postJSON("/api/config", { mqtt: mm }).then(function () {
        setMsg(msg, "Saved.", "ok");
        pass.value = "";
        return loadConfig();
      }).catch(function (e) { setMsg(msg, e.message, "err"); })
        .then(function () { save.disabled = false; });
    });
    add(foot, save, msg);
    add(body, foot);
  }).catch(function (e) {
    loading.remove();
    add(body, el("div", "note warn", "Could not load MQTT settings: " + e.message));
  });
  return s;
}

function sectionRadio() {
  var s = section("Radio", "433 MHz front end. Change these only if you know why.");
  var body = s.bodyEl;
  var loading = el("div", "empty", "Loading…");
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
    add(grid, field("Frequency (Hz)", freq, "433920000 for European doorbells.", "full"));
    add(grid, field("Modulation", mod));
    add(grid, field("Data rate (bps)", rate));
    add(grid, field("Receive bandwidth (Hz)", bw));
    add(grid, field("TX power (dBm)", pwr, "Keep this modest: 433 MHz duty-cycle limits vary by region."));
    add(grid, field("Default TX repeats", reps));
    add(grid, field("Default TX gap (us)", gap));
    add(body, grid);
    if (typeof r.rssi_dbm === "number") {
      add(body, el("div", "note",
        "Current noise floor: " + r.rssi_dbm + " dBm. On a quiet band this sits near -95 dBm; " +
        "a real press in the same room measures -24 to -42 dBm."));
    }
    var msg = el("div", "formmsg");
    var foot = el("div", "formfoot");
    var save = el("button", "btn primary", "Apply to the radio");
    save.type = "button";
    save.addEventListener("click", function () {
      save.disabled = true;
      setMsg(msg, "Reconfiguring…");
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
        setMsg(msg, "Applied live — no reboot needed.", "ok");
        renderDashStatus();
      }).catch(function (e) { setMsg(msg, e.message, "err"); })
        .then(function () { save.disabled = false; });
    });
    add(foot, save, msg);
    add(body, foot);
  }).catch(function (e) {
    loading.remove();
    S.has.radioCfg = false;
    add(body, el("div", "note bad",
      "The radio is not answering, so its parameters cannot be read or changed (" + e.message + "). " +
      "Open Diagnostics for the probe result."));
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
  var s = section("Stored signals", "", false);
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
    "Every waveform the box has learned or synthesized. Nodes come and go without "
    + "touching this list — removing one here is permanent."));

  var mkRow = el("div", "row");
  var bLearn = el("button", "btn", "\u{1F4E1} Learn a signal");
  bLearn.type = "button";
  bLearn.addEventListener("click", function () {
    openLearnFlow({}).then(function (sig) { if (sig) loadSignals(); });
  });
  var bMake = el("button", "btn", "\u2728 Create custom or random");
  bMake.type = "button";
  bMake.addEventListener("click", function () {
    openVirtualFlow({ mode: "sink" }).then(function (sig) { if (sig) loadSignals(); });
  });
  add(mkRow, bLearn, bMake);
  add(s.bodyEl, mkRow);
  add(s.bodyEl, el("div", "hint",
    "A signal created here belongs to no node yet. Wire it up later from the "
    + "Dashboard, or test it straight away by opening it above."));

  /* The rows stay a maintenance list: name, what it decodes to, whether a node
     uses it. Everything else is one tap away, in the same signalBlock() the
     node editor shows -- a signal must look and behave identically wherever
     you meet it, so there is exactly one rendering of one. */
  function render() {
    clear(listWrap);
    if (S.signalsErr) {
      add(listWrap, el("div", "note bad",
        "Could not read the signal store: " + S.signalsErr.message));
      return;
    }
    var list = S.signals || [];
    if (!list.length) {
      add(listWrap, el("div", "empty",
        "Nothing stored yet. Signals appear here once you learn a button or create a " +
        "virtual one while adding a node."));
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
        (sig.origin === "synthesized" ? "virtual" : "learned")));
      add(main, el("div", "li-sub", usedByText(users)));
      add(b, main);
      var meta = el("div", "li-meta");
      if (typeof sig.last_seen_s === "number") add(meta, el("div", null, agoText(sig.last_seen_s)));
      add(b, meta);
      b.setAttribute("aria-label", "Open " + signalLabel(sig));
      b.title = "Waveform, transmit, rename, delete";
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
  add(sh.body, el("div", "empty", "Loading…"));
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
    add(sh.body, el("div", "lg-label", "Remove from the box"));
    add(sh.body, el("div", "hint", users.length
      ? usedByText(users) + ". Deleting leaves those nodes without a signal until you give " +
        "them another one — they are not deleted with it."
      : "No node uses this signal, so deleting it changes nothing in your graph."));

    var msg = el("div", "formmsg");
    var foot = el("div", "formfoot");
    var del = el("button", "btn danger", "Delete signal");
    del.type = "button";
    del.addEventListener("click", function () {
      var lines = ["The stored waveform is removed permanently. " +
        "If it came off a remote you will have to learn that button again."];
      if (users.length) {
        lines.push(usedByText(users) + ". Those nodes keep existing but are left without a " +
          "signal, and do nothing until you give them another one.");
      } else {
        lines.push("No node uses it, so nothing in your graph changes.");
      }
      confirmSheet("Delete “" + signalLabel(sig) + "”?", lines, "Delete", true).then(function (ok) {
        if (!ok) return;
        setMsg(msg, "Deleting…");
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
    add(sh.body, el("div", "note bad", "Could not load this signal: " + e.message));
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
  return confirmSheet("Update the " + what + "?", lines, "Update").then(function (ok) {
    if (!ok) return;
    btn.disabled = true;
    setMsg(msgNode, "Downloading and flashing… this can take a minute.");
    return postJSON(path, payload).then(function () {
      setMsg(msgNode, "Flashed. The box is rebooting — reload this page in a few seconds.", "ok");
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
  add(wrap, el("h3", null, "Newer release"));
  add(wrap, el("p", "hint",
    "Asks GitHub for the newest published release. The answer is cached for a few hours — " +
    "GitHub rate-limits anonymous requests, so checking is deliberately not automatic."));

  var chips = el("div", "chiprow");
  var info = el("div");
  info.style.marginTop = ".4rem";
  var msg = el("div", "formmsg");
  var row = el("div", "btnrow");
  row.style.marginTop = ".6rem";

  var checkBtn = el("button", "btn", "Check for updates");
  checkBtn.type = "button";
  var appBtn = el("button", "btn primary hidden", "Install firmware");
  appBtn.type = "button";
  var uiBtn = el("button", "btn hidden", "Install web UI");
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

    add(chips, el("span", "chip mono", "running " + (st.current || "?")));
    if (st.checking) {
      add(chips, el("span", "chip", "checking…"));
    } else if (st.valid) {
      add(chips, el("span", "chip mono", "latest " + (st.latest || "?")));
      add(chips, st.update_available ? el("span", "chip warn", "update available")
                                     : el("span", "chip ok", "up to date"));
    } else if (!st.error) {
      add(chips, el("span", "chip", "not checked yet"));
    }

    if (st.error) add(info, el("div", "note warn", st.error));

    if (st.valid && st.html_url) {
      var a = el("a", "small", "Release notes for " + (st.latest || "the newest release") + " ↗");
      a.href = st.html_url;
      a.target = "_blank";
      a.rel = "noopener noreferrer";
      /* No <a> anywhere else in this UI, so it carries its own colour rather
         than a stylesheet rule that would exist for one link. */
      a.style.color = "var(--accent)";
      add(info, a);
    }
    if (st.valid && st.checked_at_s) {
      add(info, el("div", "hint", "Checked " + agoText(st.checked_at_s) + "."));
    }
    /* The box downloads the image itself, so no home network means no check and
       no install -- the browser upload below is the answer in that case. */
    if (st.sta_connected === false) {
      add(info, el("div", "note",
        "The box is not on a home network, so it cannot reach GitHub. Upload an image from " +
        "this device instead — that path needs no internet on the box at all."));
    }

    checkBtn.disabled = !!st.checking || st.sta_connected === false;
    checkBtn.textContent = st.checking ? "Checking…" : "Check for updates";
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
    checkBtn.textContent = "Checking…";
    postJSON("/api/update/check", { force: true }).then(function (st) {
      render(st);
      /* The fetch runs on the box; poll the local status until it settles. */
      if (st.checking) poll("update", 2000, tick);
    }).catch(function (e) {
      setMsg(msg, e.message, "err");
      checkBtn.disabled = false;
      checkBtn.textContent = "Check for updates";
    });
  });

  function install(webui, btn) {
    if (!last) return;
    var url = webui ? last.webui_url : last.app_url;
    startOta("/api/update/install", { webui: webui },
      (webui ? "web UI" : "firmware") + " to " + (last.latest || "the newest release"),
      btn, msg,
      [url,
       "The box downloads and flashes it, then reboots. Do not power it off.",
       webui ? "The current web UI is erased first; the firmware and your signals are untouched."
             : "Only the app is replaced. Update the web UI as a second step once the box is back."]);
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
  var s = section("Firmware & web UI update",
    "Two separate images: the app, and this web UI. Updating one leaves the other alone.");
  var body = s.bodyEl;

  add(body, el("div", "note",
    "The app image and the web UI live in different partitions. After an app update the old " +
    "UI is still being served until you update it too — that is normal, not a failure."));

  /* --- is there a newer release? --- */
  add(body, updateCheckBlock());

  /* --- from a URL ---
     TWO fields, not one. There are two images and they live at two different
     URLs, and a single box that had to be re-typed between the two buttons was
     how "Update web UI" ended up flashing an app image at people. Each is
     prefilled with the stable release asset for its own kind, so the common
     case is a click. */
  add(body, el("div", "divider"));
  add(body, el("h3", null, "From a URL"));
  add(body, el("div", "note",
    "Prefilled with the latest stable release of each image. They are ordinary text fields — " +
    "point them at a fork, a test build or a file on your own web server and the box fetches " +
    "that instead. The automatic check above needs none of this: it uses the URLs it finds in " +
    "the release itself."));

  var urlIn = inputEl("url", "", { placeholder: OTA_DEFAULT_APP_URL });
  urlIn.type = "url";
  add(body, field("Firmware image URL", urlIn,
    "The box downloads it itself, so it needs working internet."));
  var uiUrlIn = inputEl("url", "", { placeholder: OTA_DEFAULT_WEBUI_URL });
  uiUrlIn.type = "url";
  add(body, field("Web UI image URL", uiUrlIn,
    "The second, separate image — this page itself."));

  var urlMsg = el("div", "formmsg");
  var urlRow = el("div", "btnrow");
  var appBtn = el("button", "btn primary", "Update firmware");
  appBtn.type = "button";
  var uiBtn = el("button", "btn", "Update web UI");
  uiBtn.type = "button";
  function urlUpdate(path, what, btn, input) {
    var u = trimOf(input);
    if (!u) { setMsg(urlMsg, "Enter the " + what + " image URL first.", "err"); return; }
    startOta(path, { url: u }, what + " from this URL", btn, urlMsg,
      [u, "The box downloads and flashes it, then reboots. Do not power it off."]);
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
  add(body, el("h3", null, "Or upload from this device"));
  add(body, el("p", "hint",
    "Works with no internet on the box at all — useful over the setup hotspot."));

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
    var b = el("button", "btn", "Upload & flash");
    b.type = "button";
    b.style.marginTop = ".3rem";
    b.addEventListener("click", function () {
      var file = f.files && f.files[0];
      if (!file) { setMsg(upMsg, "Choose a .bin file first.", "err"); return; }
      confirmSheet("Flash “" + file.name + "” as the new " + what + "?",
        [(file.size / 1048576).toFixed(2) + " MB, written into flash as it arrives.",
         what === "web UI"
           ? "The current web UI is erased first. If the upload is interrupted the UI stays blank until a good image is pushed; the firmware and your signals are untouched."
           : "The image is validated before it becomes the boot target, and a bad one is rolled back on the next boot.",
         "Do not close this page or power the box off during the upload."],
        "Upload & flash").then(function (ok) {
        if (!ok) return;
        b.disabled = true;
        prog.classList.remove("hidden");
        bar.style.width = "0%";
        setMsg(upMsg, "Uploading…");
        var xhr = new XMLHttpRequest();
        xhr.open("POST", path);
        xhr.setRequestHeader("Content-Type", "application/octet-stream");
        xhr.timeout = 10 * 60 * 1000;
        xhr.upload.onprogress = function (ev) {
          if (!ev.lengthComputable) return;
          var pc = Math.round(ev.loaded * 100 / ev.total);
          bar.style.width = pc + "%";
          setMsg(upMsg, "Uploading… " + pc + "%");
        };
        xhr.onload = function () {
          var res = {};
          try { res = JSON.parse(xhr.responseText || "{}"); } catch (e) { /* non-JSON */ }
          b.disabled = false;
          if (xhr.status === 200 && !res.error) {
            bar.style.width = "100%";
            setMsg(upMsg, "Flashed. The box is rebooting — reload this page in a few seconds.", "ok");
          } else {
            prog.classList.add("hidden");
            setMsg(upMsg, "Upload failed: " + (res.error || ("HTTP " + xhr.status)), "err");
          }
        };
        xhr.onerror = xhr.ontimeout = function () {
          b.disabled = false;
          prog.classList.add("hidden");
          setMsg(upMsg, "Upload failed: the connection dropped. If the box was mid-flash it reboots on its current image.", "err");
        };
        xhr.send(file);
      });
    });
    add(wrap, b);
    return wrap;
  }
  /* The release asset is called klingelbox.bin; the label said doorbell433.bin,
     which is the project's old name and matches no file anyone can download. */
  add(body, uploadRow("Firmware image (klingelbox.bin)", "/api/ota/upload", "firmware"));
  add(body, uploadRow("Web UI image (storage.bin)", "/api/ota/webui/upload", "web UI"));
  add(body, prog, upMsg);
  return s;
}

function sectionReboot() {
  var s = section("Reboot", "Restart the box. Signals, automations and settings all survive.");
  var msg = el("div", "formmsg");
  var b = el("button", "btn danger", "Reboot now");
  b.type = "button";
  b.addEventListener("click", function () {
    confirmSheet("Reboot the box?",
      ["It is back in a few seconds. Nothing stored is lost.",
       "A doorbell press during the reboot is missed."], "Reboot", true).then(function (ok) {
      if (!ok) return;
      b.disabled = true;
      setMsg(msg, "Rebooting…");
      postJSON("/api/restart", {}).catch(function () { /* the box may drop the socket first */ });
      setTimeout(function () {
        setMsg(msg, "Reboot requested — reload this page in a few seconds.", "ok");
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
  add(h, el("h2", null, "Diagnostics"));
  add(h, el("p", null,
    "This page exists because “it does not work” has at least five different causes on an " +
    "RF box: a dead SPI bus, a mis-tuned radio, a noisy band, an unrecognised protocol, or a " +
    "transmit that never keyed the carrier. Each one shows up differently below."));
  add(p, h);
  diagEls.verdict = el("div");
  add(p, diagEls.verdict);
  add(root, p);

  var p2 = el("div", "panel");
  add(p2, el("h2", null, "Capture counters"));
  diagEls.counters = el("div", "counters");
  add(p2, diagEls.counters);
  add(root, p2);

  var p3 = el("div", "panel");
  add(p3, el("h2", null, "States"));
  add(p3, el("p", "hint",
    "Every layer of the firmware reports into this one list, so the serial log, the API and " +
    "this page can never drift apart. A state that has never fired is greyed out."));
  diagEls.states = el("div", "diag");
  add(p3, diagEls.states);
  add(root, p3);

  renderDiagnostics();
}

function renderDiagnostics(err) {
  if (!diagEls.states) return;
  var v = clear(diagEls.verdict);

  if (err || !S.diag) {
    add(v, el("div", "note " + (S.has.diagnostics ? "warn" : "bad"),
      S.has.diagnostics
        ? ("Diagnostics are momentarily unavailable" + (err ? ": " + err.message : "") + ".")
        : "This firmware does not expose /api/diagnostics."));
    return;
  }

  var states = S.diag.states || [];
  var byName = {};
  states.forEach(function (s) { byName[s.name] = s; });
  function fired(n) { return byName[n] && numOr(byName[n].count, 0) > 0; }

  var verdict, kind;
  if (fired("CC1101_NOT_DETECTED") || fired("SPI_ERROR")) {
    kind = "bad";
    verdict = "The radio module is not answering. Nothing can be received or transmitted until that is fixed — start with the wiring.";
  } else if (!fired("CC1101_OK")) {
    kind = "warn";
    verdict = "The radio has not reported a successful probe yet. If the box only just booted, give it a moment.";
  } else if (!fired("PULSES_CAPTURED")) {
    kind = "warn";
    verdict = "The radio is alive but has not captured a single frame yet. Press a 433 MHz remote within a few metres and watch this page.";
  } else if (fired("PROTOCOL_DECODED")) {
    kind = "ok";
    verdict = "Receiving and decoding normally.";
  } else {
    kind = "ok";
    verdict = "Receiving raw frames. No decoder has claimed one yet, which is fine — undecoded signals are still stored and replayable.";
  }
  add(v, el("div", "note " + kind, verdict));

  /* counters */
  var c = clear(diagEls.counters);
  var cap = S.diag.capture || {};
  ["frames", "dropped_short", "dropped_full", "overruns"].forEach(function (k) {
    var box = el("div", "counter");
    add(box, el("div", "c-num", String(numOr(cap[k], 0))));
    add(box, el("div", "c-lbl", k.replace(/_/g, " ")));
    add(box, el("div", "c-help", CAPTURE_HELP[k] || ""));
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
      count === 0 ? "never" : count + "x"));
    if (count > 0 && typeof st.last_us === "number" && st.last_us > 0 && up !== null) {
      add(head, el("span", "chip mono", shortDur(up - st.last_us / 1e6) + " ago"));
    }
    add(item, head);
    add(item, el("div", "d-help", st.help || ""));
    if (count > 0 && meta.act) add(item, el("div", "d-help muted", meta.act));
    if (st.detail) add(item, el("div", "d-detail", st.detail));
    add(list, item);
  });
  if (!states.length) add(list, el("div", "empty", "The firmware reported no diagnostic states."));
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
  add(h, el("h2", null, "Set up Wi-Fi"));
  add(h, el("p", null,
    "Welcome to your Klingelbox. It has no home network yet, so it opened its own hotspot " +
    "(" + ((sys && sys.ap_ssid) || "Klingelbox-XXXX") + ") and you are connected to it now. " +
    "Pick your home Wi-Fi, enter its password, and the box reboots onto your network."));
  add(panel, h);

  /* step 1 -- scan */
  var s1 = el("div", "wizstep");
  var t1 = el("h3");
  add(t1, el("span", "stepnum", "1"), document.createTextNode("Pick your network"));
  add(s1, t1);
  var scanRow = el("div", "btnrow");
  var scanBtn = el("button", "btn", "Scan again");
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
  add(t2, el("span", "stepnum", "2"), document.createTextNode("Enter the password"));
  add(s2, t2);
  var ssidIn = inputEl("text", "", { maxlength: "32", placeholder: "Network name" });
  add(s2, field("Network (SSID)", ssidIn, "Picked from the list above, or typed in for a hidden network."));
  var passField = field("Password", inputEl("password", "", { maxlength: "63" }));
  var passIn = $("input", passField);
  passIn.autocomplete = "current-password";
  var showPass = checkField("Show password", false);
  showPass.input.addEventListener("change", function () {
    passIn.type = showPass.input.checked ? "text" : "password";
  });
  add(s2, passField, showPass);
  var slotSel = selectEl([
    { value: 0, label: "Slot 1 (tried first)" },
    { value: 1, label: "Slot 2" },
    { value: 2, label: "Slot 3" }
  ], 0);
  add(s2, field("Save into", slotSel, "The box tries its three saved networks in order."));
  var saveMsg = el("div", "formmsg");
  var saveFoot = el("div", "formfoot");
  var saveBtn = el("button", "btn primary block", "Save and connect");
  saveBtn.type = "button";
  add(saveFoot, saveBtn, saveMsg);
  add(s2, saveFoot);
  add(panel, s2);

  add(panel, el("div", "note",
    "Nothing here leaves the box: the passphrase is written straight into its own flash."));
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
    setMsg(scanMsg, "Scanning…");
    clear(list);
    api("/api/wifi/scan").then(function (res) {
      scanBtn.disabled = false;
      var nets = dedupeNetworks(res.networks || []);
      if (!nets.length) {
        setMsg(scanMsg, "No networks found. Move the box closer to your router and scan again — or type the name in below.", "err");
        showStep2("", null);
        return;
      }
      setMsg(scanMsg, nets.length + " network(s) found. Tap yours.", "ok");
      nets.forEach(function (nw) {
        var li = el("li");
        var b = el("button", "listitem");
        b.type = "button";
        add(b, el("span", "li-ico", nw.auth === 0 ? "🔓" : "🔒"));
        var main = el("div", "li-main");
        add(main, el("div", "li-title", nw.ssid));
        add(main, el("div", "li-sub",
          (nw.known ? "already saved on this box  ·  " : "") +
          (nw.auth === 0 ? "open network" : "password protected") +
          (typeof nw.channel === "number" ? "  ·  ch " + nw.channel : "")));
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
      add(m2, el("div", "li-title", "Type the name myself"));
      add(m2, el("div", "li-sub", "For a hidden network."));
      add(mb, m2);
      mb.addEventListener("click", function () { showStep2("", null); ssidIn.focus(); });
      add(li2, mb);
      add(list, li2);
    }).catch(function (e) {
      scanBtn.disabled = false;
      setMsg(scanMsg, "Scan failed: " + e.message, "err");
      showStep2("", null);
    });
  }

  scanBtn.addEventListener("click", doScan);
  doScan();

  saveBtn.addEventListener("click", function () {
    var ssid = trimOf(ssidIn);
    if (!ssid) { setMsg(saveMsg, "Pick a network, or type its name.", "err"); ssidIn.focus(); return; }
    saveBtn.disabled = true;
    setMsg(saveMsg, "Saving…");
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
  add(done, el("h3", null, "Saved — the box is restarting"));
  add(done, el("p", "muted",
    "It is joining “" + ssid + "” now. This hotspot disappears in a moment, which is " +
    "exactly what should happen."));
  add(panel, done);
  var pr = el("div", "progress indet");
  add(pr, el("i"));
  add(panel, pr);

  var steps = el("div", "wizstep");
  add(steps, el("h3", null, "What to do next"));
  var ol = el("ol");
  ol.style.paddingLeft = "1.2rem";
  ol.style.fontSize = ".9rem";
  [
    "Reconnect this phone to your home Wi-Fi — it may do that by itself.",
    "Open http://" + hostname + ".local in your browser.",
    "If that name does not resolve, look up the box's address in your router's device list."
  ].forEach(function (t) { add(ol, el("li", null, t)); });
  add(steps, ol);
  add(steps, el("div", "note",
    "If it does not appear, the password was probably wrong. The box notices, reopens this " +
    "setup hotspot at its next boot, and you can try again."));
  add(panel, steps);
  add(root, panel);

  /* Keep watching: on a box that stays reachable we can confirm success. */
  var tries = 0;
  var t = setInterval(function () {
    tries++;
    if (tries > 40) { clearInterval(t); return; }
    api("/api/system").then(function (s) {
      if (s && s.sta_connected) {
        clearInterval(t);
        pr.classList.add("hidden");
        add(panel, el("div", "note ok",
          "Connected to “" + (s.sta_ssid || ssid) + "” at " + (s.sta_ip || "?") +
          ". Reconnect this phone to your home Wi-Fi and open http://" +
          (s.hostname || hostname) + ".local"));
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
  api("/api/radio").then(function (r) { S.radio = r; renderDashStatus(); })
    .catch(function () { S.has.radioCfg = false; });
  onTabEnter(S.tab, false);
}).catch(function () {
  /* Even with /api/system down the shell must stay navigable: the user needs
     to be able to reach Diagnostics and Settings to work out why. */
  onTabEnter(S.tab, false);
});
poll("system", 10000, function () { loadSystem().catch(function () { /* badge already shows it */ }); });

})();
