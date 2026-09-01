# Klingelbox firmware — main application component.
#
# Each feature is its own translation unit with a clean header. The radio layers
# deliberately live in components/ (cc1101, rfpulse) rather than here: they are
# reusable outside this project and must not grow doorbell-specific knowledge.
idf_component_register(
    SRCS
        "app_main.c"
        "db_diag.c"          # shared diagnostic vocabulary
        "db_config.c"        # versioned NVS config
        "rf_service.c"       # sole radio owner: RX/TX handover, burst coalescing
        "rf_raw.c"           # time-boxed raw capture session, filters relaxed
        "signal_store.c"     # persisted signals, matching, learn mode
        "node_graph.c"       # sources -> logic -> sinks routing engine
        "event_log.c"        # recent-activity ring for the UI
        "wifi_mgr.c"         # APSTA + Tasmota-style recovery portal
        "dns_server.c"       # captive DNS for the portal
        "http_api.c"         # REST + web UI serving
        "mqtt_bridge.c"      # MQTT + Home Assistant discovery
        "ota.c"              # dual-slot OTA + rollback
        "update_check.c"     # "is there a newer release?" against GitHub
    INCLUDE_DIRS "."
    REQUIRES
        cc1101              # CC1101 chip driver          (components/cc1101)
        rfpulse             # pulse capture/replay/decode (components/rfpulse)
        driver              # gpio, spi_master, rmt
        esp_timer
        esp_app_format      # esp_app_get_description
        esp_hw_support      # esp_random, esp_mac
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        lwip
        esp_http_server
        esp_http_client
        esp_https_ota
        app_update
        esp_partition
        spiffs
        vfs
        mqtt
        json                # cJSON
        mbedtls             # esp_crt_bundle for HTTPS OTA
        espressif__mdns     # mDNS responder (managed component)
)

# ---------------------------------------------------------------------------
# Web UI SPIFFS image.
#
# The UI is authored under firmware/webui/ and flashed as a SPIFFS image on the
# `storage` partition, served from /spiffs by http_api.c. SPIFFS (rather than
# EMBED_FILES) means the UI can be re-flashed — or updated over the air via
# POST /api/ota/webui — without recompiling the firmware.
#
# If webui/ has no index.html (a fresh CI checkout, or before the UI lands) we
# fall back to the minimal placeholder in main/www/, so the build always succeeds
# and the device always serves something.
# ---------------------------------------------------------------------------
set(WEBUI_SRC "${CMAKE_CURRENT_LIST_DIR}/../webui")
set(WEBUI_FALLBACK "${CMAKE_CURRENT_LIST_DIR}/www")
if(EXISTS "${WEBUI_SRC}/index.html")
    set(WEBUI_DIR "${WEBUI_SRC}")
    message(STATUS "Klingelbox: serving web UI from firmware/webui/")
else()
    set(WEBUI_DIR "${WEBUI_FALLBACK}")
    message(STATUS "Klingelbox: firmware/webui/ not found — using placeholder (main/www/)")
endif()

spiffs_create_partition_image(storage "${WEBUI_DIR}" FLASH_IN_PROJECT)
