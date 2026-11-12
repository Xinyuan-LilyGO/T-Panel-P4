#include "page_common.hpp"
#include "page_registry.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

namespace factory::ui
{
    namespace
    {
        constexpr char kRoot[] = "/sdcard";
        constexpr std::size_t kMaxEntries = 128;
        constexpr std::size_t kRowCache = 10;
        constexpr lv_coord_t kListWidth = 448;
        // Leave a clear gutter between row content and the vertical scrollbar.
        constexpr lv_coord_t kRowWidth = 420;
        constexpr lv_coord_t kRowStride = 58;

        struct FileEntry
        {
            char name[256]{};
            std::uint64_t size{};
            bool directory{};
        };

        struct FileRow
        {
            lv_obj_t *object{};
            lv_obj_t *icon{};
            lv_obj_t *name{};
            lv_obj_t *detail{};
            std::size_t index{kMaxEntries};
        };

        struct FilesUi
        {
            char directory[512]{"/sdcard"};
            lv_obj_t *path{};
            lv_obj_t *status{};
            lv_obj_t *list{};
            lv_obj_t *spacer{};
            lv_obj_t *empty{};
            lv_obj_t *up{};
            FileEntry entries[kMaxEntries]{};
            FileRow rows[kRowCache]{};
            std::size_t count{};
            bool refreshing{};
        };
        FilesUi s_files;

        void formatSize(std::uint64_t size, char *text, std::size_t capacity)
        {
            if (size >= 1024ULL * 1024ULL * 1024ULL)
                std::snprintf(text, capacity, "%.1f GB", static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0));
            else if (size >= 1024ULL * 1024ULL)
                std::snprintf(text, capacity, "%.1f MB", static_cast<double>(size) / (1024.0 * 1024.0));
            else if (size >= 1024ULL)
                std::snprintf(text, capacity, "%.1f KB", static_cast<double>(size) / 1024.0);
            else
                std::snprintf(text, capacity, "%llu B", static_cast<unsigned long long>(size));
        }

