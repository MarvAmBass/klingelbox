/*
 * http_api.h - The box's only user interface: an HTTP server on :80 that serves
 * the SPIFFS web UI and the REST surface specified in docs/API.md.
 *
 * WHY ONE ENTRY POINT. Everything the user can do — learn a button, wire the
 * node graph, replay a signal, join a Wi-Fi network, update the firmware — goes
 * through this module. It owns no domain state of its own: it translates JSON
 * into calls on signal_store / node_graph / rf_service / db_config and back. If
 * you find yourself wanting to keep a fact here, it belongs in one of those.
 *
 * NO AUTH, NO TLS, BY DESIGN. This is a trusted-LAN / softAP appliance with no
 * clock, no certificate store worth the flash, and no user database. Adding a
 * login would mean a password to lose on a device with no screen. The security
 * boundary is the network the box is on — stated here so it is a decision on the
 * record rather than an omission.
 *
 * The server is started AFTER Wi-Fi (it binds the LWIP stack) and BEFORE
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

/* Mount the `storage` SPIFFS image at /spiffs and start the HTTP server.
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
