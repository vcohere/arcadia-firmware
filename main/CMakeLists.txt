idf_component_register(
    SRCS "main.c" "protocol.c" "buttons.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)
