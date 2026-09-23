---
title: Security
layout: default
nav_order: 7
---

# Security

Out of the box the Klingelbox is what it has always been: an appliance whose
security boundary is **the network it sits on** — no login, no TLS, and a
strong "do not port-forward this" rule. Since v0.8.0 two opt-in features
tighten that boundary from *the network* to *people who know the password*:
an HTTP Basic password and TLS. This page is the reasoning; the mechanics are
in [API.md — Authentication & TLS](API.md#authentication--tls).

## Threat model

**What the box is worth attacking for.** Anyone who can drive `/api` can
transmit any learned signal, rewrite the node graph, overwrite Wi-Fi and MQTT
credentials, and — via `POST /api/ota/upload` — flash arbitrary firmware.
That last one turns "can ring your doorbell" into "owns a device on your
LAN", which is why exposure matters more than it may seem for a doorbell.

**What each layer stops:**

| Layer | Stops | Does not stop |
|---|---|---|
| Host + Content-Type checks (always on) | your own browser being used as a proxy: CSRF, DNS rebinding | anyone talking to the box directly |
| Web password (opt-in) | direct API use by people on the LAN/softAP who lack the password | credential sniffing on the wire, evil housemates with a USB cable |
| TLS (opt-in) | reading or tampering with traffic on the wire — including the Basic credentials of the layer above | anyone who has the password |

**Set both together.** HTTP Basic sends `base64(admin:password)` on *every*
request — on plain HTTP that is cleartext to anyone who can capture LAN or
Wi-Fi traffic, so a password without TLS only keeps out the polite. The
firmware deliberately does **not** couple them (each is useful alone in some
setups, and forcing TLS would break every existing `http://` bookmark the
moment a password is set); the web UI recommends the pair instead.

## The password

* One credential, user hardcoded `admin`, password 1–64 characters,
  write-only over the API, stored as a salted PBKDF2 hash in the config
  partition — the plaintext is never written to flash.
* It guards **every `/api` route on every transport** — LAN, softAP, and the
  recovery portal. Consistency is the point: a "recovery mode skips auth"
  exception would BE the vulnerability (walk near the house, jam the Wi-Fi
  until the box falls back, use the open portal).
* Static files (the UI shell, so its login screen can render) and
  `GET /cert.pem` (see below) stay open; neither changes state.
* Brute force: verifying a candidate password means stretching it through
  PBKDF2, which costs about **1 second** of real work — so every wrong guess
  pays a full second and guessing is capped at roughly one attempt per
  second, with no lockout window that could refuse the *right* password. A
  successful verify is cached server-side: the first correct request after
  boot pays the same ~1 s (the UI's login screen shows it as "Checking…"),
  every later one is checked instantly against the cache with a
  constant-time compare — timing distinguishes nothing.
* The 401 carries **no `WWW-Authenticate` header**, so browsers never pop
  their native password dialog over the UI's own login screen. `curl -u`,
  scripts and Home Assistant send credentials preemptively and never need
  the challenge.

### Lockout recovery

There is **no password reset**. Not over the API (that would be a bypass),
not via the recovery portal (same). A forgotten password is recovered with
physical possession and a USB cable: reflash the firmware with a merged
image, which resets NVS and with it the password — and also the Wi-Fi
credentials and learned signals, so export a backup while you still can.
That trade — "lost password = factory reset" — is deliberate: any softer
path would be reachable by an attacker too.

## TLS

* **On-device identity**: ECDSA P-256, generated on the box on first enable;
  the private key never leaves the device. P-256 because the ESP32-S3 has no
  ECC accelerator but software ECDSA still beats the chip's *accelerated*
  RSA-4096 by ~10× per handshake — and pinning (below) makes bigger keys
  pointless anyway.
* **Fixed validity 2026–2056**: the box has no clock (no SNTP, no RTC — every
  API timestamp is uptime-based for the same reason), so certificates carry a
  constant window instead of a fabricated "now".
* **Self-signed, and that is fine**: there is no CA that issues certificates
  for `klingelbox.local`. Trust is established by **pinning**, not by chain
  validation.

### Trust-on-first-use pinning

`GET /cert.pem` serves the active certificate — unauthenticated, and on
plain HTTP even while TLS is on, because the pin must be fetchable *before*
any trust exists. The certificate is public material (every TLS handshake
hands it to the peer), so serving it openly leaks nothing. To verify you
pinned the real box and not a man-in-the-middle's substitute, compare the
SHA-256 fingerprint from a **second channel**: `GET /api/config` →
`web.tls.fingerprint`, ideally read over the softAP, whose WPA2 key only you
have. Then give your client exactly that certificate
(`curl --cacert klingelbox.pem`, a Home Assistant `certificate:` entry, or a
browser exception) — after which nothing else on the path can impersonate
the box.

Your own certificate (from a home CA, or a real one if the box has a real
name) can replace the generated identity via `POST /api/tls/identity`; it is
fully validated before it is stored — including a key-strength floor of
RSA 2048 / EC 255 bits, because a factorable server key would hand a LAN
attacker exactly the impersonation pinning exists to prevent — so a bad
upload cannot kill the server.

### Why no HSTS — and no forced HTTPS

While TLS is on, port 80 answers GETs with a plain `302` to the same URL on
https. That redirect is the **entire** enforcement, deliberately:

* **HSTS is a browser-persisted promise** ("this host speaks HTTPS, refuse
  anything else, for *n* months"). The user can turn TLS off tomorrow — a
  supported, first-class action — and every browser that believed the
  promise would then refuse the box until the entry expires, with no UI
  hinting why. A sticky header on a togglable feature is a lockout bug.
* The redirect also never upgrades **writes**: a `302` would silently turn a
  POST into a bodyless GET, so port 80 refuses writes with a message naming
  `https://…:443` instead of half-replaying them.

### Fallbacks, stated honestly

The HTTPS server needs an identity to start. If the box ever finds itself
with TLS enabled and *no producible identity* (in practice: stored pair
corrupted **and** NVS too full to mint a new one), or the HTTPS server
itself fails to start, it serves **plain HTTP and says so** in the log and
the event feed, rather than serving nothing — an unreachable box cannot
even be told to turn TLS off. The downgrade is never permanent: while the
configuration says TLS and the fallback is what is running, the box retries
the HTTPS start **every 60 seconds, forever** — you chose TLS, and a
transient boot-time failure must not silently revoke that choice until the
next power cycle. The event feed reports the fallback once when it happens
and the recovery once when it succeeds, not every retry. On the recovery
portal with TLS enabled, captive-portal sheets may balk at the self-signed
redirect; opening `https://192.168.66.1` in a normal browser (and accepting
the pin) always works.

## The rest of the posture

Unchanged, and still deliberate:

* **Do not expose the box to the internet.** Password or not, TLS or not:
  reach it over a VPN into your own network, never a port forward.
* The **recovery portal is open (no passphrase) by default** so a box that
  lost its Wi-Fi can be rescued; set `recovery_pass` if RF range includes
  people you do not trust. (With a web password set, the portal's *pages*
  are open but every API action on it still requires the password.)
* The **operational softAP passphrase is generated from the hardware RNG at
  first boot**, never baked into the image.
* **All secrets are write-only** over the API — including the web password.
