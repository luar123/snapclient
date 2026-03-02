idf_component_register(SRCS "snapclient_helper.c" "snapclient.c" "connection_handler.c"
                       INCLUDE_DIRS "include"

                       PRIV_REQUIRES esp_timer mdns opus flac lightsnapcast
                       )

if(CONFIG_USE_DSP_PROCESSOR)
    idf_component_optional_requires(PRIVATE dsp_processor)
endif()
if(CONFIG_SNAPSERVER_USE_MDNS)
    idf_component_optional_requires(PRIVATE mdns)
endif()

idf_component_optional_requires(PRIVATE net_functions network_interface settings_manager)
if(settings_manager IN_LIST BUILD_COMPONENTS)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DHAS_SETTINGS_MANAGER")
endif()
if(network_interface IN_LIST BUILD_COMPONENTS)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DHAS_NET_IF")
endif()
if(net_functions IN_LIST BUILD_COMPONENTS)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DHAS_NET_F")
endif()