#include "page_common.h"

#include <esp_vfs_fat.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FILE_ROOT_DIRECTORY "/sdcard"
#define FILE_PATH_MAX 256
#define FILE_LIST_MAX 32

typedef struct
{
    char name[128];
    size_t size;
    bool is_directory;
} file_entry_t;

typedef struct
{
    char directory[FILE_PATH_MAX];
    lv_obj_t *path_label;
    lv_obj_t *list;
} file_page_ui_t;

static file_page_ui_t s_file_ui;

static void file_cyber_panel(lv_obj_t *object, uint32_t background,
                             uint32_t border, lv_coord_t radius)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_shadow_color(object, lv_color_hex(border), 0);
    lv_obj_set_style_shadow_width(object, 8, 0);
    lv_obj_set_style_shadow_opa(object, LV_OPA_20, 0);
}

static void file_format_size(size_t size, char *buffer, size_t buffer_size)
{
    if (size >= 1024U * 1024U)
        snprintf(buffer, buffer_size, "%.1f MB", (double)size / (1024.0 * 1024.0));
    else if (size >= 1024U)
        snprintf(buffer, buffer_size, "%.1f KB", (double)size / 1024.0);
    else
        snprintf(buffer, buffer_size, "%lu B", (unsigned long)size);
}

static void file_format_storage(char *buffer, size_t buffer_size)
{
    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    if (esp_vfs_fat_info(FILE_ROOT_DIRECTORY, &total_bytes, &free_bytes) != ESP_OK ||
        total_bytes == 0U)
    {
        snprintf(buffer, buffer_size, "STORAGE // SD UNAVAILABLE");
        return;
    }

    const double total_gb = (double)total_bytes / (1024.0 * 1024.0 * 1024.0);
    const double free_gb = (double)free_bytes / (1024.0 * 1024.0 * 1024.0);
    snprintf(buffer, buffer_size, "STORAGE // FREE %.1f GB / %.1f GB", free_gb, total_gb);
}

static void file_join_path(char *buffer, size_t buffer_size, const char *base, const char *name)
{
    const size_t base_length = strlen(base);
    const size_t name_length = strlen(name);
    const size_t available = buffer_size > base_length + 1U ? buffer_size - base_length - 1U : 0U;
    const size_t copied_name_length = name_length < available ? name_length : available;
    if (base_length >= buffer_size)
    {
        buffer[0] = '\0';
        return;
    }
    memcpy(buffer, base, base_length);
    buffer[base_length] = '/';
    memcpy(buffer + base_length + 1U, name, copied_name_length);
    buffer[base_length + 1U + copied_name_length] = '\0';
}

