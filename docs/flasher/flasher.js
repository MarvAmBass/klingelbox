/* Klingelbox web flasher — flash a release image onto an ESP32-S3 over Web
 * Serial, straight from GitHub Pages. Vanilla ES modules, no build step.
 *
 * Design constraints, in order of importance:
 *
 *  - EVERYTHING IS SAME-ORIGIN. GitHub's release-asset downloads
 *    (objects.githubusercontent.com, and the /releases/download/... redirect
 *    that leads there) send no `Access-Control-Allow-Origin` header, so a
 *    browser page simply cannot fetch a release binary: the request is blocked
 *    by CORS before any bytes arrive, and there is nothing this page can do
 *    about it — it is a property of GitHub's asset host, not of the code here.
 *    That is why CI mirrors each release's images into ./firmware/<tag>/ on
 *    this very site (the `flasher-sync` workflow) and writes
 *    ./firmware/manifest.json. The page then fetches images from its own
 *    origin, which needs no CORS headers at all.
 *    api.github.com *does* send CORS headers, so the release list from there is
 *    usable — but it is used only for decoration (names, dates, notes links),
 *    never as a source of bytes.
 *
 *  - NOTHING IS WRITTEN BEFORE ITS SHA-256 MATCHES THE MANIFEST. A truncated or
 *    stale-cached image at offset 0x0 leaves a mute board, and whoever is using
 *    this page has by definition no serial console attached to find out why.
 *    Refusing to write is always the better failure.
 *
 *  - The esptool-js plumbing comes from the vendored bundle in ./lib/ (see
 *    lib/README.md for provenance and the Apache-2.0 licence). No CDN in the
 *    write path.
 *
 *  - NVS config pre-seeding is deliberately NOT implemented: the Klingelbox
 *    firmware has no seed importer (see firmware/main/db_config.h — the
 *    reference project's "leseed" import is explicitly out of scope), so a
 *    baked config partition would be silently ignored on boot. A feature that
 *    looks like it worked and did nothing is worse than no feature. First-boot
 *    configuration happens in the recovery portal instead.
 */

const REPO = "MarvAmBass/klingelbox";
const MANIFEST_URL = "firmware/manifest.json";
const FIRMWARE_BASE = "firmware/";
const BUNDLE_URL = "./lib/esptool-js.bundle.mjs";
const FLASH_BAUD = 921600;

/* The merged release image is laid out from the very start of flash by
 * esptool's merge_bin; it is never correct anywhere else. */
const FULL_IMAGE_ADDRESS = 0x0;

const $ = (id) => document.getElementById(id);

let releases = [];        // normalized manifest entries (may stay empty)
let selectedTag = null;   // version tag of the release picked in the list
let selectedVariant = 0;  // index into the selected release's images[]
let busy = false;         // one serial operation at a time

/* ------------------------------------------------------------------ log */

function log(line) {
  const el = $("log");
  const div = document.createElement("div");
  div.textContent = line;
  el.appendChild(div);
  el.scrollTop = el.scrollHeight;
}

function openLog() { $("log-drawer").open = true; }

/* ------------------------------------------------------- browser support */

/** null = Web Serial is present. A string = extra explanation to show on top
 * of the "use Chrome or Edge" banner text that lives in the HTML. */
function webSerialDiagnosis() {
  if (typeof navigator !== "undefined" && navigator.serial !== undefined) return null;
  if (!window.isSecureContext) {
    return "This page is not a secure context, so the browser hides the Web Serial API. " +
           "Open it over https:// or as http://localhost.";
  }
  return "";
}

function applySupportGate() {
  const reason = webSerialDiagnosis();
  if (reason === null) return true;
  $("unsupported-reason").textContent = reason ? " " + reason + " " : " ";
  $("unsupported").classList.remove("hidden");
  $("flash-btn").disabled = true;
  $("erase-arm").disabled = true;
  $("erase-btn").disabled = true;
  return false;
}

/* --------------------------------------------------------------- helpers */

function hex(n) { return "0x" + Number(n).toString(16); }

function fmtSize(bytes) {
  if (!Number.isFinite(bytes)) return "";
  if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB";
  return Math.round(bytes / 1024) + " KB";
}

