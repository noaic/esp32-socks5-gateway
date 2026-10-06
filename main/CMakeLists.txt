set(diag_assets "management.html" "management_setup.html")
if(CONFIG_PROXY_LINK_BENCH)
    list(APPEND diag_assets diagnostic_bench.html)
endif()
idf_component_register(SRCS "serial_recovery.c" "serial_commands.c" "serial_console.c" "gateway_control.c" "gateway_probe.c" "gateway_state.c" "gateway_settings.c" "settings_validation.c" "management_request.c" "web_management.c" "app_main.c" "ap_clients.c" "tcp_poc.c" "packet_rewrite.c" "flow_table.c" "socks_relay.c" "socks_udp.c" "dns_flow.c" "udp_relay.c" "udp_gateway.c" "net_diagnostics.c" "cpu_profile.c" "link_probe.c" "diagnostic_bench.c" "diagnostic_sta.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_uart esp_wifi esp_event esp_netif nvs_flash esp_timer
                  esp_psram esp_hw_support esp_system spi_flash heap lwip vfs esp_http_server mbedtls espressif__cjson espressif__led_strip
    EMBED_TXTFILES ${diag_assets})

# Observe existing calls without patching ESP-IDF/esp-lwIP or replacing the
# driver/netif function pointers. Reviewed against the pinned v6.1 source.
foreach(symbol esp_netif_transmit_wrap esp_netif_receive esp_netif_free_rx_buffer esp_netif_netstack_buf_ref esp_netif_netstack_buf_free tcp_rexmit_rto_commit tcp_rexmit_fast memp_malloc memp_free)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${symbol}")
endforeach()
