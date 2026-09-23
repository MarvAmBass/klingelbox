/*
 * http_api.h - The box's only user interface: a web server that serves the
 * SPIFFS web UI and the REST surface specified in docs/API.md — plain HTTP on
 * :80 by default, HTTPS on :443 (with :80 shrunk to a 302 redirect) when the
 * user enables TLS.
 *
 * WHY ONE ENTRY POINT. Everything the user can do — learn a button, wire the
 * node graph, replay a signal, join a Wi-Fi network, update the firmware — goes
 * through this module. It owns no domain state of its own: it translates JSON
 * into calls on signal_store / node_graph / rf_service / db_config / db_tls and
 * back. If you find yourself wanting to keep a fact here, it belongs in one of
 * those.
 *
 * AUTH AND TLS ARE OPT-IN, OFF BY DEFAULT, AND INDEPENDENT. Out of the box
 * this remains the trusted-LAN appliance it always was: the security boundary
 * is the network. Setting a web password (POST /api/config, `web` section)
 * puts every /api route — on the LAN, the softAP and the recovery portal alike
 * — behind HTTP Basic auth for user "admin"; enabling TLS moves the server to
 * :443 with an on-device ECDSA identity that clients pin via GET /cert.pem.
 * Neither implies the other in firmware (the docs recommend TLS once a
 * password exists, because Basic credentials on plain HTTP can be sniffed).
 * The whole threat model lives in docs/security.md.
 *
 * The server is started AFTER Wi-Fi (it binds the LWIP stack, and a lazy TLS
 * key generation needs the RF-fed hardware RNG) and BEFORE
 * db_ota_mark_valid(), so an image that cannot serve its own UI never confirms
 * itself as good and gets rolled back on the next reset.
 */
#ifndef DB_HTTP_API_H
#define DB_HTTP_API_H

#include "esp_err.h"

#include "db_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the `storage` SPIFFS image at /spiffs and start the web server in the
 * personality cfg->tls_enabled selects (a TLS toggle at runtime is applied by
 * http_api itself, from a deferred task, without a reboot).
 *
 * `cfg` is the LIVE, long-lived configuration owned by app_main: the API reads
 * it on every request and writes it in place, calling db_config_save() itself
 * after each mutation. It must outlive the server (i.e. be a file-scope object
 * in app_main.c), because wifi_mgr's retry loop reads the same instance.
 *
 * A failed SPIFFS mount is NOT fatal: the REST API still comes up so the UI's
 * absence can be diagnosed over /api/diagnostics and repaired with a web-UI OTA.
 * Returns the httpd_start() result. */
esp_err_t db_http_start(db_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DB_HTTP_API_H */
