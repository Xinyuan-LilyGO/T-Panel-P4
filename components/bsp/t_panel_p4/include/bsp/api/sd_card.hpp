// Board class-body fragments for src/sd_card.cpp.
// The public and private sections are selected by board.hpp.

#if defined(BSP_BOARD_API_PUBLIC)

        // 3.11 SD 卡与 USB MSC API：src/sd_card.cpp
        // ========================================================================
        [[nodiscard]] esp_err_t initializeSdCard();
        [[nodiscard]] esp_err_t initializeSdCard(const SdCardConfig &config);
        [[nodiscard]] esp_err_t deinitializeSdCard();
        [[nodiscard]] bool isSdCardInitialized() const noexcept;
        [[nodiscard]] bool isSdCardMounted() const noexcept;
        // USB MSC 使用原始 SD 块设备；与本地文件系统访问互斥。
        [[nodiscard]] esp_err_t initializeUsbMsc();
        [[nodiscard]] esp_err_t deinitializeUsbMsc();
        [[nodiscard]] bool isUsbMscInitialized() const noexcept;

        // ========================================================================
        // 3.13 SD 卡句柄：src/sd_card.cpp
        // ========================================================================
        [[nodiscard]] sdmmc_card_t *sdCard() noexcept;
        [[nodiscard]] const char *sdCardMountPoint() const noexcept;

#elif defined(BSP_BOARD_API_PRIVATE)

        // 4.10 SDMMC / FAT 与 USB MSC 资源：src/sd_card.cpp
        // ========================================================================
        sdmmc_card_t *sd_card_{};
        std::unique_ptr<sdmmc_card_t> raw_sd_card_;
        sdmmc_host_t sd_host_{};
        sd_pwr_ctrl_handle_t sd_power_control_{};
        std::array<char, 64> sd_mount_point_{};
        SdCardConfig sd_card_config_{};
        std::array<char, 64> sd_card_config_mount_point_{};
        bool sd_host_initialized_{};
        bool sd_card_mounted_{};
        void *usb_msc_storage_{};
        bool usb_driver_initialized_{};
        bool usb_msc_driver_initialized_{};
        bool usb_msc_owns_sd_card_{};

#endif
