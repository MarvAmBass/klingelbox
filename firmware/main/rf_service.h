/*
 * rf_service.h - The one owner of the radio.
 *
 * Everything above this line (HTTP API, MQTT, the node graph) asks rf_service to
 * do things; nothing else touches the CC1101 or the RMT channels directly. That
 * single-ownership rule exists for a concrete hardware reason: the CC1101's GDO0
 * pin is the demodulated data OUTPUT while receiving and the data INPUT while
 * transmitting, and RMT refuses to bind two channels to one GPIO. So receiving
 * and transmitting are mutually exclusive and every switch is a small, ordered
 * dance — release the capture channel, idle the radio, flip the pin direction,
 * claim the transmit channel, and reverse it afterwards. Scattering that
 * sequence across modules would guarantee a stuck radio.
 *
 * The service also turns a stream of raw frames into meaningful *events*. A
 * single button press produces several near-identical frames, because these
 * remotes deliberately repeat themselves; publishing one event per frame would
 * make one press look like five. So frames are coalesced into a burst: similar
 * frames arriving inside a short window are merged and counted, and the event is
 * emitted once the burst ends. The repeat count is retained as evidence — a
 * frame seen several times is far more trustworthy than a one-off, which is
 * usually noise.
 */
#ifndef DB_RF_SERVICE_H
#define DB_RF_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "cc1101.h"
#include "esp_err.h"
#include "rf_decode.h"
#include "rf_frame.h"
#include "rf_raw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One coalesced reception: the raw frame plus everything we could work out
 * about it. `decoded.valid == false` is an ordinary outcome — the frame is
 * still complete and replayable. */
typedef struct {
    rf_frame_t       frame;
    rf_norm_t        norm;
    rf_decoded_t     decoded;
    rf_fingerprint_t fingerprint;
    uint8_t          repeats;   /* how many near-identical copies formed this burst */
    int              rssi_dbm;  /* sampled at the start of the burst */
    int64_t          ts_us;     /* esp_timer_get_time() of the first frame */
} rf_event_t;

/* Called from the capture task once per coalesced burst. Keep it short: heavy
 * work belongs on the consumer's own task. */
typedef void (*rf_event_cb_t)(const rf_event_t *ev, void *ctx);

/* Bring up the CC1101 and start the capture task. Returns an error only for a
 * genuinely fatal setup failure — a missing radio is reported through db_diag
 * (DB_DIAG_CC1101_NOT_DETECTED) and leaves the service running so the web UI can
 * still explain what is wrong instead of the box appearing dead. */
esp_err_t rf_service_start(void);

/* Register the burst listener (one; later consumers fan out from it). */
void rf_service_set_listener(rf_event_cb_t cb, void *ctx);

/* True once a CC1101 has been positively identified. */
bool rf_service_radio_present(void);
void rf_service_get_ident(cc1101_ident_t *out);

/* Live radio parameters. Setting them reconfigures the chip and restarts capture. */
void rf_service_get_radio_cfg(cc1101_radio_cfg_t *out);
esp_err_t rf_service_set_radio_cfg(const cc1101_radio_cfg_t *cfg);

/* Transmit a frame, handling the whole RX->TX->RX handover described above.
 * Blocks until the last pulse has left the peripheral. Reports DB_DIAG_TX_OK /
 * DB_DIAG_TX_FAILED — note TX_OK is a software-level claim only. */
esp_err_t rf_service_transmit(const rf_frame_t *frame, uint8_t repeats, uint32_t gap_us);

/* Current RSSI in dBm (valid while receiving). */
esp_err_t rf_service_rssi(int *dbm_out);

/*
 * Raw capture sessions (rf_raw.h). They live behind the radio owner because
 * relaxing the filters means RE-CREATING the RMT receive channel with a
 * different idle threshold and minimum pulse count, and nothing outside this
 * module is allowed to do that. rf_raw itself never touches the radio, so there
 * is no lock ordering to get wrong.
 *
 * While a session runs the normal pipeline is NOT switched off: frames that
 * would have passed the ordinary thresholds still go through burst coalescing,
 * matching and the node graph, so the doorbell keeps working during a session.
 * Everything BELOW those thresholds goes to the recorder only, and can therefore
 * never make the graph fire on noise.
 *
 * Returns ESP_ERR_INVALID_STATE if a session is already running, ESP_ERR_NO_MEM
 * if the heap cannot take it (in which case nothing is allocated and capture is
 * left exactly as it was).
 */
esp_err_t rf_service_raw_start(const db_raw_cfg_t *cfg);
void      rf_service_raw_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* DB_RF_SERVICE_H */
