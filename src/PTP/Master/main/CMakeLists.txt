idf_component_register(
    SRCS
        "main.c"
        "allystar_timing.c"
    INCLUDE_DIRS "."
    REQUIRES
        # networking stack
        esp_eth esp_event esp_netif lwip
        # storage / OTA
        app_update esp_partition nvs_flash
        # system runtime
        esp_system esp_timer freertos vfs
        # peripherals & display
        driver esp_driver_gptimer esp_driver_gpio esp_hw_support u8g2 u8g2-hal-esp-idf
        # web
        esp_http_server json
        # local components
        mac_hostname
)

option(PTP_TRANSPORT_UDP_IPV4 "Use UDP/IPv4 multicast E2E instead of Layer-2 PTP" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PTP_TRANSPORT_UDP_IPV4=$<BOOL:${PTP_TRANSPORT_UDP_IPV4}>)
