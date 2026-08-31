# Vendored esptool-js

`esptool-js.bundle.mjs` is a single self-contained ES module built from the
[esptool-js](https://github.com/espressif/esptool-js) npm package (Apache-2.0,
`LICENSE` alongside; `VERSION` records the bundled release — currently
**0.5.6**) with its dependencies (pako, tslib) inlined. It was produced with
this recipe:

    npm install --no-package-lock --omit=dev "esptool-js@<VERSION>"
    npm install --no-package-lock --no-save "esbuild@0.25.5"
    printf 'export * from "esptool-js";\n' > entry.mjs
    esbuild entry.mjs --bundle --format=esm --platform=browser --target=es2020 \
      --legal-comments=inline --outfile=esptool-js.bundle.mjs

The file in this directory is a **byte-identical copy** of the bundle already in
use in the author's other ESP32-S3 projects (same recipe, same version,
hardware-proven on the S3's native USB-Serial/JTAG). It was copied across
rather than re-bundled precisely so that "the thing that writes to your flash"
is a known-good artifact and not a fresh, untested build.

It is **committed** because GitHub Pages serves `docs/` as-is with no build
step, and the flasher page must load everything same-origin — no CDN in the
path of something that writes to your hardware. The directory is named `lib/`
(not `vendor/`) to dodge Jekyll's default `vendor/` exclusion, so the bundle is
actually published by the Pages build.

Do not edit the bundle. Bump it deliberately: rerun the recipe with a newer
pinned version, update `VERSION` and `LICENSE`, and re-test a real flash on an
ESP32-S3 before committing.

## Licence

esptool-js is Apache-2.0. The full licence text is in `LICENSE` next to the
bundle and is published together with it — that file must stay wherever the
bundle goes.
