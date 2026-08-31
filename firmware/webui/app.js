/* Klingelbox web UI -- "Die Klingel, lokal und ohne Cloud."
 *
 * The product name is German; the interface is English on purpose.
 *
 * Vanilla ES5-ish JavaScript, no framework, no build step, no external assets:
 * this file is flashed into a SPIFFS image on a microcontroller, so every byte
 * is paid for once in flash and again on every page load over a softAP.
 *
 * Three ideas run through the whole file.
 *
 * 1. MOBILE FIRST, LIST FIRST. The layout is authored for a 360 px phone. The
 *    node graph in particular is a LIST of cards whose links are tappable
 *    chips; the SVG canvas at >= 900 px is a second view of the same data and
 *    never the only way to do anything.
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
   State
   ====================================================================== */

var S = {
  sys: null,
  radio: null,
  config: null,
  ap: null,
  signals: null,          /* null = not loaded yet */
  graph: null,
  gpio: null,
  learn: null,
  diag: null,

  events: [],
  serial: -1,             /* /api/events serial; -1 = nothing fetched yet */

  tab: "dashboard",
  recovery: false,
  graphView: "list",
  built: {},              /* tab -> true once its static frame is in the DOM */

  /* feature availability, driven purely by response codes */
  has: { gpio: true, diagnostics: true, radioCfg: true, ap: true, config: true },
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

function openSheet(title, sub) {
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

  function close() {
    scrim.remove();
    document.removeEventListener("keydown", onKey);
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
      poll("events", 2000, loadEvents);
      poll("dashclock", 1000, tickDashClock);
      if (!S.signals || !resumed) loadSignals();
      break;
    case "signals":
      buildSignals();
      loadSignals();
      break;
    case "learn":
      buildLearn();
      poll("learn", 1000, loadLearn);
      if (!S.signals) loadSignals();
      break;
    case "automations":
      buildAutomations();
      loadGraph();
      if (!S.signals) loadSignals();
      if (S.has.gpio && !S.gpio) loadGpio();
      if (!S.config) loadConfig();
      break;
    case "settings":
      buildSettings();
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
    renderSignals();
    renderQuickSignals();
    return S.signals;
  }).catch(function (e) {
    if (S.signals === null) S.signals = [];
    renderSignals(e);
    return [];
  });
}

