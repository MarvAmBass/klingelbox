# Browser flasher (`docs/flasher/`)

A static, self-contained **Web Serial flasher** for the Klingelbox ESP32-S3
firmware, served by GitHub Pages at `…/klingelbox/flasher/`. It flashes the
CI-built merged release image over the S3's native USB (Chrome/Edge on a
desktop) — no ESP-IDF, no Python, no toolchain.

| File | Role |
|---|---|
| `index.html` | The whole page: markup + CSS. No framework, no build step. |
| `flasher.js` | Manifest discovery, verification, and the esptool-js driving code. |
| `lib/` | Vendored [esptool-js](https://github.com/espressif/esptool-js) bundle (Apache-2.0). See `lib/README.md`. **Do not edit.** |
| `firmware/` | CI-managed mirror of release images + `manifest.json`. **Do not hand-edit.** |

It writes exactly one segment:

| Offset | Data |
|---|---|
| `0x0` | `klingelbox-esp32s3-full.bin` — the merged release image (bootloader, partition table, otadata, app, web-UI SPIFFS). |

`klingelbox.bin` (app only) and `storage.bin` (web UI) are **OTA payloads**, not
flashable images: they go to `POST /api/ota` and `POST /api/ota/webui`. The page
says so, and the local-file path defaults to `0x0` accordingly.

## No config pre-seeding — on purpose

The reference project this page was ported from bakes an NVS "config seed"
partition in the browser and writes it at `0x9000`. That is **deliberately not
implemented here**: the Klingelbox firmware has no seed importer — see the
"NOT PORTED" note in
[`firmware/main/db_config.h`](../../firmware/main/db_config.h) — so a baked seed
would sit in NVS and be silently ignored on boot. A feature that appears to work
and does nothing is worse than no feature.

First-boot configuration therefore happens where the firmware actually supports
it: the `Klingelbox-XXXX` recovery portal. If a seed importer is ever added to
`db_config.c`, port `nvs-image.mjs` + `preconfig.mjs` from the reference then,
and only then.

## `firmware/` — CI-managed mirror, do not hand-edit

**GitHub release-asset downloads send no CORS headers.** A browser page cannot
fetch `https://github.com/<owner>/<repo>/releases/download/...` at all — the
request is blocked before any bytes arrive, and no amount of client-side code
can change that, because the header has to come from GitHub's asset host. So the
`flasher-sync` workflow mirrors release images **same-origin** into
`firmware/<tag>/` on this Pages site and regenerates `firmware/manifest.json`.
(`api.github.com` *does* send CORS headers, which is why the page can read the
release list from it — but only for names, dates and notes links, never for
bytes.)

### `firmware/manifest.json` — the index

```jsonc
{
  "generated": "2026-08-31T10:00:00Z",
  "keep": 3,
  "releases": [                       // newest first
    {
      "version": "v0.1.0",
      "date": "2026-08-31T09:12:34Z",
      "full": {
        "path": "v0.1.0/klingelbox-esp32s3-full.bin",  // relative to firmware/
        "size": 4194304,
        "sha256": "…64 hex…"
      },
      "notes_url": "https://github.com/MarvAmBass/klingelbox/releases/tag/v0.1.0"
    }
  ]
}
```

A release entry may instead carry a **`variants`** array (same fields plus `id`,
`label`, optional `note`) when one release ships several images — for example a
16 MB build and a 4 MB ESP32-S3 Zero build. The page then shows a board picker;
with a single image it asks nothing.

### `firmware/<tag>/manifest.json` — optional per-release detail

If the index omits `size`/`sha256`, the page lazily fetches
`firmware/<tag>/manifest.json` for the selected release only. Same `full` /
`variants` shape, but `path` is relative to `firmware/<tag>/`. Either layout
works; the index-only form is simpler and is what `flasher-sync` should emit
unless there is a reason not to.

The page verifies **size and sha256** against the manifest before writing a
byte, and refuses outright when a manifest carries no usable digest. Re-run
`flasher-sync` via `workflow_dispatch` to backfill or repair the mirror.

## Checks

Everything here is plain ES2020 with no dependencies, so the whole check is:

```sh
node --check docs/flasher/flasher.js
```

(Do not run it against `lib/esptool-js.bundle.mjs` — that is a vendored
artifact.) CI may add this to a lint job; there is nothing else to build.

## Testing a change by hand

Web Serial needs a secure context, and `file://` is not one. Serve the directory
over `http://localhost`, which counts as secure:

```sh
python3 -m http.server -d docs 8000   # then open http://localhost:8000/flasher/
```
