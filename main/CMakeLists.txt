idf_component_register(
    SRCS "main.c"
         "app_ui.c"
         "app_ui_pages.c"
         "ui_widgets.c"
         "app_font_cjk.c"
         "app_ports.c"
         "diag_log.c"
         "port_nvs.c"
         "port_wifi_portal.c"
         "port_ble.c"
    INCLUDE_DIRS "."
    REQUIRES tesla_key bsp nvs_flash esp_partition esp_timer bt esp_wifi esp_http_server esp_netif esp_event driver esp_app_format
)
