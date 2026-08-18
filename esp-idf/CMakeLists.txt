file(GLOB RNODE_BLE_SRCS "src/*.cpp")

include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS
        ${RNODE_BLE_SRCS}
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
        bt
)
