// Board class-body fragments for src/power.cpp.
// The public and private sections are selected by board.hpp.

#if defined(BSP_BOARD_API_PUBLIC)

        // 3.3 电源、充电与电池 API：src/power.cpp；板级供电/休眠见 src/board.cpp
        // ========================================================================
        [[nodiscard]] esp_err_t deinitializePower();
        [[nodiscard]] bool isPowerInitialized() const noexcept;
        [[nodiscard]] esp_err_t setPowerEventCallback(PowerEventCallback callback, void *context = nullptr);
        [[nodiscard]] esp_err_t readPowerStatus(pdl::charge::Axp517PowerStatus &status,
                                                std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readPowerModuleEnableStatus(
            pdl::charge::Axp517ModuleEnableStatus &status, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setChargerEnabled(bool enabled, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t getChargerEnabled(bool &enabled, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setChargeLedEnabled(bool enabled, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setChargeLedOutputType(
            pdl::charge::Axp517ChargeLedOutputType type, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setChargeLedMode(
            pdl::charge::Axp517ChargeLedMode mode, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setChargeLedCfgState(
            pdl::charge::Axp517ChargeLedCfgState state, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readChargeLedConfig(pdl::charge::Axp517ChargeLedConfig &config,
                                                    std::uint32_t timeout_ms = 100);
        // Controls both the AXP517 mask and GPIO delivery. Enabling uses the
        // initialization config's mask, even when interrupts started disabled.
        // May be called from the event callback; an in-flight callback can finish
        // after disabling. Serialize lifecycle operations externally.
        [[nodiscard]] esp_err_t setPowerInterruptEnabled(bool enabled);
        [[nodiscard]] bool isPowerInterruptEnabled() const noexcept;
        [[nodiscard]] esp_err_t readPowerFaults(pdl::charge::Axp517FaultStatus &status,
                                                std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t clearPowerFaults(std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readBatteryPercentage(std::uint8_t &percent,
                                                      std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readBatteryVoltageMv(std::uint16_t &millivolts,
                                                     std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readBatteryCurrentMa(std::int16_t &milliamps,
                                                     std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readVbusVoltageMv(std::uint16_t &millivolts,
                                                  std::uint32_t timeout_ms = 100);

#elif defined(BSP_BOARD_API_PRIVATE)

        // 4.2 AXP517、供电事件与中断任务：src/power.cpp
        // ========================================================================
        [[nodiscard]] esp_err_t initializePowerController(const PowerConfig &config);
        static void powerInterruptHandler(void *argument);
        static void powerEventTaskEntry(void *argument);
        void stopPowerEventTask() noexcept;

        std::optional<pdl::esp_idf::I2cDevice> axp517_transport_;
        std::optional<pdl::charge::Axp517> axp517_;
        pdl::esp_idf::GpioInterrupt power_interrupt_;
        TaskHandle_t power_event_task_{};
        SemaphoreHandle_t power_event_stopped_{};
        SemaphoreHandle_t power_interrupt_mutex_{};
        std::atomic<bool> power_interrupt_enabled_{false};
        std::atomic<bool> power_task_stop_{false};
        std::atomic<PowerEventCallback> power_event_callback_{nullptr};
        std::atomic<void *> power_event_context_{nullptr};
        PowerConfig power_config_{};

#endif