function fmtDate(iso) {
  if (!iso) return "";
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? "" : d.toISOString().slice(0, 10);
}

/** "0x9000" or "36864" -> integer, or null when unparseable. */
function parseOffset(text) {
  const value = String(text || "").trim().toLowerCase();
  if (/^0x[0-9a-f]+$/.test(value)) return parseInt(value, 16);
  if (/^[0-9]+$/.test(value)) return parseInt(value, 10);
  return null;
}

async function sha256hex(bytes) {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return Array.from(new Uint8Array(digest))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

/* -------------------------------------------------------------- manifest */

/*
 * ./firmware/manifest.json is written by the `flasher-sync` CI job and is the
 * index of everything mirrored onto this site:
 *
 *   { "generated": "<iso8601>", "keep": 3,
 *     "releases": [                                  // newest first
 *       { "version": "v0.1.0", "date": "<iso8601>",
 *         "full": { "path": "v0.1.0/klingelbox-esp32s3-full.bin",
 *                   "size": 4194304, "sha256": "<64 hex>" },
 *         "notes_url": "https://github.com/.../releases/tag/v0.1.0",
 *         // OPTIONAL: several images per release (e.g. a 16 MB build and a
 *         // 4 MB ESP32-S3 Zero build). When present it replaces `full`.
 *         "variants": [ { "id": "16mb", "label": "ESP32-S3, 16 MB flash",
 *                         "path": "...", "size": 0, "sha256": "..." } ] } ] }
 *
 * `path` is relative to ./firmware/.
 *
 * A release entry MAY omit size/sha256 in the index and instead ship a per-tag
 * ./firmware/<tag>/manifest.json with the same `full`/`variants` shape (paths
 * then relative to ./firmware/<tag>/). That detail manifest is fetched lazily,
 * only for the release the operator actually selected, so a big mirror costs
 * one small index fetch on page load.
 */

/** One image, normalized: absolute-ish path relative to ./firmware/. */
function normalizeImage(raw, tag, base, fallbackLabel) {
  if (!raw || typeof raw.path !== "string" || !raw.path) return null;
  // Join without letting a stray leading slash escape the firmware directory.
  const rel = raw.path.replace(/^\/+/, "");
  return {
    id: raw.id || "full",
    label: raw.label || fallbackLabel || tag,
    path: base ? base.replace(/\/+$/, "") + "/" + rel : rel,
    size: Number.isFinite(raw.size) ? raw.size : null,
    sha256: typeof raw.sha256 === "string" ? raw.sha256.toLowerCase() : null,
    note: typeof raw.note === "string" ? raw.note : "",
  };
}

/** Pull the image list out of a manifest entry (index or per-tag detail). */
function imagesFrom(source, tag, base) {
  if (!source) return [];
  if (Array.isArray(source.variants) && source.variants.length) {
    return source.variants
      .map((v, i) => normalizeImage(v, tag, base, "Image " + (i + 1)))
      .filter(Boolean);
  }
  const one = normalizeImage(source.full, tag, base, "Full image");
  return one ? [one] : [];
}

function normalizeRelease(entry) {
  if (!entry || typeof entry.version !== "string" || !entry.version) return null;
  const tag = entry.version;
  return {
    version: tag,
    date: entry.date || null,
    notesUrl: entry.notes_url ||
              "https://github.com/" + REPO + "/releases/tag/" + encodeURIComponent(tag),
    images: imagesFrom(entry, tag, ""),
    // Set once the per-tag detail manifest has been merged in, so it is only
    // ever fetched once per release.
    detailLoaded: false,
  };
}

async function fetchManifest() {
  const response = await fetch(MANIFEST_URL, { cache: "no-store" });
  if (!response.ok) throw new Error("HTTP " + response.status);
  const data = await response.json();
  if (!data || !Array.isArray(data.releases)) {
    throw new Error("manifest carries no releases array");
  }
  return data.releases.map(normalizeRelease).filter(Boolean);
}

/** Fill in size/sha256 (and any variants) from ./firmware/<tag>/manifest.json
 * when the index left them out. Missing detail manifests are not an error
 * here — the "no usable sha256" check below is what actually stops a flash. */
async function loadReleaseDetail(release) {
  if (release.detailLoaded) return;
  release.detailLoaded = true;
  const needsDetail = !release.images.length ||
                      release.images.some((img) => !img.sha256 || img.size === null);
  if (!needsDetail) return;

  const url = FIRMWARE_BASE + encodeURIComponent(release.version) + "/manifest.json";
  try {
    const response = await fetch(url, { cache: "no-store" });
    if (!response.ok) return;
    const detail = await response.json();
    const images = imagesFrom(detail, release.version, release.version);
    if (images.length) {
      release.images = images;
      log("Loaded per-release manifest for " + release.version + ".");
    }
  } catch (err) {
    log("No per-release manifest for " + release.version + " (" +
        (err.message || err) + ") — using the index entry as-is.");
  }
}

/* ---------------------------------------------------- release list (UI) */

/** Release list from api.github.com. CORS-friendly, and strictly optional: it
 * only contributes notes links and "not yet synced" rows. Any failure (rate
 * limit, offline, private repo) leaves the manifest-driven UI fully working. */
async function fetchGithubReleases() {
  try {
    const response = await fetch(
      "https://api.github.com/repos/" + REPO + "/releases?per_page=15",
      { headers: { Accept: "application/vnd.github+json" } }
    );
    if (!response.ok) return [];
    const list = await response.json();
    return Array.isArray(list) ? list.filter((r) => !r.draft) : [];
  } catch {
    return [];
  }
}

function relRow({ name, date, size, notesUrl, tag, synced, checked }) {
  const row = document.createElement("div");
  row.className = "rel-row" + (synced ? "" : " unsynced");

  if (synced) {
    const radio = document.createElement("input");
    radio.type = "radio";
    radio.name = "rel";
    radio.value = tag;
    radio.id = "rel-" + tag;
    radio.checked = checked;
    if (checked) selectedTag = tag;
    radio.addEventListener("change", () => {
      selectedTag = tag;
      selectedVariant = 0;
      onReleaseSelected();
    });
    row.appendChild(radio);
  }

  const label = document.createElement("label");
  label.className = "inline";
  if (synced) label.htmlFor = "rel-" + tag;
  const nm = document.createElement("span");
  nm.className = "rel-name";
  nm.textContent = name;
  label.appendChild(nm);
  row.appendChild(label);

  const meta = document.createElement("span");
  meta.className = "rel-meta";
  meta.textContent = [date, size].filter(Boolean).join(" · ");
  row.appendChild(meta);

  if (checked) {
    const pill = document.createElement("span");
    pill.className = "pill up";
    pill.textContent = "latest";
    row.appendChild(pill);
  }
  if (!synced) {
    const pill = document.createElement("span");
    pill.className = "pill";
    pill.textContent = "not yet synced to the flasher";
    row.appendChild(pill);
  }

  if (notesUrl) {
    const a = document.createElement("a");
    a.className = "rel-notes";
    a.href = notesUrl;
    a.target = "_blank";
    a.rel = "noopener";
    a.textContent = "release notes";
    row.appendChild(a);
  }
  return row;
}

function currentRelease() {
  return releases.find((e) => e.version === selectedTag) || null;
}

/** Show the board/flash-size picker only when a release really ships more than
 * one image. One image is the normal case and needs no question asked. */
function renderVariants() {
  const wrap = $("variant-row");
  const sel = $("variant-sel");
  const release = currentRelease();
  const images = release ? release.images : [];

  if (!release || images.length < 2) {
    wrap.classList.add("hidden");
    sel.textContent = "";
    selectedVariant = 0;
    return;
  }

  sel.textContent = "";
  images.forEach((img, i) => {
    const option = document.createElement("option");
    option.value = String(i);
    option.textContent = img.label +
      (Number.isFinite(img.size) && img.size ? " — " + fmtSize(img.size) : "");
    sel.appendChild(option);
  });
  if (selectedVariant >= images.length) selectedVariant = 0;
  sel.value = String(selectedVariant);
  $("variant-hint").textContent = images[selectedVariant].note || "";
  wrap.classList.remove("hidden");
}

async function onReleaseSelected() {
  const release = currentRelease();
  if (!release) { renderVariants(); return; }
  await loadReleaseDetail(release);
  renderVariants();
}

async function renderReleases() {
  const list = $("rel-list");

  let manifestError = null;
  const [mf, ghReleases] = await Promise.all([
    fetchManifest().catch((err) => { manifestError = err; return []; }),
    fetchGithubReleases(),
  ]);
  releases = mf;

  const notesByTag = new Map(ghReleases.map((r) => [r.tag_name, r.html_url]));

  list.textContent = "";
  let first = true;
  for (const entry of releases) {
    list.appendChild(relRow({
      name: entry.version,
      tag: entry.version,
      date: fmtDate(entry.date),
      size: entry.images.length === 1 ? fmtSize(entry.images[0].size) : "",
      notesUrl: notesByTag.get(entry.version) || entry.notesUrl,
      synced: true,
      checked: first,
    }));
    first = false;
  }

  // Releases GitHub knows about but CI has not mirrored here yet (just
  // published, or older than the retention window). Visible but unpickable.
  const syncedTags = new Set(releases.map((e) => e.version));
  for (const rel of ghReleases) {
    if (syncedTags.has(rel.tag_name)) continue;
    list.appendChild(relRow({
      name: rel.name || rel.tag_name,
      tag: rel.tag_name,
      date: fmtDate(rel.published_at),
      size: "",
      notesUrl: rel.html_url,
      synced: false,
      checked: false,
    }));
  }

  if (!list.children.length) {
    const span = document.createElement("span");
    span.className = "hint";
    span.textContent = manifestError
      ? "Could not load the firmware list (" + manifestError.message + "). " +
        "You can still flash a local .bin file."
      : "No firmware has been synced to the flasher yet. " +
        "You can still flash a local .bin file.";
    list.appendChild(span);
  }

  await onReleaseSelected();
}

/* ------------------------------------------------------- firmware bytes */

/** Resolve the current selection into [{name, address, data}], verified. */
async function collectPlan() {
  if ($("src-local").checked) {
    const picked = $("local-file").files && $("local-file").files[0];
    if (!picked) throw new Error("pick a .bin file first");
    const offset = parseOffset($("local-offset").value);
    if (offset === null) {
      throw new Error('invalid offset "' + $("local-offset").value +
                      '" — use hex like 0x0, or decimal');
    }
    const data = new Uint8Array(await picked.arrayBuffer());
    if (data.length === 0) throw new Error('"' + picked.name + '" is empty');
    log("Local file " + picked.name + ": " + data.length.toLocaleString() +
        " bytes at " + hex(offset) + " (no checksum to verify for local files).");
    return [{ name: picked.name, address: offset, data }];
  }

  const release = currentRelease();
  if (!release) throw new Error("pick a firmware version first");
  await loadReleaseDetail(release);
  const image = release.images[selectedVariant];
  if (!image) {
    throw new Error("the manifest lists no image for " + release.version +
                    " — re-run the flasher-sync workflow");
  }

  log("Downloading " + release.version + " (" + image.path + ") ...");
  let response;
  try {
    response = await fetch(FIRMWARE_BASE + image.path, { cache: "no-store" });
  } catch (err) {
    throw new Error("the image could not be downloaded (" + (err.message || err) +
                    ") — check your network connection and reload");
  }
  if (!response.ok) {
    throw new Error("could not download the image (HTTP " + response.status +
                    "). The mirror may be mid-update — try again in a minute.");
  }
  const data = new Uint8Array(await response.arrayBuffer());

  if (Number.isFinite(image.size) && image.size !== null && data.length !== image.size) {
    throw new Error("size mismatch — expected " + image.size + " bytes, got " +
                    data.length + ". Refusing to flash a partial download.");
  }

  // Refuse to write anything whose digest does not match the manifest. A bad
  // image at 0x0 produces a board that cannot boot and cannot report why.
  if (!/^[0-9a-f]{64}$/.test(image.sha256 || "")) {
    throw new Error("the manifest carries no usable sha256 for this image — " +
                    "refusing to flash something that cannot be verified");
  }
  const actual = await sha256hex(data);
  if (actual !== image.sha256) {
    throw new Error("checksum mismatch — refusing to flash. Expected " +
                    image.sha256.slice(0, 16) + "…, got " + actual.slice(0, 16) + "…");
  }
  log("Checksum verified (sha256 " + actual.slice(0, 16) + "…), " +
      data.length.toLocaleString() + " bytes.");

  return [{ name: release.version, address: FULL_IMAGE_ADDRESS, data }];
}

/* -------------------------------------------------------- esptool-js core */

let modulePromise = null;

async function loadEsptool() {
  if (!modulePromise) {
    modulePromise = import(BUNDLE_URL).catch((err) => {
      modulePromise = null;
      throw new Error("the flashing engine failed to load from " + BUNDLE_URL +
                      " (" + err.message + ") — reload the page and try again");
    });
  }
  return modulePromise;
}

/** Uint8Array -> the "binary string" esptool-js's writeFlash expects. */
function toBinaryString(bytes) {
  const CHUNK = 0x8000;
  let out = "";
  for (let i = 0; i < bytes.length; i += CHUNK) {
    out += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
  }
  return out;
}

/** Reset out of the download stub, tolerating API drift across versions. */
async function resetDevice(loader, transport) {
  if (loader && typeof loader.after === "function") { await loader.after(); return; }
  if (loader && typeof loader.hardReset === "function") { await loader.hardReset(); return; }
  if (transport && typeof transport.setDTR === "function") {
    await transport.setDTR(false);
    await transport.setRTS(true);
    await new Promise((resolve) => setTimeout(resolve, 100));
    await transport.setRTS(false);
  }
}

/** Prompt for a port, enter the download stub, run action(loader), reset.
 * No requestPort filter: the S3's native USB-Serial/JTAG must always list. */
async function withConnectedChip(action) {
  const { ESPLoader, Transport } = await loadEsptool();

  log("Select the serial port in the browser prompt ...");
  let port;
  try {
    port = await navigator.serial.requestPort({});
  } catch (err) {
    throw new Error("no serial port was selected. If the list was empty, the cable may be " +
                    "charge-only, or the board is not in download mode — redo the BOOT " +
                    "dance above (" + (err.message || err) + ")");
  }

  const terminal = {
    clean() {},
    writeLine(data) { log(String(data)); },
    write(data) { log(String(data).replace(/\r?\n$/, "")); },
  };

  const transport = new Transport(port, true);
  try {
    const loader = new ESPLoader({
      transport,
      baudrate: FLASH_BAUD,
      romBaudrate: 115200,
      terminal,
      enableTracing: false,
    });

    log("Connecting (redo the BOOT dance if this stalls) ...");
    let chip;
    try {
      chip = await loader.main();
    } catch (err) {
      throw new Error("could not talk to the board (" + (err.message || err) + "). " +
                      "Close any serial monitor that has the port open, redo the BOOT " +
                      "dance, and try again.");
    }
    log("Connected to " + chip + ".");
    if (!String(chip).toLowerCase().replace(/[^a-z0-9]/g, "").includes("esp32s3")) {
      log("WARNING: Klingelbox firmware targets the ESP32-S3; the connected chip reports '" +
          chip + "'. Continuing, but this image will not run on it.");
    }

    await action(loader);

    log("Resetting the device ...");
    await resetDevice(loader, transport);
    log("Make sure BOOT is not held — otherwise the chip re-enters the ROM download " +
        "loader instead of running the firmware. If the board does not come back on its " +
        "own, unplug and replug the USB cable.");
    return chip;
  } finally {
    try { await transport.disconnect(); } catch { /* port already gone */ }
  }
}

/* ------------------------------------------------------------------ flash */

async function doFlash() {
  if (busy) return;

  const errEl = $("flash-error");
  errEl.textContent = "";
  $("success").classList.add("hidden");

  busy = true;
  $("flash-btn").disabled = true;
  $("erase-btn").disabled = true;
  openLog();

  const progress = $("progress");
  const bar = $("bar");
  const barLabel = $("bar-label");

  try {
    const plan = await collectPlan();

    progress.classList.remove("hidden");
    bar.style.width = "0%";
    barLabel.textContent = "waiting for the device …";

    await withConnectedChip(async (loader) => {
      if ($("erase-first").checked) {
        barLabel.textContent = "erasing flash …";
        log("Erasing flash — this takes a few seconds ...");
        await loader.eraseFlash();
      }
      for (const item of plan) {
        log("Will write " + item.data.length.toLocaleString() + " bytes at " +
            hex(item.address) + ".");
      }
      const grandTotal = plan.reduce((sum, f) => sum + f.data.length, 0);
      const before = [];
      let acc = 0;
      for (const f of plan) { before.push(acc); acc += f.data.length; }

      await loader.writeFlash({
        fileArray: plan.map((f) => ({ data: toBinaryString(f.data), address: f.address })),
        // merge_bin already stamped the bootloader header on the build host;
        // never re-stamp headers this page did not produce.
        flashSize: "keep",
        flashMode: "keep",
        flashFreq: "keep",
        eraseAll: false,
        compress: true,
        reportProgress(fileIndex, written, total) {
          const overall = grandTotal
            ? Math.round(((before[fileIndex] + written) / grandTotal) * 100)
            : 0;
          bar.style.width = overall + "%";
          barLabel.textContent = overall + "% — " +
            (before[fileIndex] + written).toLocaleString() + " / " +
            grandTotal.toLocaleString() + " bytes";
        },
      });
      log("Write complete.");
    });

    bar.style.width = "100%";
    barLabel.textContent = "done";
    $("success").classList.remove("hidden");
    log("Done. If nothing happens within ~10 seconds, unplug and replug the device.");
  } catch (err) {
    errEl.textContent = err.message || String(err);
    barLabel.textContent = "failed";
    log("ERROR: " + (err.message || err));
  } finally {
    busy = false;
    $("flash-btn").disabled = false;
    updateEraseArm();
  }
}

/* ------------------------------------------------------------------ erase */

function updateEraseArm() {
  $("erase-btn").disabled = busy || !$("erase-arm").checked;
}

async function doErase() {
  if (busy || !$("erase-arm").checked) return;
  if (!window.confirm(
    "Erase the ENTIRE flash of the connected device?\n\n" +
    "Firmware, Wi-Fi credentials, MQTT settings, every learned signal and the whole " +
    "node graph are destroyed. This cannot be undone.\n\n" +
    "The chip itself is not damaged: its ROM bootloader is permanent, so it stays " +
    "reflashable from this page at any time."
  )) return;

  $("erase-error").textContent = "";
  $("erase-msg").textContent = "";
  busy = true;
  $("flash-btn").disabled = true;
  $("erase-btn").disabled = true;
  openLog();
  log("=== erasing entire flash ===");
  try {
    const chip = await withConnectedChip(async (loader) => {
      log("Erasing the ENTIRE flash — this takes a few seconds ...");
      await loader.eraseFlash();
    });
    $("erase-msg").textContent = "Flash erased — " + chip + " is now blank.";
    log("Done.");
  } catch (err) {
    $("erase-error").textContent = err.message || String(err);
    log("ERROR: " + (err.message || err));
  } finally {
    busy = false;
    $("flash-btn").disabled = false;
    $("erase-arm").checked = false; // a second erase must be a fresh decision
    updateEraseArm();
  }
}

/* ------------------------------------------------------------------- init */

function onSourceChange() {
  const local = $("src-local").checked;
  $("release-pane").classList.toggle("hidden", local);
  $("local-pane").classList.toggle("hidden", !local);
  // A release full image factory-resets anyway, so pre-erasing costs nothing
  // and guarantees a clean slate; for arbitrary local files (which may be a
  // partial write on purpose) it must be the operator's own decision.
  $("erase-first").checked = !local;
}

function init() {
  applySupportGate();
  $("src-release").addEventListener("change", onSourceChange);
  $("src-local").addEventListener("change", onSourceChange);
  $("variant-sel").addEventListener("change", (event) => {
    selectedVariant = Number(event.target.value) || 0;
    const release = currentRelease();
    const image = release && release.images[selectedVariant];
    $("variant-hint").textContent = (image && image.note) || "";
  });
  $("flash-btn").addEventListener("click", doFlash);
  $("erase-arm").addEventListener("change", updateEraseArm);
  $("erase-btn").addEventListener("click", doErase);
  renderReleases();
}

init();