function loadGraph() {
  return api("/api/graph").then(function (res) {
    S.graph = { nodes: res.nodes || [], links: res.links || [] };
    renderGraph();
    return S.graph;
  }).catch(function (e) {
    renderGraph(e);
  });
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

function loadLearn() {
  return api("/api/learn").then(function (res) {
    S.learn = res;
    renderLearn();
  }).catch(function (e) {
    S.learn = null;
    renderLearn(e);
  });
}

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

function transmit(signalId, btn, msgNode) {
  if (!txAvailable()) { if (msgNode) setMsg(msgNode, S.txBlock, "err"); return Promise.resolve(); }
  var old = btn ? btn.textContent : null;
  if (btn) { btn.disabled = true; btn.textContent = "Sending…"; }
  if (msgNode) setMsg(msgNode, "");
  return postJSON("/api/signals/" + signalId + "/transmit", {}).then(function () {
    if (btn) { btn.textContent = "Sent ✓"; setTimeout(function () { btn.textContent = old; btn.disabled = false; }, 1400); }
    if (msgNode) setMsg(msgNode, "Transmitted. This only confirms the pulses left the radio -- it cannot know a receiver reacted.", "ok");
  }).catch(function (e) {
    if (btn) { btn.textContent = old; btn.disabled = false; }
    if (e.status === 503 || e.status === 409) {
      S.txBlock = e.message || "The radio is unavailable, so nothing can be transmitted.";
      renderQuickSignals();
      renderSignals();
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
   DASHBOARD
   ====================================================================== */

var dashEls = {};

function buildDashboard() {
  if (S.built.dashboard) return;
  S.built.dashboard = true;
  var root = clear($("#tab-dashboard"));

  /* --- status + call to action --- */
  var p1 = el("div", "panel");
  dashEls.statusRow = el("div", "chiprow");
  add(p1, dashEls.statusRow);
  dashEls.statusNote = el("div");
  add(p1, dashEls.statusNote);
  var cta = el("div", "btnrow");
  cta.style.marginTop = ".7rem";
  var learnBtn = el("button", "btn primary", "➕ Learn a button");
  learnBtn.type = "button";
  learnBtn.addEventListener("click", function () { selectTab("learn"); armLearn(60); });
  var diagBtn = el("button", "btn", "Diagnostics");
  diagBtn.type = "button";
  diagBtn.addEventListener("click", function () { selectTab("diagnostics"); });
  add(cta, learnBtn, diagBtn);
  add(p1, cta);
  add(root, p1);

  /* --- live event feed --- */
  var p2 = el("div", "panel");
  var h2 = el("div", "panel-head");
  add(h2, el("h2", null, "Live activity"));
  add(h2, el("p", null,
    "Everything the receiver hears, as it happens. The radio listens continuously; " +
    "learn mode only decides what to do with a signal it does not recognise."));
  add(p2, h2);
  dashEls.feed = el("ul", "feed");
  add(p2, dashEls.feed);
  dashEls.feedEmpty = el("div", "empty", "Nothing heard yet. Press a doorbell button.");
  add(p2, dashEls.feedEmpty);
  add(root, p2);

  /* --- quick transmit --- */
  var p3 = el("div", "panel");
  var h3 = el("div", "panel-head");
  add(h3, el("h2", null, "Stored signals"));
  add(h3, el("p", null, "Tap Transmit to replay a signal on 433 MHz right now."));
  add(p3, h3);
  dashEls.txNote = el("div");
  add(p3, dashEls.txNote);
  dashEls.quick = el("div", "cards");
  add(p3, dashEls.quick);
  dashEls.quickEmpty = el("div", "empty",
    "No signals stored yet. Use “Learn a button” above to add the first one.");
  add(p3, dashEls.quickEmpty);
  add(root, p3);

  renderDashStatus();
  renderFeed();
  renderQuickSignals();
}

function tickDashClock() {
  if (!S.built.dashboard) return;
  renderDashStatus();
  /* refresh only the age column of the feed -- cheap, no re-render */
  $$(".fi-age", dashEls.feed).forEach(function (n) {
    var ts = parseFloat(n.dataset.ts);
    if (isFinite(ts)) n.textContent = agoText(ts);
  });
}

function renderDashStatus() {
  if (!dashEls.statusRow) return;
  var row = clear(dashEls.statusRow);
  var sys = S.sys;

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

function renderQuickSignals() {
  if (!dashEls.quick) return;
  var box = clear(dashEls.quick);
  var list = S.signals || [];
  dashEls.quickEmpty.classList.toggle("hidden", list.length > 0);
  clear(dashEls.txNote);
  var warn = txBlockNote();
  if (warn) add(dashEls.txNote, warn);

  list.forEach(function (sig) {
    var c = el("div", "card");
    var head = el("div", "card-head");
    add(head, el("div", "card-title", sig.name || ("Signal " + sig.id)));
    add(c, head);
    var chips = el("div", "chiprow");
    if (sig.decoded && sig.decoded.text) add(chips, el("span", "chip mono", sig.decoded.text));
    else add(chips, el("span", "chip warn", "raw waveform"));
    if (sig.origin) add(chips, el("span", "chip", sig.origin));
    if (typeof sig.seen_count === "number") add(chips, el("span", "chip", "seen " + sig.seen_count + "x"));
    add(c, chips);
    var msg = el("div", "formmsg");
    var row = el("div", "btnrow");
    var tx = el("button", "btn primary", "📡 Transmit");
    tx.type = "button";
    tx.disabled = !txAvailable();
    if (!txAvailable()) tx.title = S.txBlock;
    tx.addEventListener("click", function () { transmit(sig.id, tx, msg); });
    var det = el("button", "btn", "Details");
    det.type = "button";
    det.addEventListener("click", function () { openSignalSheet(sig.id); });
    add(row, tx, det);
    add(c, row, msg);
    add(box, c);
  });
}

/* ======================================================================
   SIGNALS
   ====================================================================== */

var sigEls = {};

function buildSignals() {
  if (S.built.signals) return;
  S.built.signals = true;
  var root = clear($("#tab-signals"));

  var p = el("div", "panel");
  var h = el("div", "panel-head");
  add(h, el("h2", null, "Signals"));
  add(h, el("p", null,
    "Every waveform this box knows: captured from a remote, or synthesized here. " +
    "Tap one to see how it decoded, look at its pulse train, rename it, replay it or delete it."));
  add(p, h);
  sigEls.txNote = el("div");
  add(p, sigEls.txNote);
  sigEls.list = el("ul", "list");
  add(p, sigEls.list);
  sigEls.empty = el("div", "empty",
    "No signals yet. Go to Learn and press your doorbell button, or create a virtual signal below.");
  add(p, sigEls.empty);
  sigEls.err = el("div", "note bad hidden");
  add(p, sigEls.err);
  add(root, p);

  /* --- virtual signal --- */
  var vp = el("div", "panel");
  var vh = el("div", "panel-head");
  add(vh, el("h2", null, "Create a virtual signal"));
  add(vh, el("p", null,
    "A virtual signal is a brand-new EV1527 code that no remote in the world is using yet. " +
    "It exists so you can pair YOUR OWN receivers to this box: put a plug-in chime, a relay " +
    "or a socket into its learning mode, then transmit this signal. From then on the receiver " +
    "obeys this box, and any node in Automations can ring it."));
  add(vp, vh);
  var grid = el("div", "formgrid");
  var vName = inputEl("text", "Virtual chime 1", { maxlength: "40", placeholder: "Virtual chime 1" });
  var vBtn = selectEl([1, 2, 4, 8].map(function (b) { return { value: b, label: "Button " + b }; }), 8);
  var vBase = inputEl("number", "350", { min: "100", max: "1500", step: "10", inputmode: "numeric" });
  var vId = inputEl("number", "", { min: "0", max: "1048575", step: "1", inputmode: "numeric", placeholder: "random" });
  add(grid,
    field("Name", vName, null, "full"),
    field("Button code", vBtn, "The 4-bit button nibble sent with the address."),
    field("Base pulse (us)", vBase, "350 us suits most EV1527 receivers."),
    field("20-bit address", vId, "Leave empty for a random address -- that is what you want.", "full"));
  add(vp, grid);
  var vFoot = el("div", "formfoot");
  var vSave = el("button", "btn primary", "Create virtual signal");
  vSave.type = "button";
  var vMsg = el("div", "formmsg");
  add(vFoot, vSave, vMsg);
  add(vp, vFoot);
  add(vp, el("div", "note",
    "After creating it: put your receiver into pairing mode, open the new signal and tap " +
    "Transmit a few times. The receiver stores the code and will answer to it from then on."));
  vSave.addEventListener("click", function () {
    var body = {
      name: trimOf(vName) || "Virtual signal",
      button: intOf(vBtn, 8),
      base_us: intOf(vBase, 350)
    };
    var idv = parseInt(vId.value, 10);
    body.id20 = isFinite(idv) ? idv : 0;
    vSave.disabled = true;
    setMsg(vMsg, "Creating…");
    postJSON("/api/signals/virtual", body).then(function (sig) {
      vSave.disabled = false;
      setMsg(vMsg, "Created. Transmit it while your receiver is in pairing mode.", "ok");
      loadSignals().then(function () { if (sig && sig.id) openSignalSheet(sig.id); });
    }).catch(function (e) {
      vSave.disabled = false;
      setMsg(vMsg, e.message, "err");
    });
  });
  add(root, vp);

  renderSignals();
}

function renderSignals(err) {
  if (!sigEls.list) return;
  var ul = clear(sigEls.list);
  clear(sigEls.txNote);
  var warn = txBlockNote();
  if (warn) add(sigEls.txNote, warn);

  if (err) {
    sigEls.err.classList.remove("hidden");
    sigEls.err.textContent = "Could not read the signal store: " + err.message;
  } else {
    sigEls.err.classList.add("hidden");
  }
  var list = S.signals || [];
  sigEls.empty.classList.toggle("hidden", list.length > 0 || !!err);

  list.forEach(function (sig) {
    var li = el("li");
    var row = el("div", "row");
    row.style.gap = ".4rem";
    var b = el("button", "listitem"); b.type = "button";
    b.style.flex = "1 1 12rem";
    add(b, el("span", "li-ico", sig.origin === "synthesized" ? "✨" : "📥"));
    var main = el("div", "li-main");
    add(main, el("div", "li-title", sig.name || ("Signal " + sig.id)));
    var subParts = [];
    if (sig.decoded && sig.decoded.text) subParts.push(sig.decoded.text);
    else subParts.push("raw waveform, " + numOr(sig.pulse_count, 0) + " pulses");
    if (typeof sig.base_us === "number") subParts.push(sig.base_us + " us base");
    add(main, el("div", "li-sub", subParts.join("  ·  ")));
    add(b, main);
    var meta = el("div", "li-meta");
    if (typeof sig.seen_count === "number") add(meta, el("div", null, sig.seen_count + "x"));
    if (typeof sig.last_seen_s === "number") add(meta, el("div", null, agoText(sig.last_seen_s)));
    add(b, meta);
    b.addEventListener("click", function () { openSignalSheet(sig.id); });
    add(row, b);

    var tx = el("button", "btn small", "📡");
    tx.type = "button";
    tx.setAttribute("aria-label", "Transmit " + (sig.name || sig.id));
    tx.style.minHeight = "2.75rem";
    tx.style.minWidth = "3rem";
    tx.disabled = !txAvailable();
    if (!txAvailable()) tx.title = S.txBlock;
    tx.addEventListener("click", function () { transmit(sig.id, tx, null); });
    add(row, tx);
    add(li, row);
    add(ul, li);
  });
}

function openSignalSheet(id) {
  var sh = openSheet("Signal", null);
  add(sh.body, el("div", "empty", "Loading…"));
  api("/api/signals/" + id).then(function (sig) {
    clear(sh.body);
    $("h3", sh.sheet).textContent = sig.name || ("Signal " + sig.id);

    /* identity */
    var chips = el("div", "chiprow");
    if (sig.decoded && sig.decoded.text) add(chips, el("span", "chip accent mono", sig.decoded.text));
    else add(chips, el("span", "chip warn", "Unknown protocol — stored as raw pulses"));
    if (sig.origin) add(chips, el("span", "chip", sig.origin));
    add(sh.body, chips);

    if (!sig.decoded) {
      add(sh.body, el("div", "note",
        "No decoder claimed this waveform. That is a fully supported state: the exact pulse " +
        "timings are stored and replay works normally. Only the human-readable identity is missing."));
    }

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
      add(sh.body, cwrap);
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
    add(sh.body, kv);

    /* waveform */
    var wf = waveform(sig.durations_us, sig.first_level);
    if (wf) {
      var wh = el("div", "field");
      add(wh, el("span", null, "Pulse train"));
      add(wh, wf);
      add(wh, el("span", "hint",
        "High = carrier on, low = carrier off. This is what gets replayed, verbatim, on transmit."));
      add(sh.body, wh);
    }

    /* rename */
    var nameIn = inputEl("text", sig.name || "", { maxlength: "40" });
    add(sh.body, field("Name", nameIn));

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
        setMsg(msg, "Renamed.", "ok");
        $("h3", sh.sheet).textContent = n;
        loadSignals();
        if (S.graph) loadGraph();
      }).catch(function (e) { save.disabled = false; setMsg(msg, e.message, "err"); });
    });
    var delb = el("button", "btn danger", "Delete");
    delb.type = "button";
    delb.addEventListener("click", function () {
      confirmSheet("Delete “" + (sig.name || sig.id) + "”?",
        ["The stored waveform is removed permanently.",
         "Any automation node pointing at it stops working until you point it somewhere else."],
        "Delete", true).then(function (ok) {
        if (!ok) return;
        setMsg(msg, "Deleting…");
        api("/api/signals/" + sig.id, { method: "DELETE" }).then(function () {
          sh.close();
          loadSignals();
          if (S.graph) loadGraph();
        }).catch(function (e) { setMsg(msg, e.message, "err"); });
      });
    });
    add(foot, txb, save, delb, msg);
    add(sh.body, foot);
  }).catch(function (e) {
    clear(sh.body);
    add(sh.body, el("div", "note bad", "Could not load this signal: " + e.message));
  });
}

/* ======================================================================
   LEARN
   ====================================================================== */

var learnEls = {};

function buildLearn() {
  if (S.built.learn) return;
  S.built.learn = true;
  var root = clear($("#tab-learn"));

  var p = el("div", "panel");
  var h = el("div", "panel-head");
  add(h, el("h2", null, "Learn a button"));
  add(h, el("p", null,
    "Arm learn mode, then press the button on your remote a few times."));
  add(p, h);

  learnEls.count = el("div", "countdown idle", "—");
  add(p, learnEls.count);
  learnEls.state = el("div", "listening");
  add(p, learnEls.state);

  var ctl = el("div", "btnrow");
  ctl.style.marginTop = ".8rem";
  learnEls.timeout = selectEl([
    { value: 30, label: "30 seconds" },
    { value: 60, label: "1 minute" },
    { value: 120, label: "2 minutes" },
    { value: 300, label: "5 minutes" }
  ], 60);
  learnEls.arm = el("button", "btn primary", "Arm learn mode");
  learnEls.arm.type = "button";
  learnEls.arm.addEventListener("click", function () { armLearn(intOf(learnEls.timeout, 60)); });
  learnEls.cancel = el("button", "btn", "Cancel");
  learnEls.cancel.type = "button";
  learnEls.cancel.addEventListener("click", function () {
    learnEls.cancel.disabled = true;
    postJSON("/api/learn/cancel", {}).then(loadLearn)
      .catch(function (e) { setMsg(learnEls.msg, e.message, "err"); })
      .then(function () { learnEls.cancel.disabled = false; });
  });
  add(ctl, learnEls.arm, learnEls.cancel);
  add(p, field("Stay armed for", learnEls.timeout,
    "Learn mode always expires on its own, so the box never sits armed forever."));
  add(p, ctl);
  learnEls.msg = el("div", "formmsg");
  add(p, learnEls.msg);
  add(root, p);

  /* candidate */
  learnEls.candPanel = el("div", "panel hidden");
  add(root, learnEls.candPanel);

  /* explanation -- this is the single most misunderstood part of the box */
  var ep = el("div", "panel");
  var eh = el("div", "panel-head");
  add(eh, el("h2", null, "What learn mode actually does"));
  add(ep, eh);
  add(ep, el("p", "small",
    "The receiver is ALWAYS listening. It has to be: a button you already registered must " +
    "ring the moment it is pressed, so there is no on-demand receive mode to switch on."));
  add(ep, el("p", "small",
    "Learn mode changes exactly one thing — the fate of a signal the box does NOT recognise. " +
    "Normally such a signal is dropped with one line in the activity log. While armed, it is " +
    "offered to you here for registration instead. Signals you already know behave identically " +
    "either way."));
  add(ep, el("div", "note",
    "A candidate must repeat at least twice and decode with at least 65% confidence before it " +
    "is offered. Both thresholds come from bench measurements: a real remote always sends " +
    "several copies (67-92% confidence), while band noise scores 24-48% and rarely repeats. " +
    "Without those filters the box would happily register the amplifier's own noise as a doorbell."));
  add(root, ep);

  renderLearn();
}

function armLearn(timeout) {
  buildLearn();
  if (learnEls.arm) learnEls.arm.disabled = true;
  setMsg(learnEls.msg, "Arming…");
  postJSON("/api/learn/arm", { timeout_s: timeout }).then(function () {
    setMsg(learnEls.msg, "Armed. Press your remote button now — several times.", "ok");
    poll("learn", 1000, loadLearn);
  }).catch(function (e) {
    setMsg(learnEls.msg, e.message, "err");
  }).then(function () {
    if (learnEls.arm) learnEls.arm.disabled = false;
  });
}

function renderLearn(err) {
  if (!learnEls.count) return;
  var L = S.learn;

  if (err) {
    learnEls.count.textContent = "—";
    learnEls.count.className = "countdown idle";
    clear(learnEls.state);
    add(learnEls.state, el("span", null, "Learn mode is not available: " + err.message));
    learnEls.arm.disabled = true;
    learnEls.cancel.disabled = true;
    learnEls.candPanel.classList.add("hidden");
    return;
  }
  learnEls.arm.disabled = false;

  var active = !!(L && L.active);
  learnEls.arm.classList.toggle("hidden", active);
  learnEls.cancel.classList.toggle("hidden", !active);

  if (active) {
    var rem = numOr(L.remaining_s, 0);
    learnEls.count.textContent = Math.floor(rem / 60) + ":" + ("0" + (rem % 60)).slice(-2);
    learnEls.count.className = "countdown";
    clear(learnEls.state);
    add(learnEls.state, el("span", "pulse"), el("span", null,
      L.candidate ? "Candidate captured — review it below." : "Armed — press your remote button"));
  } else {
    learnEls.count.textContent = "—";
    learnEls.count.className = "countdown idle";
    clear(learnEls.state);
    add(learnEls.state, el("span", null,
      "Not armed. The receiver is still listening — known buttons work as usual."));
    /* Deliberately NOT re-scheduling the poll from here: poll() fires its
       function immediately, so calling it from inside a render that was itself
       triggered by that poll would recurse. The 1 s timer set on tab entry is
       the only scheduler. */
  }

  renderCandidate(L && L.candidate);
}

function renderCandidate(c) {
  var p = learnEls.candPanel;
  if (!p) return;
  if (!c) { p.classList.add("hidden"); clear(p); return; }
  /* Do not rebuild while the user is typing the name. */
  if (p.dataset.fp === (c.fingerprint || "") && $(".cand-name", p) === document.activeElement) return;
  p.dataset.fp = c.fingerprint || "";
  p.classList.remove("hidden");
  clear(p);

  var h = el("div", "panel-head");
  add(h, el("h2", null, "New button detected"));
  add(h, el("p", null, "Give it a name and accept it, or ignore it and press a different button."));
  add(p, h);

  var chips = el("div", "chiprow");
  if (c.decoded && c.decoded.text) add(chips, el("span", "chip accent mono", c.decoded.text));
  else add(chips, el("span", "chip warn", "Unknown protocol — will be stored as raw pulses"));
  if (typeof c.repeats === "number") add(chips, el("span", "chip ok", c.repeats + " repeats"));
  if (typeof c.rssi_dbm === "number") {
    add(chips, el("span", "chip " + (c.rssi_dbm > -60 ? "ok" : "warn"), c.rssi_dbm + " dBm"));
  }
  add(p, chips);

  if (typeof c.confidence === "number") {
    var m = el("div", "meter " + (c.confidence >= 65 ? "ok" : "warn"));
    var f = el("i");
    f.style.width = Math.max(2, Math.min(100, c.confidence)) + "%";
    add(m, f);
    var wrap = el("div");
    wrap.style.margin = ".6rem 0";
    var r = el("div", "row");
    r.style.justifyContent = "space-between";
    add(r, el("span", "small muted", "Confidence"), el("span", "small mono", c.confidence + "%"));
    add(wrap, r, m);
    add(p, wrap);
  }

  var kv = el("dl", "kv");
  if (typeof c.base_us === "number") { add(kv, el("dt", null, "Base pulse")); add(kv, el("dd", "mono", c.base_us + " us")); }
  if (typeof c.pulse_count === "number") { add(kv, el("dt", null, "Pulses")); add(kv, el("dd", "mono", String(c.pulse_count))); }
  if (c.fingerprint) { add(kv, el("dt", null, "Fingerprint")); add(kv, el("dd", "mono", c.fingerprint)); }
  add(p, kv);

  var nameIn = inputEl("text", (c.decoded && c.decoded.text) ? "" : "", { maxlength: "40", placeholder: "e.g. Front door" });
  nameIn.className = "cand-name";
  add(p, field("Name this button", nameIn, "Shown in the activity feed and in Automations."));

  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");
  var acc = el("button", "btn primary", "Accept and save");
  acc.type = "button";
  acc.addEventListener("click", function () {
    var n = trimOf(nameIn);
    if (!n) { setMsg(msg, "Give the button a name first.", "err"); nameIn.focus(); return; }
    acc.disabled = true;
    setMsg(msg, "Saving…");
    postJSON("/api/learn/accept", { name: n }).then(function () {
      setMsg(msg, "Saved. Learn mode is now off.", "ok");
      p.classList.add("hidden");
      loadSignals();
      loadLearn();
      if (S.graph) loadGraph();
    }).catch(function (e) {
      acc.disabled = false;
      setMsg(msg, e.message, "err");
    });
  });
  add(foot, acc, msg);
  add(p, foot);
  if (!nameIn.value) setTimeout(function () { nameIn.focus(); }, 40);
}

/* ======================================================================
   AUTOMATIONS -- the node graph
   ====================================================================== */

var NODE_TYPES = [
  { t: "source.button", g: "source", label: "433 MHz button", ico: "🔘",
    help: "Fires when a learned remote button is pressed." },
  { t: "source.gpio", g: "source", label: "Wired button", ico: "🔌",
    help: "Fires when a button wired to a GPIO pin is pressed. Optional." },
  { t: "source.virtual", g: "source", label: "Virtual trigger", ico: "✨",
    help: "Fires from this page, from the REST API, or from an MQTT topic." },
  { t: "source.any_rf", g: "source", label: "Any RF signal", ico: "📻",
    help: "Wildcard: fires on EVERY burst the receiver hears, registered or not." },
  { t: "logic.group", g: "logic", label: "Group", ico: "🔗",
    help: "Passes on when ANY or ALL of its inputs fire inside a time window." },
  { t: "logic.throttle", g: "logic", label: "Rate limit", ico: "⏱",
    help: "Passes the first press, then ignores everything for a cooldown you set. " +
          "Someone can lean on the button — the bell still rings once." },
  { t: "sink.transmit", g: "sink", label: "Transmit", ico: "📡",
    help: "Transmits a stored signal on 433 MHz." },
  { t: "sink.mqtt", g: "sink", label: "MQTT publish", ico: "📨",
    help: "Publishes to your broker / fires a Home Assistant device trigger." }
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
/* Ports: a source has no input, a sink has no output. */
function hasInput(n) { return nodeType(n.type).g !== "source"; }
function hasOutput(n) { return nodeType(n.type).g !== "sink"; }
function linkExists(from, to) {
  var ls = (S.graph && S.graph.links) || [];
  for (var i = 0; i < ls.length; i++) if (ls[i].from === from && ls[i].to === to) return true;
  return false;
}

var autoEls = {};

function buildAutomations() {
  if (S.built.automations) return;
  S.built.automations = true;
  var root = clear($("#tab-automations"));

  var p = el("div", "panel");
  var h = el("div", "panel-head");
  add(h, el("h2", null, "Automations"));
  add(h, el("p", null,
    "A press travels left to right: a SOURCE hears it, optional LOGIC decides whether it " +
    "passes, and a SINK does something — transmit a signal, publish to MQTT. " +
    "Nodes are linked by tapping, never by dragging."));
  add(h, el("p", null,
    "Two sources are worth knowing about: “Any RF signal” is a wildcard that fires on every " +
    "burst on the band, and a “Group” lets several buttons drive one action."));
  add(p, h);

  var topRow = el("div", "row");
  var addBtn = el("button", "btn primary", "➕ Add node");
  addBtn.type = "button";
  addBtn.addEventListener("click", openAddNode);
  add(topRow, addBtn);

  /* View switch: hidden below 900 px by CSS -- the list is the whole product
     on a phone, and the canvas is strictly additive. */
  autoEls.viewSwitch = el("div", "segmented graph-viewswitch");
  var bList = el("button", null, "List"); bList.type = "button"; bList.classList.add("active");
  var bMap = el("button", null, "Map"); bMap.type = "button";
  bList.addEventListener("click", function () { setGraphView("list", bList, bMap); });
  bMap.addEventListener("click", function () { setGraphView("map", bList, bMap); });
  add(autoEls.viewSwitch, bList, bMap);
  add(topRow, autoEls.viewSwitch);
  add(p, topRow);
  add(root, p);

  autoEls.listWrap = el("div", "stack");
  add(root, autoEls.listWrap);
  autoEls.canvasWrap = el("div", "panel hidden");
  add(root, autoEls.canvasWrap);
  autoEls.empty = el("div", "empty",
    "No nodes yet. Add a “433 MHz button” source and a “Transmit” sink, then link them.");
  add(root, autoEls.empty);

  /* Recipes: the group pattern is the least obvious capability here, and it is
     precisely what "make several buttons ring one chime" needs. */
  var rp = el("div", "panel");
  var rh = el("div", "panel-head");
  add(rh, el("h2", null, "Recipes"));
  add(rh, el("p", null, "Patterns that need no special node type — just links."));
  add(rp, rh);
  [
    ["Repeat a doorbell to a second chime",
     "433 MHz button (Front door) → Transmit (Virtual chime 1)"],
    ["Several buttons, one chime",
     "433 MHz button (Front) + 433 MHz button (Back) → Group (mode: any) → Transmit (Virtual chime 1). " +
     "This is how you fold several remotes into a single virtual signal — no special node needed, " +
     "just two sources into one group."],
    ["Two buttons pressed together",
     "Same as above but set the Group to mode ALL and give it a window (e.g. 3000 ms)."],
    ["Stop a stuck button ringing forever",
     "433 MHz button → Rate limit (10 s cooldown) → Transmit."],
    ["Ring the chime from Home Assistant",
     "Virtual trigger (topic: front_gate) → Transmit. Publish anything to the trigger topic and it fires."],
    ["Tell Home Assistant someone rang",
     "433 MHz button → MQTT publish (topic: front)."],
    ["Proxy the whole band to Home Assistant",
     "Any RF signal → MQTT publish. Every press within range reaches HA, including buttons you " +
     "never registered. Add a Rate limit in the middle if the band is busy."]
  ].forEach(function (r) {
    var b = el("div");
    b.style.marginBottom = ".6rem";
    add(b, el("div", null, r[0]));
    add(b, el("div", "hint mono", r[1]));
    add(rp, b);
  });
  add(root, rp);

  renderGraph();
}

function setGraphView(v, bList, bMap) {
  S.graphView = v;
  bList.classList.toggle("active", v === "list");
  bMap.classList.toggle("active", v === "map");
  autoEls.listWrap.classList.toggle("hidden", v !== "list");
  autoEls.canvasWrap.classList.toggle("hidden", v !== "map");
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

  /* Order sources, then logic, then sinks: that is the direction of flow, and
     on a phone the reading order IS the diagram. */
  var order = { source: 0, logic: 1, sink: 2 };
  nodes.slice().sort(function (a, b) {
    var d = order[nodeType(a.type).g] - order[nodeType(b.type).g];
    return d !== 0 ? d : (a.id - b.id);
  }).forEach(function (n) {
    add(wrap, nodeCard(n, links));
  });

  if (S.graphView === "map") renderCanvas();
}

function noteRow(text) {
  var d = el("div", "hint");
  d.textContent = text;
  return d;
}

function nodeSummary(n) {
  switch (n.type) {
    case "source.button": {
      var sn = signalName(n.signal_id);
      return sn ? ("Listens for: " + sn)
                : (n.signal_id ? "Signal " + n.signal_id + " (missing from the store)" : "No signal chosen yet");
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
    case "logic.group":
      return "Mode " + (n.group_mode === "all" ? "ALL" : "ANY") + " · window " + numOr(n.window_s, 1) + " s";
    case "logic.throttle":
      return "Rings once, then ignores presses for " + numOr(n.window_s, 10) + " s";
    case "sink.transmit": {
      var tn = signalName(n.signal_id);
      return (tn ? "Transmits: " + tn : (n.signal_id ? "Signal " + n.signal_id + " (missing)" : "No signal chosen yet")) +
        " · " + numOr(n.repeats, 6) + "x, " + numOr(n.gap_us, 8000) + " us gap";
    }
    case "sink.mqtt":
      return n.topic ? ("Publishes to " + mqttPublishTopic(n.topic)) : "No topic set";
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

function nodeCard(n, links) {
  var ty = nodeType(n.type);
  var c = el("div", "card nodecard g-" + ty.g + (n.enabled === false ? " off" : ""));

  var head = el("div", "card-head");
  add(head, el("span", "li-ico", ty.ico));
  var main = el("div", "card-title");
  add(main, document.createTextNode(n.name || ty.label));
  var wrapT = el("div");
  wrapT.style.flex = "1";
  wrapT.style.minWidth = "0";
  add(wrapT, main, el("div", "node-type", ty.label));
  add(head, wrapT);
  if (n.enabled === false) add(head, el("span", "chip warn", "disabled"));
  add(c, head);

  add(c, el("div", "card-sub", nodeSummary(n)));

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
  var fire = el("button", "btn", n.type === "source.virtual" ? "▶ Trigger" : "Test fire");
  fire.type = "button";
  fire.addEventListener("click", function () {
    fire.disabled = true;
    setMsg(msg, "Firing…");
    postJSON("/api/graph/nodes/" + n.id + "/fire", {}).then(function () {
      setMsg(msg, "Fired. Watch the Dashboard feed for what it triggered.", "ok");
    }).catch(function (e) { setMsg(msg, e.message, "err"); })
      .then(function () { fire.disabled = false; });
  });
  var del = el("button", "btn danger", "Delete");
  del.type = "button";
  del.addEventListener("click", function () {
    confirmSheet("Delete “" + (n.name || ty.label) + "”?",
      ["The node and every link to or from it are removed.",
       "Stored signals are not touched."], "Delete", true).then(function (ok) {
      if (!ok) return;
      api("/api/graph/nodes/" + n.id, { method: "DELETE" })
        .then(loadGraph)
        .catch(function (e) { setMsg(msg, e.message, "err"); });
    });
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

function nextPosition(group) {
  var nodes = (S.graph && S.graph.nodes) || [];
  var col = { source: 40, logic: 260, sink: 480 }[group] || 260;
  var count = nodes.filter(function (n) { return nodeType(n.type).g === group; }).length;
  return { x: col, y: 30 + count * 100 };
}

function openAddNode() {
  var items = NODE_TYPES.filter(function (t) {
    return !(t.t === "source.gpio" && !S.has.gpio);
  }).map(function (t) {
    return {
      value: t.t, icon: t.ico, label: t.label, sub: t.help,
      meta: t.g === "source" ? "source" : t.g === "sink" ? "sink" : "logic"
    };
  });
  pickerSheet("Add a node", "Sources hear things, logic filters, sinks act.", items, function (type) {
    var ty = nodeType(type);
    var pos = nextPosition(ty.g);
    var body = {
      type: type, name: ty.label, enabled: true,
      signal_id: 0, gpio_pin: -1, gpio_active_low: true, gpio_debounce_ms: 50,
      repeats: 6, gap_us: 8000, window_s: 10, group_mode: "any",
      topic: "", ui_x: pos.x, ui_y: pos.y
    };
    postJSON("/api/graph/nodes", body).then(function (created) {
      return loadGraph().then(function () {
        var n = created && created.id ? nodeById(created.id) : null;
        if (n) openNodeEditor(n);
      });
    }).catch(function (e) { alertSheet("Could not add the node", e.message); });
  });
}

function openNodeEditor(n) {
  var ty = nodeType(n.type);
  var sh = openSheet(n.name || ty.label, ty.help);
  var patch = {};   /* only what the user touched is sent */

  var nameIn = inputEl("text", n.name || "", { maxlength: "40" });
  add(sh.body, field("Name", nameIn));

  var enabled = checkField("Enabled", n.enabled !== false,
    "A disabled node stays in the graph but never fires.");
  add(sh.body, enabled);

  var grid = el("div", "formgrid");
  add(sh.body, grid);

  var ctl = {};   /* type-specific controls */

  if (n.type === "source.button" || n.type === "sink.transmit") {
    var sigs = (S.signals || []).map(function (s) {
      return { value: s.id, label: (s.name || ("Signal " + s.id)) +
        (s.decoded && s.decoded.text ? "  —  " + s.decoded.text : "") };
    });
    sigs.unshift({ value: 0, label: "— choose a signal —" });
    ctl.signal = selectEl(sigs, numOr(n.signal_id, 0));
    add(grid, field(n.type === "source.button" ? "Listen for this signal" : "Signal to transmit",
      ctl.signal,
      (S.signals && S.signals.length) ? null : "No signals stored yet — learn a button first.",
      "full"));
  }

  if (n.type === "sink.transmit") {
    ctl.repeats = inputEl("number", numOr(n.repeats, 6), { min: "1", max: "32", step: "1", inputmode: "numeric" });
    ctl.gap = inputEl("number", numOr(n.gap_us, 8000), { min: "500", max: "60000", step: "500", inputmode: "numeric" });
    add(grid, field("Repeats", ctl.repeats, "Many cheap receivers need several identical copies before they act."));
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
      "It fires IN ADDITION to any matching button node — a registered press drives both its " +
      "own chain and this wildcard chain. That is intended, not double-firing."));
    add(sh.body, el("div", "note warn",
      "If the band around you is busy, put a Rate limit between this node and its sink. " +
      "A chatty neighbouring remote will otherwise spam your broker."));
  }

  if (n.type === "logic.group") {
    ctl.mode = selectEl([
      { value: "any", label: "ANY — pass on the first input that fires" },
      { value: "all", label: "ALL — pass on only when every input fired in the window" }
    ], n.group_mode === "all" ? "all" : "any");
    ctl.windowS = inputEl("number", numOr(n.window_s, 1), { min: "1", max: "6000", step: "1", inputmode: "numeric" });
    add(grid, field("Mode", ctl.mode, null, "full"));
    add(grid, field("Window (ms)", ctl.window, "How long inputs are remembered when matching ALL."));
    add(sh.body, el("div", "note",
      "This is how several remote buttons become one action: link every button node into this " +
      "group with mode ANY, then link the group to a Transmit sink."));
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

  var msg = el("div", "formmsg");
  var foot = el("div", "formfoot");
  var save = el("button", "btn primary", "Save");
  save.type = "button";
  save.addEventListener("click", function () {
    patch = { name: trimOf(nameIn) || nodeType(n.type).label, enabled: enabled.input.checked };
    if (ctl.signal) patch.signal_id = intOf(ctl.signal, 0);
    if (ctl.repeats) patch.repeats = intOf(ctl.repeats, 6);
    if (ctl.gap) patch.gap_us = intOf(ctl.gap, 8000);
    if (ctl.pin) patch.gpio_pin = intOf(ctl.pin, -1);
    if (ctl.activeLow) patch.gpio_active_low = ctl.activeLow.input.checked;
    if (ctl.debounce) patch.gpio_debounce_ms = intOf(ctl.debounce, 50);
    if (ctl.windowS) patch.window_s = intOf(ctl.windowS, 10);
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
  add(foot, save, cancel, msg);
  add(sh.body, foot);
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
    "Drag a node to rearrange it; tap it to edit. Everything here is also doable from the list view."));

  var NW = 168, NH = 52;
  var maxX = 0, maxY = 0;
  nodes.forEach(function (n) {
    maxX = Math.max(maxX, numOr(n.ui_x, 40) + NW);
    maxY = Math.max(maxY, numOr(n.ui_y, 40) + NH);
  });
  var VW = Math.max(700, maxX + 40), VH = Math.max(360, maxY + 40);

  var box = el("div", "canvas-wrap");
  var svg = svgEl("svg", "canvas");
  svg.setAttribute("viewBox", "0 0 " + VW + " " + VH);
  svg.setAttribute("preserveAspectRatio", "xMidYMid meet");
  var gLinks = svgEl("g");
  var gNodes = svgEl("g");
  add(svg, gLinks);
  add(svg, gNodes);

  function pos(n) { return { x: numOr(n.ui_x, 40), y: numOr(n.ui_y, 40) }; }

  function drawLinks() {
    clear(gLinks);
    links.forEach(function (l) {
      var a = nodeById(l.from), b = nodeById(l.to);
      if (!a || !b) return;
      var p1 = pos(a), p2 = pos(b);
      var x1 = p1.x + NW, y1 = p1.y + NH / 2;
      var x2 = p2.x, y2 = p2.y + NH / 2;
      var dx = Math.max(30, Math.abs(x2 - x1) / 2);
      var path = svgEl("path", "lnk");
      path.setAttribute("d", "M" + x1 + "," + y1 + " C" + (x1 + dx) + "," + y1 +
        " " + (x2 - dx) + "," + y2 + " " + x2 + "," + y2);
      add(gLinks, path);
    });
  }

  nodes.forEach(function (n) {
    var ty = nodeType(n.type);
    var p = pos(n);
    var g = svgEl("g", "node");
    g.setAttribute("transform", "translate(" + p.x + "," + p.y + ")");
    var r = svgEl("rect", "nbox g-" + ty.g);
    r.setAttribute("width", NW); r.setAttribute("height", NH);
    r.setAttribute("rx", "10");
    add(g, r);
    var t1 = svgEl("text", "ntitle");
    t1.setAttribute("x", "12"); t1.setAttribute("y", "22");
    t1.textContent = (n.name || ty.label).slice(0, 22);
    add(g, t1);
    var t2 = svgEl("text", "ntype");
    t2.setAttribute("x", "12"); t2.setAttribute("y", "38");
    t2.textContent = ty.ico + " " + ty.label;
    add(g, t2);
    add(gNodes, g);

    /* pointer drag -> persist ui_x/ui_y on release */
    var drag = null;
    g.addEventListener("pointerdown", function (ev) {
      var pt = svgPoint(svg, ev);
      drag = { sx: pt.x, sy: pt.y, ox: numOr(n.ui_x, 40), oy: numOr(n.ui_y, 40), moved: false };
      try { g.setPointerCapture(ev.pointerId); } catch (e) { /* ignore */ }
    });
    g.addEventListener("pointermove", function (ev) {
      if (!drag) return;
      var pt = svgPoint(svg, ev);
      var nx = Math.max(0, Math.round(drag.ox + pt.x - drag.sx));
      var ny = Math.max(0, Math.round(drag.oy + pt.y - drag.sy));
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
}

function svgPoint(svg, ev) {
  var r = svg.getBoundingClientRect();
  var vb = svg.viewBox.baseVal;
  var sx = vb.width / r.width, sy = vb.height / r.height;
  var s = Math.max(sx, sy);   /* matches xMidYMid meet */
  return {
    x: (ev.clientX - r.left - (r.width - vb.width / s) / 2) * s,
    y: (ev.clientY - r.top - (r.height - vb.height / s) / 2) * s
  };
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

function sectionFirmware() {
  var s = section("Firmware & web UI update",
    "Two separate images: the app, and this web UI. Updating one leaves the other alone.");
  var body = s.bodyEl;

  add(body, el("div", "note",
    "The app image and the web UI live in different partitions. After an app update the old " +
    "UI is still being served until you update it too — that is normal, not a failure."));

  /* --- from a URL --- */
  add(body, el("h3", null, "From a URL"));
  var urlIn = inputEl("url", "", { placeholder: "https://.../doorbell433.bin" });
  urlIn.type = "url";
  add(body, field("Image URL", urlIn, "The box downloads it itself, so it needs working internet."));
  var urlMsg = el("div", "formmsg");
  var urlRow = el("div", "btnrow");
  var appBtn = el("button", "btn primary", "Update firmware");
  appBtn.type = "button";
  var uiBtn = el("button", "btn", "Update web UI");
  uiBtn.type = "button";
  function urlUpdate(path, what, btn) {
    var u = trimOf(urlIn);
    if (!u) { setMsg(urlMsg, "Enter the image URL first.", "err"); return; }
    confirmSheet("Update the " + what + " from this URL?",
      [u, "The box downloads and flashes it, then reboots. Do not power it off."],
      "Update").then(function (ok) {
      if (!ok) return;
      btn.disabled = true;
      setMsg(urlMsg, "Downloading and flashing… this can take a minute.");
      postJSON(path, { url: u }).then(function () {
        setMsg(urlMsg, "Flashed. The box is rebooting — reload this page in a few seconds.", "ok");
      }).catch(function (e) { setMsg(urlMsg, e.message, "err"); })
        .then(function () { btn.disabled = false; });
    });
  }
  appBtn.addEventListener("click", function () { urlUpdate("/api/ota", "firmware", appBtn); });
  uiBtn.addEventListener("click", function () { urlUpdate("/api/ota/webui", "web UI", uiBtn); });
  add(urlRow, appBtn, uiBtn);
  add(body, urlRow, urlMsg);

  loadConfig().then(function (cfg) {
    if (cfg && cfg.ota && cfg.ota.url && !urlIn.value) urlIn.value = cfg.ota.url;
  });

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
  add(body, uploadRow("Firmware image (doorbell433.bin)", "/api/ota/upload", "firmware"));
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
