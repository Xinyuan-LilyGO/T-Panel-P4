#include <bsp/board.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>

#include <driver/isp.h>
#include <esp_attr.h>
#include <esp_cam_ctlr.h>
#include <esp_cam_ctlr_csi.h>
#include <esp_heap_caps.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <pdl/drivers/power/sgm38121/sgm38121.hpp>

#include "bsp_private.hpp"

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    struct CameraState
    {
        std::optional<pdl::esp_idf::I2cDevice> power_transport;
        std::optional<pdl::power::Sgm38121> power;
        std::optional<pdl::esp_idf::I2cDevice> camera_transport;
        std::optional<pdl::camera::Ov2710> camera;
        esp_cam_ctlr_handle_t controller{};
        isp_proc_handle_t isp_processor{};
        SemaphoreHandle_t frame_ready{};
        CameraConfig config{};
        std::array<std::uint8_t *, Board::maximumCameraBufferCount> capture_buffers{};
        pdl::camera::Ov2710ModeInfo mode_info{};
        std::size_t capture_buffer_size{};
        std::size_t next_capture_buffer{};
        std::size_t latest_frame_size{};
        const std::uint8_t *latest_frame{};
        const std::uint8_t *active_frame{};
        const std::uint8_t *dma_frame{};
        portMUX_TYPE frame_lock = portMUX_INITIALIZER_UNLOCKED;
        std::uint32_t received_frames{};
        std::uint32_t dropped_frames{};
        std::uint32_t recovery_count{};
        std::uint32_t recovery_failures{};
        esp_err_t last_error{ESP_OK};
        std::uint8_t capture_buffer_count{};
        bool controller_enabled{};
        bool controller_started{};
        bool isp_enabled{};
        bool initialized{};
    };
}

namespace
{
    constexpr char kTag[] = "t_panel_p4_camera";
    constexpr std::uint16_t kCameraDvddMillivolts = 1500;
    constexpr std::uint16_t kCameraAvdd1Millivolts = 2800;
    constexpr std::uint16_t kCameraAvdd2Millivolts = 3300;
    constexpr std::uint32_t kCameraPowerSettleMilliseconds = 20;
    constexpr std::uint32_t kCsiStartSettleMilliseconds = 100;

    using bsp::T_PANEL_P4_BSP_NAMESPACE::Board;
    using bsp::T_PANEL_P4_BSP_NAMESPACE::CameraConfig;
    using bsp::T_PANEL_P4_BSP_NAMESPACE::CameraState;

    struct CameraColorFormat
    {
        isp_color_t isp_color;
        std::uint8_t bits_per_pixel;
        const char *name;
    };

    // Keep CSI format, ISP format and buffer sizing consistent. An invalid
    // enum value must fail validation before power or DMA resources are touched.
    std::optional<CameraColorFormat> cameraColorFormat(cam_ctlr_color_t color) noexcept
    {
        switch (color)
        {
        case CAM_CTLR_COLOR_RAW8:
            return CameraColorFormat{ISP_COLOR_RAW8, 8, "RAW8"};
        case CAM_CTLR_COLOR_RAW10:
            return CameraColorFormat{ISP_COLOR_RAW10, 10, "RAW10"};
        case CAM_CTLR_COLOR_RAW12:
            return CameraColorFormat{ISP_COLOR_RAW12, 12, "RAW12"};
        case CAM_CTLR_COLOR_RGB565:
            return CameraColorFormat{ISP_COLOR_RGB565, 16, "RGB565"};
        case CAM_CTLR_COLOR_RGB888:
            return CameraColorFormat{ISP_COLOR_RGB888, 24, "RGB888"};
        default:
            return std::nullopt;
        }
    }

    void preserveFirstError(esp_err_t candidate, esp_err_t &first_error)
    {
        if (first_error == ESP_OK && candidate != ESP_OK)
            first_error = candidate;
    }

    esp_err_t pdlError(pdl::Status status, const char *operation)
    {
        if (pdl::isOk(status))
            return ESP_OK;
        ESP_LOGE(kTag, "%s failed: %s", operation, pdl::statusName(status));
        return bsp::T_PANEL_P4_BSP_NAMESPACE::detail::toEspError(status);
    }

