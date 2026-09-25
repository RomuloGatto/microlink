set(DASHBOARD_WEB_DIR "${CMAKE_CURRENT_LIST_DIR}/../web")
set(DASHBOARD_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/build_dashboard.py")
set(DASHBOARD_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(DASHBOARD_HTML "${DASHBOARD_GEN_DIR}/dashboard.html")
set(DASHBOARD_HEADER "${DASHBOARD_GEN_DIR}/dashboard_html.h")

idf_build_get_property(python PYTHON)

add_custom_command(
    OUTPUT
        "${DASHBOARD_HTML}"
        "${DASHBOARD_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DASHBOARD_GEN_DIR}"
    COMMAND "${python}" "${DASHBOARD_TOOL}"
            --web-dir "${DASHBOARD_WEB_DIR}"
            --out-html "${DASHBOARD_HTML}"
            --out-header "${DASHBOARD_HEADER}"
    DEPENDS
        "${DASHBOARD_TOOL}"
        "${DASHBOARD_WEB_DIR}/index.html"
        "${DASHBOARD_WEB_DIR}/app.css"
        "${DASHBOARD_WEB_DIR}/app.js"
    VERBATIM
)

add_custom_target(modem_watchdog_dashboard_assets
    DEPENDS "${DASHBOARD_HTML}" "${DASHBOARD_HEADER}"
)

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        microlink
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        app_update
        dns_server
        esp_app_format
        esp_driver_gpio
        espressif__cjson
        lwip
)

add_dependencies(${COMPONENT_LIB} modem_watchdog_dashboard_assets)
target_include_directories(${COMPONENT_LIB} PRIVATE "${DASHBOARD_GEN_DIR}")
