if(NOT T_PANEL_P4_EXAMPLE)
    message(FATAL_ERROR "T_PANEL_P4_EXAMPLE is not set")
endif()

if(NOT T_PANEL_P4_EXAMPLE_CMAKE)
    set(T_PANEL_P4_EXAMPLE_CMAKE
        "${CMAKE_CURRENT_LIST_DIR}/examples/${T_PANEL_P4_EXAMPLE}/CMakeLists.txt")
endif()
set(EXAMPLE_CMAKE "${T_PANEL_P4_EXAMPLE_CMAKE}")
if(NOT EXISTS "${EXAMPLE_CMAKE}")
    message(FATAL_ERROR "Missing example build file: ${EXAMPLE_CMAKE}")
endif()

set(EXAMPLE_SRCS "")
set(EXAMPLE_INCLUDE_DIRS "")
set(EXAMPLE_PRIV_REQUIRES "")
set(EXAMPLE_EMBED_FILES "")
set(EXAMPLE_WHOLE_ARCHIVE OFF)

include("${EXAMPLE_CMAKE}")

# The round board uses the standard BSP API and headers, but has its own BSP
# component because its board wiring is packaged separately. Keep existing
# standard examples source-compatible while linking them with the round BSP.
if((T_PANEL_P4_HARDWARE_VERSION STREQUAL "round" OR
    T_PANEL_P4_BSP_COMPONENT STREQUAL "t_panel_p4_round") AND
   EXAMPLE_PRIV_REQUIRES)
    list(TRANSFORM EXAMPLE_PRIV_REQUIRES
        REPLACE "^t_panel_p4_standard$" "t_panel_p4_round")
endif()

if(NOT EXAMPLE_SRCS)
    message(FATAL_ERROR
        "Example '${T_PANEL_P4_EXAMPLE}' did not define EXAMPLE_SRCS")
endif()

set(COMPONENT_ARGS SRCS ${EXAMPLE_SRCS})
if(EXAMPLE_INCLUDE_DIRS)
    list(APPEND COMPONENT_ARGS INCLUDE_DIRS ${EXAMPLE_INCLUDE_DIRS})
endif()
if(EXAMPLE_PRIV_REQUIRES)
    list(APPEND COMPONENT_ARGS PRIV_REQUIRES ${EXAMPLE_PRIV_REQUIRES})
endif()
if(EXAMPLE_EMBED_FILES)
    list(APPEND COMPONENT_ARGS EMBED_FILES ${EXAMPLE_EMBED_FILES})
endif()
if(EXAMPLE_WHOLE_ARCHIVE)
    list(APPEND COMPONENT_ARGS WHOLE_ARCHIVE)
endif()

idf_component_register(${COMPONENT_ARGS})
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

