/*
 * dns_server.h - Captive DNS for the softAP (UDP :53). See dns_server.c.
 *
 * Answers EVERY A query with the box's current softAP address. That, together
 * with the AP's DHCP server handing itself out as the resolver (wifi_mgr.c), is
 * what makes a phone pop its "Sign in to network" sheet the moment it joins
 * Klingelbox-XXXX — which is the whole first-run experience: join the hotspot,
 * the Wi-Fi wizard appears, pick your network, done.
 */
#ifndef DB_DNS_SERVER_H
#define DB_DNS_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the DNS task. Call after Wi-Fi/the AP is up. The task resolves the AP
 * address freshly on every query (via db_wifi_ap_ip), so it survives a subnet
 * hop or an AP restart with no re-registration. */
void db_dns_start(void);

#ifdef __cplusplus
}
#endif

#endif /* DB_DNS_SERVER_H */
