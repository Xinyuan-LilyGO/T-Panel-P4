// Board class-body fragments for src/display.cpp.
// The public and private sections are selected by board.hpp.

#if defined(BSP_BOARD_API_PUBLIC)

        // 3.4 显示与背光 API：src/display.cpp；setDisplaySleep 仅 Standard
        // ========================================================================
        [[nodiscard]] esp_err_t initializeDisplay();
        [[nodiscard]] esp_err_t initializeDisplay(const DisplayConfig &config);
        [[nodiscard]] esp_err_t deinitializeDisplay();
        [[nodiscard]] bool isDisplayInitialized() const noexcept;
        // Returns the active configuration, or defaults when uninitialized.
        [[nodiscard]] DisplayConfig displayConfig() const noexcept;

        [[nodiscard]] esp_err_t setBacklightBrightness(std::uint8_t percent);
        [[nodiscard]] std::uint8_t backlightBrightness() const noexcept;
#if T_PANEL_P4_BSP_IS_STANDARD
        // Light-sleep display control: suspend the DPI stream/GDMA around P4 light
        // sleep, resynchronize JD9365 with write-only commands, and toggle the
        // backlight. Resume performs no panel register reads.
        [[nodiscard]] esp_err_t setDisplaySleep(bool sleeping);
#endif

        // ========================================================================
        // 3.13 显示句柄与帧缓冲：src/display.cpp
        // ========================================================================
        // Common ESP-IDF panel handle for all profiles. Borrowed; null until initialized.
        [[nodiscard]] esp_lcd_panel_handle_t displayPanel() const noexcept;
        [[nodiscard]] void *frameBuffer(std::uint8_t index = 0) const noexcept;
        [[nodiscard]] std::uint8_t frameBufferCount() const noexcept;

#elif defined(BSP_BOARD_API_PRIVATE)

        // 4.3 MIPI DSI / DPI、面板与背光：src/display.cpp
        // ========================================================================
        [[nodiscard]] esp_err_t initializeBacklight();
        [[nodiscard]] esp_err_t deinitializeBacklight();
        [[nodiscard]] esp_err_t assertDisplayReset(bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT);
        [[nodiscard]] esp_err_t releaseDisplayReset(bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT);

        esp_ldo_channel_handle_t mipi_phy_ldo_{};
        esp_lcd_dsi_bus_handle_t dsi_bus_{};
        esp_lcd_panel_io_handle_t dbi_io_{};
        esp_lcd_panel_handle_t panel_{};
        std::array<void *, maximumFrameBufferCount> frame_buffers_{};

        std::uint8_t frame_buffer_count_{};
        DisplayConfig display_config_{};
        std::uint8_t backlight_brightness_{};
        bool backlight_initialized_{};
        bool panel_initialized_{};
        bool display_initialized_{};
#if T_PANEL_P4_BSP_IS_STANDARD
        bool display_sleeping_{};
        std::uint8_t display_sleep_brightness_{};
#endif

#endif
