// Board class-body fragments for src/sensor.cpp.
// The public and private sections are selected by board.hpp.

#if defined(BSP_BOARD_API_PUBLIC)

#if T_PANEL_P4_BSP_IS_RECT

        // 3.14 传感器 API：仅 Rect；src/sensor.cpp
        // ========================================================================
        // Repeated initialization preserves existing configurations. On failure,
        // initializeSensors() rolls back only sensors created by that call.
        [[nodiscard]] esp_err_t initializeSensors();
        [[nodiscard]] esp_err_t initializeSensors(const SensorConfig &config);
        [[nodiscard]] esp_err_t deinitializeSensors();
        [[nodiscard]] bool isSensorsInitialized() const noexcept;
        [[nodiscard]] esp_err_t initializeQmi8658c();
        [[nodiscard]] esp_err_t initializeQmi8658c(const Qmi8658cConfig &config);
        [[nodiscard]] esp_err_t deinitializeQmi8658c();
        [[nodiscard]] bool isQmi8658cInitialized() const noexcept;
        [[nodiscard]] esp_err_t initializeQmc6309();
        [[nodiscard]] esp_err_t initializeQmc6309(const Qmc6309Config &config);
        [[nodiscard]] esp_err_t deinitializeQmc6309();
        [[nodiscard]] bool isQmc6309Initialized() const noexcept;

        // Poll from task context. QMI reads the latest sample without waiting for
        // data-ready; QMC returns ESP_ERR_INVALID_STATE when no new sample is ready.
        // Units and raw chip axes follow the PDL types (g, degrees/s, Celsius, gauss).
        [[nodiscard]] esp_err_t readQmi8658cDataReady(pdl::sensor::Qmi8658cDataReady &ready,
                                                     std::uint32_t timeout_ms = 100) const;
        [[nodiscard]] esp_err_t readQmi8658cMeasurement(pdl::sensor::Qmi8658cMeasurement &measurement,
                                                       std::uint32_t timeout_ms = 100) const;
        [[nodiscard]] esp_err_t readQmc6309Status(pdl::sensor::Qmc6309Status &status,
                                                std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readQmc6309Measurement(pdl::sensor::Qmc6309Measurement &measurement,
                                                     std::uint32_t timeout_ms = 100);
        // Borrowed driver pointers; null until the corresponding sensor is ready.
        [[nodiscard]] pdl::sensor::Qmi8658c *qmi8658c() noexcept;
        [[nodiscard]] pdl::sensor::Qmc6309 *qmc6309() noexcept;
#endif

#elif defined(BSP_BOARD_API_PRIVATE)

        // 4.11 QMI8658C / QMC6309：仅 Rect；src/sensor.cpp
        // task_delay_ and i2c_bus_ outlive these transports and drivers.
        // ========================================================================
#if T_PANEL_P4_BSP_IS_RECT
        std::optional<pdl::esp_idf::I2cDevice> qmi8658c_transport_;
        std::optional<pdl::sensor::Qmi8658c> qmi8658c_;
        std::optional<pdl::esp_idf::I2cDevice> qmc6309_transport_;
        std::optional<pdl::sensor::Qmc6309> qmc6309_;

#endif

#endif
