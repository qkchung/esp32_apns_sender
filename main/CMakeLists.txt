idf_component_register(SRCS "scan.c" "apns.c" "api_server.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event mbedtls esp-tls espressif__nghttp esp_http_server json
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "certs/apns_auth_key.p8")
