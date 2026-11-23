// Board class-body fragments for src/lora.cpp.
// The public and private sections are selected by board.hpp.

#if defined(BSP_BOARD_API_PUBLIC)

        // 3.10 LoRa API：仅 Standard / Round；src/lora.cpp
        // ========================================================================
#if !T_PANEL_P4_BSP_IS_RECT
        [[nodiscard]] esp_err_t initializeLora();
        [[nodiscard]] esp_err_t initializeLora(const LoraConfig &config);
        [[nodiscard]] esp_err_t deinitializeLora();
        [[nodiscard]] bool isLoraInitialized() const noexcept;
        [[nodiscard]] esp_err_t loraStandby();
        [[nodiscard]] esp_err_t loraSleep();
        [[nodiscard]] std::int16_t loraLastError() const noexcept;
#endif

        // ========================================================================
        // 3.13 LoRa 句柄：src/lora.cpp
        // ========================================================================
#if !T_PANEL_P4_BSP_IS_RECT
        [[nodiscard]] LR2021 *loraRadio() noexcept;
        [[nodiscard]] const LoraConfig *loraConfig() const noexcept;
#endif

#elif defined(BSP_BOARD_API_PRIVATE)

        // 4.9 LR2021 / RadioLib：仅 Standard / Round；src/lora.cpp
        // ========================================================================
#if !T_PANEL_P4_BSP_IS_RECT
        std::unique_ptr<RadioLibHal> lora_hal_;
        std::unique_ptr<Module> lora_module_;
        std::unique_ptr<LR2021> lora_radio_;
        std::optional<LoraConfig> lora_config_;
        bool lora_initialized_{};
        std::int16_t lora_last_error_{};
#endif

#endif