static size_t file_scan_directory(
    const char *directory_path, file_entry_t *entries, size_t entry_capacity)
{
    DIR *directory = opendir(directory_path);
    if (directory == NULL)
        return 0;

    size_t entry_count = 0;
    struct dirent *entry;
    while (entry_count < entry_capacity && (entry = readdir(directory)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;

        file_entry_t *file = &entries[entry_count];
        strncpy(file->name, entry->d_name, sizeof(file->name) - 1U);
        file->name[sizeof(file->name) - 1U] = '\0';

        char path[FILE_PATH_MAX];
        file_join_path(path, sizeof(path), directory_path, file->name);
        struct stat info;
        const bool stat_ok = stat(path, &info) == 0;
        file->is_directory = stat_ok && S_ISDIR(info.st_mode);
        file->size = stat_ok && !file->is_directory ? (size_t)info.st_size : 0U;
        ++entry_count;
    }
    closedir(directory);
    return entry_count;
}

static void file_render_directory(void);

static void file_open_directory_event(lv_event_t *event)
{
    char *directory_path = (char *)lv_event_get_user_data(event);
    if (directory_path == NULL)
        return;
    strncpy(s_file_ui.directory, directory_path, sizeof(s_file_ui.directory) - 1U);
    s_file_ui.directory[sizeof(s_file_ui.directory) - 1U] = '\0';
    free(directory_path);
    lv_label_set_text(s_file_ui.path_label, s_file_ui.directory);
    file_render_directory();
}

static void file_render_directory(void)
{
    lv_obj_clean(s_file_ui.list);
    lv_obj_scroll_to_y(s_file_ui.list, 0, LV_ANIM_OFF);
    file_entry_t entries[FILE_LIST_MAX];
    size_t entry_count = file_scan_directory(s_file_ui.directory, entries, FILE_LIST_MAX);
    for (size_t index = 0; index < entry_count; ++index)
    {
        for (size_t next = index + 1U; next < entry_count; ++next)
        {
            if (!entries[index].is_directory && entries[next].is_directory)
            {
                file_entry_t temporary = entries[index];
                entries[index] = entries[next];
                entries[next] = temporary;
            }
        }
    }
    if (strcmp(s_file_ui.directory, FILE_ROOT_DIRECTORY) != 0 && entry_count < FILE_LIST_MAX)
    {
        memmove(&entries[1], &entries[0], entry_count * sizeof(entries[0]));
        strcpy(entries[0].name, "..");
        entries[0].size = 0;
        entries[0].is_directory = true;
        ++entry_count;
    }
    for (size_t index = 0; index < entry_count; ++index)
    {
        const int row_y = 16 + (int)index * 52;
        lv_obj_t *row = page_ui_panel(
            s_file_ui.list, 16, row_y, 632, 44, CYBER_PANEL);
        const bool is_parent = strcmp(entries[index].name, "..") == 0;
        file_cyber_panel(row, CYBER_PANEL,
                         entries[index].is_directory ? CYBER_CYAN : CYBER_MAGENTA, 0);
        lv_obj_t *icon = page_ui_label(
            row, entries[index].is_directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
            &lv_font_ShareTechMono1_20,
            is_parent ? CYBER_YELLOW : (entries[index].is_directory ? CYBER_CYAN : CYBER_MAGENTA));
        lv_obj_set_pos(icon, 18, 12);
        lv_obj_t *label = page_ui_label(
            row, entries[index].name, &lv_font_source_han_sans_sc_16_cjk, CYBER_TEXT);
        lv_obj_set_pos(label, 66, 11);
        char size_text[24];
        if (is_parent || entries[index].is_directory)
            strcpy(size_text, is_parent ? "返回上级" : "文件夹");
        else
            file_format_size(entries[index].size, size_text, sizeof(size_text));
        label = page_ui_label(
            row, size_text, &lv_font_ShareTechMono1_16, CYBER_MUTED);
        lv_obj_align(label, LV_ALIGN_RIGHT_MID, -22, 0);
        if (entries[index].is_directory)
        {
            char *target = static_cast<char *>(malloc(FILE_PATH_MAX));
            if (target == NULL)
                continue;
            if (strcmp(entries[index].name, "..") == 0)
            {
                const size_t directory_length = strlen(s_file_ui.directory);
                const size_t copied_length = directory_length < FILE_PATH_MAX - 1U
                                                 ? directory_length
                                                 : FILE_PATH_MAX - 1U;
                memcpy(target, s_file_ui.directory, copied_length);
                target[copied_length] = '\0';
                char *separator = strrchr(target, '/');
                if (separator != NULL && separator != target)
                    *separator = '\0';
            }
            else
                file_join_path(target, FILE_PATH_MAX, s_file_ui.directory, entries[index].name);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(row, lv_color_hex(CYBER_CYAN), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, LV_OPA_40, LV_STATE_PRESSED);
            lv_obj_add_event_cb(row, file_open_directory_event, LV_EVENT_CLICKED, target);
        }
    }
}

static void file_page_create(Page *page)
{
    lv_obj_set_style_bg_color(page->root, lv_color_hex(CYBER_BG), 0);
    lv_obj_set_style_pad_all(page->root, 0, 0);
    lv_obj_set_scrollbar_mode(page->root, LV_SCROLLBAR_MODE_OFF);
    strcpy(s_file_ui.directory, FILE_ROOT_DIRECTORY);

    lv_obj_t *toolbar = page_ui_panel(page->root, 28, 92, 664, 58, CYBER_PANEL);
    file_cyber_panel(toolbar, CYBER_PANEL, CYBER_CYAN, 0);
    lv_obj_t *label = page_ui_label(toolbar, LV_SYMBOL_SD_CARD, &lv_font_ShareTechMono1_20, CYBER_CYAN);
    lv_obj_set_pos(label, 18, 17);
    s_file_ui.path_label = page_ui_label(toolbar, s_file_ui.directory, &lv_font_ShareTechMono1_16, CYBER_TEXT);
    lv_obj_set_pos(s_file_ui.path_label, 54, 19);
    page_ui_add_top_left_bottom_right_brackets(toolbar, 664, 58, CYBER_YELLOW_GREEN, 0);

    s_file_ui.list = page_ui_panel(page->root, 28, 166, 664, 452, CYBER_PANEL);
    file_cyber_panel(s_file_ui.list, CYBER_PANEL, CYBER_CYAN, 0);
    lv_obj_add_flag(s_file_ui.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_file_ui.list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(s_file_ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_file_ui.list, LV_SCROLLBAR_MODE_OFF);
    file_render_directory();

    char storage_text[64];
    file_format_storage(storage_text, sizeof(storage_text));
    label = page_ui_label(
        page->root, storage_text, &lv_font_ShareTechMono1_16, CYBER_YELLOW_GREEN);
    lv_obj_set_pos(label, 34, 650);
}

static void file_page_leave(Page *page)
{
    (void)page;
    (void)ui_page_destroy(PAGE_FILE);
}

static void file_page_destroy(Page *page)
{
    (void)page;
    memset(&s_file_ui, 0, sizeof(s_file_ui));
}

extern "C" void file_page_register(void)
{
    Page page{};

    page.id = PAGE_FILE;

    page.name = "file";

    page.on_create = file_page_create;

    page.on_destroy = file_page_destroy;

    page.on_leave = file_page_leave;

    page.on_gesture = page_ui_back_gesture;
    ui_page_register(&page);
}
