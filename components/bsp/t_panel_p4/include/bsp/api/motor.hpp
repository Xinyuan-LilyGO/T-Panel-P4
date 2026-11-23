// Board class-body fragments for src/motor.cpp.
// The public and private sections are selected by board.hpp.

#if defined(BSP_BOARD_API_PUBLIC)

#if T_PANEL_P4_BSP_IS_RECT

        // 3.15 马达 API：仅 Rect；src/motor.cpp
        // ========================================================================
        // Initialization leaves the chip in standby. Calls must run in task
        // context and be serialized with lifecycle and direct driver operations.
        [[nodiscard]] esp_err_t initializeMotor();
        [[nodiscard]] esp_err_t initializeMotor(const MotorConfig &config);
        [[nodiscard]] esp_err_t deinitializeMotor();
        [[nodiscard]] bool isMotorInitialized() const noexcept;
        // Nonblocking playback; effect IDs are 1..117, sequences have 1..8 IDs.
        [[nodiscard]] esp_err_t playMotorEffect(std::uint8_t effect_id,
                                               std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t playMotorSequence(const std::uint8_t *effects,
                                                 std::size_t effect_count,
                                                 std::uint32_t timeout_ms = 100);
        // Forward signed-RTP register values 0..127; zero stops and enters standby.
        // A nonzero value keeps driving until explicitly stopped or deinitialized.
        [[nodiscard]] esp_err_t setMotorRealtimeValue(std::uint8_t value,
                                                     std::uint32_t timeout_ms = 100);
        // Stops both waveform and RTP output, then enters standby.
        [[nodiscard]] esp_err_t stopMotor(std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readMotorPlaying(bool &playing,
                                                std::uint32_t timeout_ms = 100) const;
        [[nodiscard]] esp_err_t readMotorStatus(std::uint8_t &status,
                                               std::uint32_t timeout_ms = 100) const;
        [[nodiscard]] pdl::motor::Drv2605 *drv2605() noexcept;
#endif

#elif defined(BSP_BOARD_API_PRIVATE)

#if T_PANEL_P4_BSP_IS_RECT
        // 4.12 DRV2605：transport outlives the driver; EN is owned by this group.
        [[nodiscard]] esp_err_t prepareMotorPlayback(std::uint32_t timeout_ms);
        std::optional<pdl::esp_idf::I2cDevice> drv2605_transport_;
        std::optional<pdl::motor::Drv2605> drv2605_;
        bool motor_enable_configured_{};
#endif

#endif
