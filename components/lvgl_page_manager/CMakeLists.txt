set(LV_PM_LVGL_COMPONENT "lvgl")
if("$ENV{IDF_COMPONENT_MANAGER}" STREQUAL "0")
    set(LV_PM_LVGL_COMPONENT "lvgl__lvgl")
endif()

idf_component_register(
    SRCS
        "lvgl_page_manager.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        freertos
        log
        ${LV_PM_LVGL_COMPONENT}
)