        void refreshRows()
        {
            if (s_files.list == nullptr || s_files.refreshing)
                return;
            s_files.refreshing = true;
            const lv_coord_t scroll = lv_obj_get_scroll_y(s_files.list);
            std::size_t first = scroll > 0 ? static_cast<std::size_t>(scroll / kRowStride) : 0U;
            first = std::min(first, s_files.count > kRowCache ? s_files.count - kRowCache : 0U);
            for (std::size_t slot = 0; slot < kRowCache; ++slot)
            {
                FileRow &row = s_files.rows[slot];
                row.index = first + slot;
                if (row.index >= s_files.count)
                {
                    lv_obj_add_flag(row.object, LV_OBJ_FLAG_HIDDEN);
                    continue;
                }
                const FileEntry &entry = s_files.entries[row.index];
                lv_obj_remove_flag(row.object, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_y(row.object, static_cast<lv_coord_t>(row.index * kRowStride));
                lv_label_set_text(row.icon, entry.directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
                const uint32_t item_color = entry.directory ? UI_WIHTE : UI_GTAY;
                lv_obj_set_style_text_color(row.icon, lv_color_hex(item_color), LV_PART_MAIN);
                lv_label_set_text(row.name, entry.name);
                lv_obj_set_style_text_color(row.name, lv_color_hex(item_color), LV_PART_MAIN);
                char detail[32]{};
                if (entry.directory)
                    std::snprintf(detail, sizeof(detail), "Folder");
                else
                    formatSize(entry.size, detail, sizeof(detail));
                lv_label_set_text(row.detail, detail);
                lv_obj_set_style_text_color(
                    row.detail,
                    lv_color_hex(entry.directory ? UI_WIHTE : UI_GTAY),
                    LV_PART_MAIN);
            }
            s_files.refreshing = false;
        }

        void scanDirectory()
        {
            s_files.refreshing = true;
            s_files.count = 0;
            bool truncated = false;
            bool read_error = false;
            DIR *directory = opendir(s_files.directory);
            if (directory != nullptr)
            {
                while (dirent *entry = readdir(directory))
                {
                    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
                        continue;
                    char path[512]{};
                    const int length = std::snprintf(path, sizeof(path), "%s/%s", s_files.directory, entry->d_name);
                    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path))
                    {
                        read_error = true;
                        continue;
                    }
                    struct stat info{};
                    if (stat(path, &info) != 0)
                    {
                        read_error = true;
                        continue;
                    }
                    if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode))
                        continue;
                    if (s_files.count == kMaxEntries)
                    {
                        truncated = true;
                        break;
                    }
                    FileEntry &file = s_files.entries[s_files.count++];
                    std::snprintf(file.name, sizeof(file.name), "%s", entry->d_name);
                    file.directory = S_ISDIR(info.st_mode);
                    file.size = info.st_size > 0 ? static_cast<std::uint64_t>(info.st_size) : 0U;
                }
                closedir(directory);
            }
            std::sort(s_files.entries, s_files.entries + s_files.count,
                      [](const FileEntry &a, const FileEntry &b) {
                          if (a.directory != b.directory)
                              return a.directory;
                          return strcasecmp(a.name, b.name) < 0;
                      });

            lv_label_set_text(s_files.path, s_files.directory);
            char status[80]{};
            if (directory == nullptr)
                std::snprintf(status, sizeof(status), "Cannot open folder. Check SD card.");
            else if (truncated)
                std::snprintf(status, sizeof(status), "Showing first %u items", static_cast<unsigned>(kMaxEntries));
            else
                std::snprintf(status, sizeof(status), "%u items%s", static_cast<unsigned>(s_files.count),
                              read_error ? " - some entries could not be read" : "");
            lv_label_set_text(s_files.status, status);
            lv_label_set_text(s_files.empty, directory == nullptr ? "SD card unavailable" : "Empty folder");
            if (s_files.count == 0)
                lv_obj_remove_flag(s_files.empty, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(s_files.empty, LV_OBJ_FLAG_HIDDEN);
            if (std::strcmp(s_files.directory, kRoot) == 0)
                lv_obj_add_state(s_files.up, LV_STATE_DISABLED);
            else
                lv_obj_remove_state(s_files.up, LV_STATE_DISABLED);
            lv_obj_set_height(s_files.spacer, std::max<lv_coord_t>(1, static_cast<lv_coord_t>(s_files.count * kRowStride)));
            lv_obj_scroll_to_y(s_files.list, 0, LV_ANIM_OFF);
            s_files.refreshing = false;
            refreshRows();
        }

        void rowClicked(lv_event_t *event)
        {
            auto *row = static_cast<FileRow *>(lv_event_get_user_data(event));
            if (row == nullptr || row->index >= s_files.count)
                return;
            const FileEntry &entry = s_files.entries[row->index];
            if (!entry.directory)
            {
                char size[32]{};
                formatSize(entry.size, size, sizeof(size));
                lv_label_set_text_fmt(s_files.status, "File size: %s", size);
                return;
            }
            char next[sizeof(s_files.directory)]{};
            const int length = std::snprintf(next, sizeof(next), "%s/%s", s_files.directory, entry.name);
            if (length < 0 || static_cast<std::size_t>(length) >= sizeof(next))
            {
                lv_label_set_text(s_files.status, "Folder path is too long");
                return;
            }
            std::snprintf(s_files.directory, sizeof(s_files.directory), "%s", next);
            scanDirectory();
        }

        void upClicked(lv_event_t *)
        {
            if (std::strcmp(s_files.directory, kRoot) == 0)
                return;
            char *slash = std::strrchr(s_files.directory, '/');
            if (slash != nullptr && slash >= s_files.directory + sizeof(kRoot) - 1)
                *slash = '\0';
            scanDirectory();
        }

        void refreshClicked(lv_event_t *) { scanDirectory(); }
        void listScrolled(lv_event_t *) { refreshRows(); }

        lv_obj_t *toolbarButton(lv_obj_t *parent, const char *symbol, lv_coord_t x, lv_event_cb_t callback)
        {
            lv_obj_t *button = lv_obj_create(parent);
            lv_obj_set_size(button, 44, 44);
            lv_obj_align(button, LV_ALIGN_TOP_MID, x, 92);
            page_ui_apply_icon_style(button);
            lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_STATE_PRESSED);
            lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
            lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
            lv_obj_t *icon = page_ui_label(button, symbol, &lv_font_montserrat_20, UI_WIHTE);
            lv_obj_center(icon);
            return button;
        }

        void filesPageCreate(Page *page)
        {
            if (page == nullptr || page->root == nullptr)
                return;
            lv_obj_set_style_pad_all(page->root, 0, LV_PART_MAIN);
            lv_obj_remove_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);
            ui_create_background(page->root);
            lv_obj_t *heading = page_ui_label(page->root, "Files", &lv_font_montserrat_24, UI_WIHTE);
            lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 98);
            s_files.up = toolbarButton(page->root, LV_SYMBOL_UP, -190, upClicked);
            toolbarButton(page->root, LV_SYMBOL_REFRESH, 190, refreshClicked);
            s_files.path = page_ui_label(page->root, kRoot, &lv_font_source_han_sans_sc_16_cjk, UI_GTAY);
            lv_obj_set_width(s_files.path, kListWidth);
            lv_label_set_long_mode(s_files.path, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(s_files.path, LV_ALIGN_TOP_MID, 0, 150);

            s_files.list = page_ui_container(page->root, kListWidth, 348);
            lv_obj_align(s_files.list, LV_ALIGN_TOP_MID, 0, 190);
            lv_obj_set_style_bg_opa(s_files.list, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_scroll_dir(s_files.list, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(s_files.list, LV_SCROLLBAR_MODE_AUTO);
            lv_obj_set_style_width(s_files.list, kListWidth, LV_PART_MAIN);
            lv_obj_set_style_pad_right(s_files.list, 12, LV_PART_MAIN);
            lv_obj_set_style_width(s_files.list, 6, LV_PART_SCROLLBAR);
            lv_obj_set_style_bg_color(s_files.list, lv_color_hex(UI_GTAY), LV_PART_SCROLLBAR);
            lv_obj_set_style_bg_opa(s_files.list, LV_OPA_40, LV_PART_SCROLLBAR);
            lv_obj_set_style_radius(s_files.list, 3, LV_PART_SCROLLBAR);
            lv_obj_remove_flag(s_files.list, LV_OBJ_FLAG_SCROLL_ELASTIC);
            lv_obj_add_event_cb(s_files.list, listScrolled, LV_EVENT_SCROLL, nullptr);
            s_files.spacer = lv_obj_create(s_files.list);
            lv_obj_set_size(s_files.spacer, 1, 1);
            lv_obj_set_style_opa(s_files.spacer, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_remove_flag(s_files.spacer, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            for (FileRow &row : s_files.rows)
            {
                row.object = page_ui_container(s_files.list, kRowWidth, 52);
                lv_obj_set_style_bg_opa(row.object, LV_OPA_TRANSP, LV_PART_MAIN);
                // Rows are flat list elements: only a one-pixel bottom divider.
                lv_obj_set_style_radius(row.object, 0, LV_PART_MAIN);
                lv_obj_set_style_border_width(row.object, 1, LV_PART_MAIN);
                lv_obj_set_style_border_side(row.object, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
                lv_obj_set_style_border_color(row.object, lv_color_hex(UI_GTAY), LV_PART_MAIN);
                lv_obj_set_style_border_opa(row.object, LV_OPA_40, LV_PART_MAIN);
                lv_obj_set_style_bg_color(row.object, lv_color_hex(UI_WIHTE), LV_STATE_PRESSED);
                lv_obj_set_style_bg_opa(row.object, LV_OPA_10, LV_STATE_PRESSED);
                lv_obj_remove_flag(row.object, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_event_cb(row.object, rowClicked, LV_EVENT_CLICKED, &row);
                row.icon = page_ui_label(row.object, LV_SYMBOL_FILE, &lv_font_montserrat_20, UI_WIHTE);
                lv_obj_align(row.icon, LV_ALIGN_LEFT_MID, 10, 0);
                row.name = page_ui_label(row.object, "", &lv_font_source_han_sans_sc_16_cjk, UI_WIHTE);
                lv_obj_set_width(row.name, 235);
                lv_label_set_long_mode(row.name, LV_LABEL_LONG_MODE_DOTS);
                lv_obj_align(row.name, LV_ALIGN_LEFT_MID, 44, 0);
                row.detail = page_ui_label(row.object, "", &lv_font_montserrat_14, UI_GTAY);
                lv_obj_set_width(row.detail, 100);
                lv_obj_set_style_text_align(row.detail, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
                lv_obj_align(row.detail, LV_ALIGN_RIGHT_MID, -16, 0);
            }
            s_files.empty = page_ui_label(page->root, "", &lv_font_montserrat_16, UI_GTAY);
            lv_obj_align(s_files.empty, LV_ALIGN_CENTER, 0, 0);
            s_files.status = page_ui_label(page->root, "", &lv_font_montserrat_14, UI_GTAY);
            lv_obj_set_width(s_files.status, kListWidth);
            lv_obj_set_style_text_align(s_files.status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_align(s_files.status, LV_ALIGN_TOP_MID, 0, 562);
            scanDirectory();
        }

        void filesPageDestroy(Page *)
        {
            // Do not construct a large FilesUi temporary on the LVGL task stack.
            std::snprintf(s_files.directory, sizeof(s_files.directory), "%s", kRoot);
            s_files.path = nullptr;
            s_files.status = nullptr;
            s_files.list = nullptr;
            s_files.spacer = nullptr;
            s_files.empty = nullptr;
            s_files.up = nullptr;
            s_files.count = 0;
            s_files.refreshing = false;
            for (FileRow &row : s_files.rows)
                row = FileRow{};
        }
        void filesPageLeave(Page *) { (void)ui_page_destroy(PAGE_FILE); }
    } // namespace

    void registerFilesPage()
    {
        Page page{};
        page.id = PAGE_FILE;
        page.name = "files";
        page.on_create = filesPageCreate;
        page.on_destroy = filesPageDestroy;
        page.on_leave = filesPageLeave;
        page.on_gesture = page_ui_back_gesture;
        (void)ui_page_register(&page);
    }
} // namespace factory::ui

extern "C" void file_page_register() { factory::ui::registerFilesPage(); }
