// Board class-body fragments for src/camera.cpp.
// The public and private sections are selected by board.hpp.

#if defined(BSP_BOARD_API_PUBLIC)

        // 3.6 摄像头控制、取帧与统计 API：src/camera.cpp
        // ========================================================================
        // Prepares the sensor and pipeline; call startCameraStreaming() to capture.
        [[nodiscard]] esp_err_t initializeCamera();
        [[nodiscard]] esp_err_t initializeCamera(const CameraConfig &config);
        [[nodiscard]] esp_err_t deinitializeCamera();
        [[nodiscard]] bool isCameraInitialized() const noexcept;
        [[nodiscard]] bool isCameraStreaming() const noexcept;
        [[nodiscard]] CameraConfig cameraConfig() const noexcept;
        [[nodiscard]] esp_err_t startCameraStreaming();
        [[nodiscard]] esp_err_t stopCameraStreaming();
        [[nodiscard]] esp_err_t reconfigureCamera(const CameraConfig &config);
        [[nodiscard]] esp_err_t recoverCameraPipeline();
        [[nodiscard]] esp_err_t readCameraProductId(std::uint16_t &product_id,
                                                    std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t softwareResetCameraSensor(std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setCameraTestPattern(bool enabled, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setCameraHorizontalMirror(bool enabled,
                                                          std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setCameraVerticalFlip(bool enabled, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setCameraAutoExposureTarget(std::uint8_t target,
                                                            std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readCameraSensorStatistics(
            pdl::camera::Ov2710Statistics &statistics, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t acquireCameraFrame(CameraFrame &frame,
                                                   std::uint32_t timeout_ms = 2000);
        [[nodiscard]] esp_err_t releaseCameraFrame(const std::uint8_t *frame);
        [[nodiscard]] pdl::camera::Ov2710ModeInfo cameraModeInfo() const noexcept;
        [[nodiscard]] CameraStatistics cameraStatistics() const noexcept;

#elif defined(BSP_BOARD_API_PRIVATE)

        // 4.5 OV2710 / CSI / ISP：src/camera.cpp；CameraState 隐藏管线实现
        // ========================================================================
        void populateCameraDevices(Devices &devices) noexcept;
        pdl::esp_idf::TaskDelay task_delay_;
        std::unique_ptr<CameraState> camera_state_;

#endif
