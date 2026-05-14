idf_component_register(
    SRCS
        "../../third_party/monocypher/src/monocypher.c"
        "../../third_party/monocypher/src/optional/monocypher-ed25519.c"
    INCLUDE_DIRS
        "../../third_party/monocypher/src"
        "../../third_party/monocypher/src/optional"
)
