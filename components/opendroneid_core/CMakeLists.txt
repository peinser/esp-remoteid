idf_component_register(
    SRCS "../../third_party/opendroneid-core-c/libopendroneid/opendroneid.c"
    INCLUDE_DIRS "../../third_party/opendroneid-core-c/libopendroneid"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE ODID_AUTH_MAX_PAGES=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)