    esp_err_t shutdownCameraPower(pdl::power::Sgm38121 &power, pdl::I2cDevice &transport)
    {
        ESP_RETURN_ON_ERROR(pdlError(power.shutdownSequence(), "stop camera power sequence"),
                            kTag, "camera power sequence command failed");
        // Sequence shutdown does not affect channels assigned to manual control.
        ESP_RETURN_ON_ERROR(pdlError(power.setAllChannelsEnabled(false), "disable manual camera rails"),
                            kTag, "camera manual power disable failed");
        std::uint8_t enabled{}, sequence{};
        for (unsigned attempt = 0; attempt < 30U; ++attempt)
        {
            ESP_RETURN_ON_ERROR(pdlError(power.readEnabledChannels(enabled), "read camera rail enables"),
                                kTag, "camera power readback failed");
            constexpr std::uint8_t sequence_register = 0x0F;
            ESP_RETURN_ON_ERROR(pdlError(transport.writeRead(&sequence_register, 1, &sequence, 1, 100),
                                          "read camera sequence status"),
                                kTag, "camera sequence readback failed");
            // REG0F[3] is SEQ_ON, [2:0] is SEQ_COUNT. Command bits self-clear.
            if (enabled == 0U && (sequence & 0x0FU) == 0U)
            {
                ESP_LOGI(kTag, "SGM38121 shutdown readback: EN=0x%02X SEQ=0x%02X",
                         static_cast<unsigned>(enabled), static_cast<unsigned>(sequence));
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        ESP_LOGE(kTag, "SGM38121 shutdown incomplete: EN=0x%02X SEQ=0x%02X",
                 static_cast<unsigned>(enabled), static_cast<unsigned>(sequence));
        return ESP_ERR_TIMEOUT;
    }

    TickType_t timeoutTicks(std::uint32_t timeout_ms) noexcept
    {
        if (timeout_ms == 0U)
            return 0;
        const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
        return ticks == 0 ? 1 : ticks;
    }

    bool IRAM_ATTR provideCaptureBuffer(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *transaction,
                                        void *user_context)
    {
        auto *state = static_cast<CameraState *>(user_context);
        portENTER_CRITICAL_ISR(&state->frame_lock);
        std::size_t selected = state->capture_buffer_count;
        for (std::size_t offset = 0; offset < state->capture_buffer_count; ++offset)
        {
            const std::size_t candidate =
                (state->next_capture_buffer + offset) % state->capture_buffer_count;
            const auto *buffer = state->capture_buffers[candidate];
            if (buffer != state->active_frame && buffer != state->latest_frame &&
                buffer != state->dma_frame)
            {
                selected = candidate;
                break;
            }
        }
        // With all buffers occupied, discard an unacquired frame rather than
        // overwrite the one the application is reading. Two-buffer clients may need to
        // reuse the just-completed DMA buffer; finishCapture must not publish it.
        if (selected == state->capture_buffer_count)
        {
            for (std::size_t offset = 0; offset < state->capture_buffer_count; ++offset)
            {
                const std::size_t candidate =
                    (state->next_capture_buffer + offset) % state->capture_buffer_count;
                if (state->capture_buffers[candidate] != state->active_frame)
                {
                    selected = candidate;
                    break;
                }
            }
        }
        if (state->capture_buffers[selected] == state->latest_frame)
        {
            state->latest_frame = nullptr;
            state->latest_frame_size = 0;
            ++state->dropped_frames;
        }
        state->dma_frame = state->capture_buffers[selected];
        state->next_capture_buffer = (selected + 1U) % state->capture_buffer_count;
        portEXIT_CRITICAL_ISR(&state->frame_lock);

        transaction->buffer = state->capture_buffers[selected];
        transaction->buflen = state->capture_buffer_size;
        return false;
    }

bool IRAM_ATTR finishCapture(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *transaction,
                             void *user_context)
{
    auto *state = static_cast<CameraState *>(user_context);

    portENTER_CRITICAL_ISR(&state->frame_lock);
    // ESP-IDF requests the next DMA destination before announcing the
    // completed frame. Never publish a buffer already reused for capture.
    if (transaction->buffer == state->dma_frame)
    {
        ++state->dropped_frames;
        portEXIT_CRITICAL_ISR(&state->frame_lock);
        return false;
    }
    if (state->latest_frame != nullptr)
        ++state->dropped_frames;
    state->latest_frame = static_cast<const std::uint8_t *>(transaction->buffer);
    state->latest_frame_size = transaction->received_size;
    ++state->received_frames;
    portEXIT_CRITICAL_ISR(&state->frame_lock);

    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(state->frame_ready, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

bool validCameraConfig(const CameraConfig &config) noexcept
{
    const pdl::camera::Ov2710ModeInfo mode_info = pdl::camera::ov2710ModeInfo(config.mode);
    const auto input_format = cameraColorFormat(config.input_data_color_type);
    const auto output_format = cameraColorFormat(config.output_data_color_type);
    // The OV2710 driver currently provides RAW10 modes only. Reject any input
    // that disagrees with the sensor rather than silently changing the sensor.
    const bool raw_input = config.input_data_color_type == CAM_CTLR_COLOR_RAW8 ||
                           config.input_data_color_type == CAM_CTLR_COLOR_RAW10 ||
                           config.input_data_color_type == CAM_CTLR_COLOR_RAW12;
    const bool rgb_output = config.output_data_color_type == CAM_CTLR_COLOR_RGB565 ||
                            config.output_data_color_type == CAM_CTLR_COLOR_RGB888;
    return mode_info.width != 0U && mode_info.height != 0U && mode_info.bits_per_pixel == 10U &&
           raw_input && input_format && input_format->bits_per_pixel == mode_info.bits_per_pixel &&
           rgb_output && output_format &&
           mode_info.mipi_lane_count == 1U && config.capture_buffer_count >= 2U &&
           config.capture_buffer_count <= Board::maximumCameraBufferCount &&
           config.auto_exposure_target >= pdl::camera::Ov2710::minimumAutoExposureTarget &&
           config.auto_exposure_target <= pdl::camera::Ov2710::maximumAutoExposureTarget &&
           config.isp_clock_hz != 0U;
}

void recordCameraError(CameraState &state, esp_err_t error) noexcept
{
    if (error == ESP_OK)
        return;
    portENTER_CRITICAL(&state.frame_lock);
    state.last_error = error;
    portEXIT_CRITICAL(&state.frame_lock);
}

esp_err_t stopStreamingInternal(CameraState &state)
{
    esp_err_t first_error = ESP_OK;
    if (state.camera && state.camera->isStreaming())
    {
        preserveFirstError(pdlError(state.camera->stopStreaming(), "stop OV2710 stream"),
                           first_error);
    }
    if (state.controller != nullptr && state.controller_started)
    {
        const esp_err_t error = esp_cam_ctlr_stop(state.controller);
        preserveFirstError(error, first_error);
        if (error == ESP_OK)
        {
            state.controller_started = false;
            portENTER_CRITICAL(&state.frame_lock);
            state.dma_frame = nullptr;
            state.latest_frame = nullptr;
            state.latest_frame_size = 0;
            portEXIT_CRITICAL(&state.frame_lock);
            (void)xSemaphoreTake(state.frame_ready, 0);
        }
    }
    recordCameraError(state, first_error);
    return first_error;
}

esp_err_t startStreamingInternal(CameraState &state)
{
    if (!state.initialized || state.controller == nullptr || !state.controller_enabled ||
        state.isp_processor == nullptr || !state.isp_enabled || !state.camera ||
        !state.camera->isInitialized())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (state.controller_started && state.camera->isStreaming())
        return ESP_OK;

    if (state.camera->isStreaming())
    {
        const esp_err_t error = pdlError(state.camera->stopStreaming(), "stop OV2710 stream");
        if (error != ESP_OK)
        {
            recordCameraError(state, error);
            return error;
        }
    }

    esp_err_t error = esp_cam_ctlr_start(state.controller);
    if (error == ESP_OK)
        state.controller_started = true;
    if (error == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(kCsiStartSettleMilliseconds));
        error = pdlError(state.camera->startStreaming(), "start OV2710 stream");
    }
    if (error != ESP_OK)
    {
        if (state.controller_started)
        {
            const esp_err_t stop_error = esp_cam_ctlr_stop(state.controller);
            if (stop_error == ESP_OK)
                state.controller_started = false;
        }
        recordCameraError(state, error);
        return error;
    }
    return ESP_OK;
}
}

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    Board::Board() = default;

    Board::~Board()
    {
        const esp_err_t error = deinitialize();
        if (error == ESP_ERR_INVALID_STATE && camera_state_)
        {
            // Destruction cannot leave DMA resources behind. Applications
            // should release acquired frames before destroying the board.
            portENTER_CRITICAL(&camera_state_->frame_lock);
            camera_state_->active_frame = nullptr;
            portEXIT_CRITICAL(&camera_state_->frame_lock);
            (void)deinitialize();
        }
    }

    esp_err_t Board::initializeCamera()
    {
        return initializeCamera(CameraConfig{});
    }

    esp_err_t Board::initializeCamera(const CameraConfig &config)
    {
        if (isCameraInitialized())
            return ESP_OK;
        if (camera_state_)
            return ESP_ERR_INVALID_STATE;

        const pdl::camera::Ov2710ModeInfo mode_info = pdl::camera::ov2710ModeInfo(config.mode);
        if (!validCameraConfig(config))
        {
            return ESP_ERR_INVALID_ARG;
        }
        const auto input_format = cameraColorFormat(config.input_data_color_type).value();
        const auto output_format = cameraColorFormat(config.output_data_color_type).value();

        ESP_RETURN_ON_ERROR(initializePower(), kTag, "initialize device power failed");

        camera_state_.reset(new (std::nothrow) CameraState{});
        if (!camera_state_)
            return ESP_ERR_NO_MEM;
        CameraState &state = *camera_state_;
        state.config = config;
        state.mode_info = mode_info;
        state.capture_buffer_count = config.capture_buffer_count;
        state.capture_buffer_size =
            static_cast<std::size_t>(mode_info.width) * mode_info.height *
            (output_format.bits_per_pixel / 8U);

        const auto fail = [this](esp_err_t failure) {
            const esp_err_t cleanup = deinitializeCamera();
            if (cleanup != ESP_OK)
            {
                ESP_LOGE(kTag, "camera cleanup failed after initialization error: %s",
                         esp_err_to_name(cleanup));
            }
            return failure;
        };

        state.frame_ready = xSemaphoreCreateBinary();
        if (state.frame_ready == nullptr)
            return fail(ESP_ERR_NO_MEM);

        state.power_transport.emplace(i2c_bus_, pdl::power::Sgm38121::defaultI2cAddress,
                                      pdl::power::Sgm38121::defaultI2cFrequencyHz);
        state.power.emplace(*state.power_transport);
        esp_err_t error = pdlError(state.power->initialize(), "initialize SGM38121");
        if (error == ESP_OK)
        {
            error = pdlError(state.power->setOutputVoltageMv(pdl::power::Sgm38121Channel::dvdd1,
                                                             kCameraDvddMillivolts),
                             "set camera DVDD1");
        }
        if (error == ESP_OK)
        {
            error = pdlError(state.power->setOutputVoltageMv(pdl::power::Sgm38121Channel::avdd1,
                                                             kCameraAvdd1Millivolts),
                             "set camera AVDD1");
        }
        if (error == ESP_OK)
        {
            error = pdlError(state.power->setOutputVoltageMv(pdl::power::Sgm38121Channel::avdd2,
                                                             kCameraAvdd2Millivolts),
                             "set camera AVDD2");
        }
        if (error == ESP_OK)
        {
            error = pdlError(state.power->setSequenceSlot(pdl::power::Sgm38121Channel::dvdd1,
                                                          pdl::power::Sgm38121SequenceSlot::slot_1),
                             "set camera DVDD1 sequence");
        }
        if (error == ESP_OK)
        {
            error = pdlError(state.power->setSequenceSlot(pdl::power::Sgm38121Channel::avdd1,
                                                          pdl::power::Sgm38121SequenceSlot::slot_2),
                             "set camera AVDD1 sequence");
        }
        if (error == ESP_OK)
        {
            error = pdlError(state.power->setSequenceSlot(pdl::power::Sgm38121Channel::avdd2,
                                                          pdl::power::Sgm38121SequenceSlot::slot_3),
                             "set camera AVDD2 sequence");
        }
        if (error == ESP_OK)
            error = pdlError(state.power->powerUpSequence(), "power up camera rails");
        if (error != ESP_OK)
            return fail(error);
        vTaskDelay(pdMS_TO_TICKS(kCameraPowerSettleMilliseconds));

        state.camera_transport.emplace(i2c_bus_, pdl::camera::Ov2710::defaultI2cAddress,
                                       pdl::camera::Ov2710::defaultI2cFrequencyHz);
        state.camera.emplace(*state.camera_transport, task_delay_);
        pdl::camera::Ov2710Config sensor_config{};
        sensor_config.mode = config.mode;
        sensor_config.auto_exposure_target = config.auto_exposure_target;
        error = pdlError(state.camera->initialize(sensor_config), "initialize OV2710");
        if (error != ESP_OK)
            return fail(error);

        esp_cam_ctlr_csi_config_t csi_config{};
        csi_config.ctlr_id = 0;
        csi_config.clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT;
        csi_config.h_res = mode_info.width;
        csi_config.v_res = mode_info.height;
        csi_config.data_lane_num = mode_info.mipi_lane_count;
        csi_config.lane_bit_rate_mbps = mode_info.mipi_lane_rate_bps / 1'000'000U;
        csi_config.input_data_color_type = config.input_data_color_type;
        csi_config.output_data_color_type = config.output_data_color_type;
        csi_config.queue_items = 1;
        csi_config.byte_swap_en = false;
        csi_config.bk_buffer_dis = true;
        error = esp_cam_new_csi_ctlr(&csi_config, &state.controller);
        if (error != ESP_OK)
            return fail(error);

        bool used_non_dma_psram = false;
        for (std::size_t index = 0; index < state.capture_buffer_count; ++index)
        {
            state.capture_buffers[index] = static_cast<std::uint8_t *>(esp_cam_ctlr_alloc_buffer(
                state.controller, state.capture_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
            if (state.capture_buffers[index] == nullptr && config.allow_non_dma_psram_fallback)
            {
                state.capture_buffers[index] = static_cast<std::uint8_t *>(
                    esp_cam_ctlr_alloc_buffer(state.controller, state.capture_buffer_size,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                used_non_dma_psram = true;
            }
            if (state.capture_buffers[index] == nullptr)
                return fail(ESP_ERR_NO_MEM);
        }

        esp_cam_ctlr_evt_cbs_t callbacks{};
        callbacks.on_get_new_trans = provideCaptureBuffer;
        callbacks.on_trans_finished = finishCapture;
        error = esp_cam_ctlr_register_event_callbacks(state.controller, &callbacks, &state);
        if (error == ESP_OK)
            error = esp_cam_ctlr_enable(state.controller);
        if (error != ESP_OK)
            return fail(error);
        state.controller_enabled = true;

        esp_isp_processor_cfg_t isp_config{};
        isp_config.clk_src = ISP_CLK_SRC_DEFAULT;
        isp_config.clk_hz = config.isp_clock_hz;
        isp_config.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
        isp_config.input_data_color_type = input_format.isp_color;
        isp_config.output_data_color_type = output_format.isp_color;
        isp_config.has_line_start_packet = false;
        isp_config.has_line_end_packet = false;
        isp_config.h_res = mode_info.width;
        isp_config.v_res = mode_info.height;
        isp_config.bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR;
        error = esp_isp_new_processor(&isp_config, &state.isp_processor);
        if (error == ESP_OK)
            error = esp_isp_enable(state.isp_processor);
        if (error != ESP_OK)
            return fail(error);
        state.isp_enabled = true;

        state.initialized = true;
        ESP_LOGI(kTag,
                 "camera initialized: %ux%u %s %u FPS -> %s, %u lane at %lu Mbps, %u "
                 "buffers of %u bytes",
                 static_cast<unsigned int>(mode_info.width),
                 static_cast<unsigned int>(mode_info.height),
                 input_format.name,
                 static_cast<unsigned int>(mode_info.frames_per_second),
                 output_format.name,
                 static_cast<unsigned int>(mode_info.mipi_lane_count),
                 static_cast<unsigned long>(mode_info.mipi_lane_rate_bps / 1'000'000U),
                 static_cast<unsigned int>(state.capture_buffer_count),
                 static_cast<unsigned int>(state.capture_buffer_size));
        if (used_non_dma_psram)
        {
            ESP_LOGW(kTag,
                     "camera buffers use general PSRAM because DMA-capable PSRAM was unavailable");
        }
        return ESP_OK;
    }

    esp_err_t Board::deinitializeCamera()
    {
        if (!camera_state_)
            return ESP_OK;

        CameraState &state = *camera_state_;
        portENTER_CRITICAL(&state.frame_lock);
        const bool frame_in_use = state.active_frame != nullptr;
        portEXIT_CRITICAL(&state.frame_lock);
        if (frame_in_use)
            return ESP_ERR_INVALID_STATE;

        esp_err_t first_error = ESP_OK;
        preserveFirstError(stopStreamingInternal(state), first_error);
        if (state.controller != nullptr && state.controller_enabled)
        {
            const esp_err_t error = esp_cam_ctlr_disable(state.controller);
            preserveFirstError(error, first_error);
            if (error == ESP_OK)
                state.controller_enabled = false;
        }
        if (state.isp_processor != nullptr && state.isp_enabled)
        {
            const esp_err_t error = esp_isp_disable(state.isp_processor);
            preserveFirstError(error, first_error);
            if (error == ESP_OK)
                state.isp_enabled = false;
        }
        if (state.controller != nullptr)
        {
            preserveFirstError(esp_cam_ctlr_del(state.controller), first_error);
            state.controller = nullptr;
        }
        if (state.isp_processor != nullptr)
        {
            preserveFirstError(esp_isp_del_processor(state.isp_processor), first_error);
            state.isp_processor = nullptr;
        }
        for (auto *&buffer : state.capture_buffers)
        {
            if (buffer != nullptr)
            {
                heap_caps_free(buffer);
                buffer = nullptr;
            }
        }
        if (state.frame_ready != nullptr)
        {
            vSemaphoreDelete(state.frame_ready);
            state.frame_ready = nullptr;
        }
        if (state.camera && state.camera->isInitialized())
        {
            preserveFirstError(detail::toEspError(state.camera->deinitialize()), first_error);
        }
        state.camera.reset();
        state.camera_transport.reset();

        if (state.power && state.power->isInitialized())
        {
            const esp_err_t power_error = shutdownCameraPower(*state.power, *state.power_transport);
            preserveFirstError(power_error, first_error);
            if (power_error != ESP_OK)
                return first_error; // Retain the transport so shutdown can be retried.
            preserveFirstError(detail::toEspError(state.power->deinitialize()), first_error);
        }
        state.power.reset();
        state.power_transport.reset();
        camera_state_.reset();
        return first_error;
    }

    bool Board::isCameraInitialized() const noexcept
    {
        return camera_state_ && camera_state_->initialized && camera_state_->camera &&
               camera_state_->camera->isInitialized() && camera_state_->controller != nullptr &&
               camera_state_->isp_processor != nullptr;
    }

    void Board::populateCameraDevices(Devices &devices) noexcept
    {
        if (!isCameraInitialized())
            return;
        devices.ov2710 = &*camera_state_->camera;
        if (camera_state_->power && camera_state_->power->isInitialized())
            devices.sgm38121 = &*camera_state_->power;
    }

    bool Board::isCameraStreaming() const noexcept
    {
        return isCameraInitialized() && camera_state_->camera->isStreaming() &&
               camera_state_->controller_started;
    }

    CameraConfig Board::cameraConfig() const noexcept
    {
        return isCameraInitialized() ? camera_state_->config : CameraConfig{};
    }

    esp_err_t Board::startCameraStreaming()
    {
        if (!camera_state_ || !camera_state_->initialized)
            return ESP_ERR_INVALID_STATE;
        return startStreamingInternal(*camera_state_);
    }

    esp_err_t Board::stopCameraStreaming()
    {
        if (!camera_state_ || !camera_state_->initialized)
            return ESP_ERR_INVALID_STATE;
        return stopStreamingInternal(*camera_state_);
    }

    esp_err_t Board::reconfigureCamera(const CameraConfig &config)
    {
        if (!validCameraConfig(config))
            return ESP_ERR_INVALID_ARG;
        if (!camera_state_ || !camera_state_->initialized)
            return ESP_ERR_INVALID_STATE;

        CameraState &current = *camera_state_;
        portENTER_CRITICAL(&current.frame_lock);
        const bool frame_in_use = current.active_frame != nullptr;
        portEXIT_CRITICAL(&current.frame_lock);
        if (frame_in_use)
            return ESP_ERR_INVALID_STATE;

        const CameraConfig previous_config = current.config;
        const bool was_streaming = isCameraStreaming();
        esp_err_t error = deinitializeCamera();
        if (error != ESP_OK)
            return error;

        error = initializeCamera(config);
        if (error == ESP_OK)
        {
            if (was_streaming)
                error = startCameraStreaming();
            return error;
        }

        ESP_LOGE(kTag, "camera reconfiguration failed: %s; attempting rollback",
                 esp_err_to_name(error));
        const esp_err_t rollback = initializeCamera(previous_config);
        if (rollback == ESP_OK && was_streaming)
            (void)startCameraStreaming();
        if (rollback != ESP_OK)
        {
            ESP_LOGE(kTag, "camera rollback failed: %s", esp_err_to_name(rollback));
        }
        return error;
    }

    esp_err_t Board::recoverCameraPipeline()
    {
        if (!camera_state_ || !camera_state_->initialized)
            return ESP_ERR_INVALID_STATE;

        const CameraConfig config = camera_state_->config;
        const std::uint32_t next_recovery = camera_state_->recovery_count + 1U;
        const esp_err_t error = reconfigureCamera(config);
        if (camera_state_)
        {
            portENTER_CRITICAL(&camera_state_->frame_lock);
            if (error == ESP_OK)
                camera_state_->recovery_count = next_recovery;
            else
                ++camera_state_->recovery_failures;
            camera_state_->last_error = error;
            portEXIT_CRITICAL(&camera_state_->frame_lock);
        }
        return error;
    }

    esp_err_t Board::readCameraProductId(std::uint16_t &product_id, std::uint32_t timeout_ms)
    {
        if (!camera_state_ || !camera_state_->initialized || !camera_state_->camera)
        {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t error = pdlError(
            camera_state_->camera->readProductId(product_id, timeout_ms), "read OV2710 product ID");
        recordCameraError(*camera_state_, error);
        return error;
    }

    esp_err_t Board::softwareResetCameraSensor(std::uint32_t timeout_ms)
    {
        if (!camera_state_ || !camera_state_->initialized || !camera_state_->camera)
        {
            return ESP_ERR_INVALID_STATE;
        }
        if (isCameraStreaming())
            return ESP_ERR_INVALID_STATE;

        CameraState &state = *camera_state_;
        esp_err_t error = pdlError(state.camera->softwareReset(timeout_ms), "reset OV2710");
        if (error == ESP_OK)
            error = pdlError(state.camera->configure(state.config.mode, timeout_ms),
                             "reconfigure OV2710 after reset");
        if (error == ESP_OK && state.config.auto_exposure_target != 0x30U)
        {
            error = pdlError(
                state.camera->setAutoExposureTarget(state.config.auto_exposure_target, timeout_ms),
                "restore OV2710 exposure after reset");
        }
        recordCameraError(state, error);
        return error;
    }

    esp_err_t Board::setCameraTestPattern(bool enabled, std::uint32_t timeout_ms)
    {
        if (!camera_state_ || !camera_state_->initialized || !camera_state_->camera)
        {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t error = pdlError(camera_state_->camera->setTestPattern(enabled, timeout_ms),
                                         "set OV2710 test pattern");
        recordCameraError(*camera_state_, error);
        return error;
    }

    esp_err_t Board::setCameraHorizontalMirror(bool enabled, std::uint32_t timeout_ms)
    {
        if (!camera_state_ || !camera_state_->initialized || !camera_state_->camera)
        {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t error =
            pdlError(camera_state_->camera->setHorizontalMirror(enabled, timeout_ms),
                     "set OV2710 horizontal mirror");
        recordCameraError(*camera_state_, error);
        return error;
    }

    esp_err_t Board::setCameraVerticalFlip(bool enabled, std::uint32_t timeout_ms)
    {
        if (!camera_state_ || !camera_state_->initialized || !camera_state_->camera)
        {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t error =
            pdlError(camera_state_->camera->setVerticalFlip(enabled, timeout_ms),
                     "set OV2710 vertical flip");
        recordCameraError(*camera_state_, error);
        return error;
    }

    esp_err_t Board::setCameraAutoExposureTarget(std::uint8_t target, std::uint32_t timeout_ms)
    {
        if (!camera_state_ || !camera_state_->initialized || !camera_state_->camera)
        {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t error =
            pdlError(camera_state_->camera->setAutoExposureTarget(target, timeout_ms),
                     "set OV2710 exposure target");
        if (error == ESP_OK)
            camera_state_->config.auto_exposure_target = target;
        recordCameraError(*camera_state_, error);
        return error;
    }

    esp_err_t Board::readCameraSensorStatistics(pdl::camera::Ov2710Statistics &statistics,
                                                std::uint32_t timeout_ms)
    {
        if (!camera_state_ || !camera_state_->initialized || !camera_state_->camera)
        {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t error =
            pdlError(camera_state_->camera->readStatistics(statistics, timeout_ms),
                     "read OV2710 statistics");
        recordCameraError(*camera_state_, error);
        return error;
    }

    esp_err_t Board::acquireCameraFrame(CameraFrame &frame, std::uint32_t timeout_ms)
    {
        frame = {};
        if (!isCameraStreaming())
            return ESP_ERR_INVALID_STATE;
        CameraState &state = *camera_state_;

        portENTER_CRITICAL(&state.frame_lock);
        const bool already_acquired = state.active_frame != nullptr;
        portEXIT_CRITICAL(&state.frame_lock);
        if (already_acquired)
            return ESP_ERR_INVALID_STATE;

        if (xSemaphoreTake(state.frame_ready, timeoutTicks(timeout_ms)) != pdTRUE)
            return ESP_ERR_TIMEOUT;

        portENTER_CRITICAL(&state.frame_lock);
        frame.data = state.latest_frame;
        frame.size = state.latest_frame_size;
        state.latest_frame = nullptr;
        state.latest_frame_size = 0;
        state.active_frame = frame.data;
        portEXIT_CRITICAL(&state.frame_lock);
        return frame.data != nullptr ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    esp_err_t Board::releaseCameraFrame(const std::uint8_t *frame)
    {
        if (!isCameraInitialized() || frame == nullptr)
            return ESP_ERR_INVALID_ARG;
        CameraState &state = *camera_state_;

        portENTER_CRITICAL(&state.frame_lock);
        const bool matches = state.active_frame == frame;
        if (matches)
            state.active_frame = nullptr;
        portEXIT_CRITICAL(&state.frame_lock);
        return matches ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    pdl::camera::Ov2710ModeInfo Board::cameraModeInfo() const noexcept
    {
        return isCameraInitialized() ? camera_state_->mode_info : pdl::camera::Ov2710ModeInfo{};
    }

    CameraStatistics Board::cameraStatistics() const noexcept
    {
        CameraStatistics statistics{};
        if (!camera_state_)
            return statistics;

        CameraState &state = *camera_state_;
        portENTER_CRITICAL(&state.frame_lock);
        statistics.received_frames = state.received_frames;
        statistics.dropped_frames = state.dropped_frames;
        statistics.recovery_count = state.recovery_count;
        statistics.recovery_failures = state.recovery_failures;
        statistics.last_error = state.last_error;
        portEXIT_CRITICAL(&state.frame_lock);
        return statistics;
    }
}
