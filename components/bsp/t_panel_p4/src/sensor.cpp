#include <bsp/api/board.hpp>

#if T_PANEL_P4_BSP_IS_RECT

#include <limits>

#include <esp_check.h>
#include <esp_log.h>

#include "bsp_private.hpp"

namespace
{
    constexpr char kTag[] = "t_panel_p4_sensor";

    bool isValidTimeout(std::uint32_t timeout_ms) noexcept
    {
        return timeout_ms > 0U &&
               timeout_ms <= static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    }

    bool isValidBusConfig(std::uint32_t frequency_hz, std::uint32_t timeout_ms) noexcept
    {
        return frequency_hz > 0U && frequency_hz <= 400000U && isValidTimeout(timeout_ms);
    }
}

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    esp_err_t Board::initializeSensors()
    {
        return initializeSensors(SensorConfig{});
    }

    esp_err_t Board::initializeSensors(const SensorConfig &config)
    {
        const bool had_qmi8658c = isQmi8658cInitialized();
        ESP_RETURN_ON_ERROR(initializeQmi8658c(config.qmi8658c), kTag, "initialize IMU failed");
        const esp_err_t error = initializeQmc6309(config.qmc6309);
        if (error != ESP_OK && !had_qmi8658c)
        {
            const esp_err_t cleanup = deinitializeQmi8658c();
            if (cleanup != ESP_OK)
                ESP_LOGE(kTag, "rollback QMI8658C failed: %s", esp_err_to_name(cleanup));
        }
        return error;
    }

    esp_err_t Board::deinitializeSensors()
    {
        // Attempt both even when one sensor fails to enter its low-power mode.
        const esp_err_t qmc_error = deinitializeQmc6309();
        const esp_err_t qmi_error = deinitializeQmi8658c();
        return qmc_error != ESP_OK ? qmc_error : qmi_error;
    }

    bool Board::isSensorsInitialized() const noexcept
    {
        return isQmi8658cInitialized() && isQmc6309Initialized();
    }

    esp_err_t Board::initializeQmi8658c()
    {
        return initializeQmi8658c(Qmi8658cConfig{});
    }

    esp_err_t Board::initializeQmi8658c(const Qmi8658cConfig &config)
    {
        if (isQmi8658cInitialized())
            return ESP_OK;
        ESP_RETURN_ON_FALSE(
            (config.i2c_address == pdl::sensor::Qmi8658c::lowI2cAddress ||
             config.i2c_address == pdl::sensor::Qmi8658c::highI2cAddress) &&
                isValidBusConfig(config.i2c_frequency_hz, config.timeout_ms),
            ESP_ERR_INVALID_ARG, kTag, "invalid QMI8658C I2C configuration");
        ESP_RETURN_ON_ERROR(initializePower(), kTag, "initialize sensor power/I2C failed");

        auto address = config.i2c_address;
        esp_err_t probe_error = i2c_master_probe(i2c_bus_.nativeHandle(), address,
                                                static_cast<int>(config.timeout_ms));
        if (probe_error == ESP_ERR_NOT_FOUND && config.try_alternate_i2c_address)
        {
            address = address == pdl::sensor::Qmi8658c::lowI2cAddress
                          ? pdl::sensor::Qmi8658c::highI2cAddress
                          : pdl::sensor::Qmi8658c::lowI2cAddress;
            probe_error = i2c_master_probe(i2c_bus_.nativeHandle(), address,
                                           static_cast<int>(config.timeout_ms));
        }
        ESP_RETURN_ON_ERROR(probe_error, kTag, "probe QMI8658C failed");

        qmi8658c_transport_.emplace(i2c_bus_, address, config.i2c_frequency_hz);
        qmi8658c_.emplace(*qmi8658c_transport_, task_delay_);
        const esp_err_t error = detail::toEspError(qmi8658c_->initialize(config.sensor, config.timeout_ms));
        if (error != ESP_OK)
        {
            // PDL handles failed initialization; destroy the transport as a final
            // safeguard, without disturbing any other device on the shared bus.
            qmi8658c_.reset();
            qmi8658c_transport_.reset();
            ESP_LOGE(kTag, "QMI8658C initialization failed: %s", esp_err_to_name(error));
            return error;
        }
        ESP_LOGI(kTag, "QMI8658C initialized at 0x%02X, %lu Hz (polling)",
                 static_cast<unsigned>(address),
                 static_cast<unsigned long>(config.i2c_frequency_hz));
        return ESP_OK;
    }

    esp_err_t Board::deinitializeQmi8658c()
    {
        esp_err_t first_error = ESP_OK;
        if (isQmi8658cInitialized())
        {
            // PDL deinitialize() only releases the transport; power down first.
            first_error = detail::toEspError(qmi8658c_->setPowerEnabled(false));
            const esp_err_t error = detail::toEspError(qmi8658c_->deinitialize());
            if (first_error == ESP_OK)
                first_error = error;
        }
        qmi8658c_.reset();
        qmi8658c_transport_.reset();
        return first_error;
    }

    bool Board::isQmi8658cInitialized() const noexcept
    {
        return qmi8658c_ && qmi8658c_->isInitialized();
    }

    esp_err_t Board::initializeQmc6309()
    {
        return initializeQmc6309(Qmc6309Config{});
    }

    esp_err_t Board::initializeQmc6309(const Qmc6309Config &config)
    {
        if (isQmc6309Initialized())
            return ESP_OK;
        ESP_RETURN_ON_FALSE(
            config.i2c_address == pdl::sensor::Qmc6309::defaultI2cAddress &&
                isValidBusConfig(config.i2c_frequency_hz, config.timeout_ms),
            ESP_ERR_INVALID_ARG, kTag, "invalid QMC6309 I2C configuration");
        ESP_RETURN_ON_ERROR(initializePower(), kTag, "initialize sensor power/I2C failed");

        qmc6309_transport_.emplace(i2c_bus_, config.i2c_address, config.i2c_frequency_hz);
        qmc6309_.emplace(*qmc6309_transport_, task_delay_);
        const esp_err_t error = detail::toEspError(qmc6309_->initialize(config.sensor, config.timeout_ms));
        if (error != ESP_OK)
        {
            qmc6309_.reset();
            qmc6309_transport_.reset();
            ESP_LOGE(kTag, "QMC6309 initialization failed: %s", esp_err_to_name(error));
            return error;
        }
        ESP_LOGI(kTag, "QMC6309 initialized at 0x%02X, %lu Hz (polling)",
                 static_cast<unsigned>(config.i2c_address),
                 static_cast<unsigned long>(config.i2c_frequency_hz));
        return ESP_OK;
    }

    esp_err_t Board::deinitializeQmc6309()
    {
        esp_err_t first_error = ESP_OK;
        if (isQmc6309Initialized())
        {
            first_error = detail::toEspError(qmc6309_->setOperationMode(
                pdl::sensor::Qmc6309OperationMode::suspend));
            const esp_err_t error = detail::toEspError(qmc6309_->deinitialize());
            if (first_error == ESP_OK)
                first_error = error;
        }
        qmc6309_.reset();
        qmc6309_transport_.reset();
        return first_error;
    }

    bool Board::isQmc6309Initialized() const noexcept
    {
        return qmc6309_ && qmc6309_->isInitialized();
    }

    esp_err_t Board::readQmi8658cDataReady(pdl::sensor::Qmi8658cDataReady &ready,
                                          std::uint32_t timeout_ms) const
    {
        if (!isQmi8658cInitialized())
            return ESP_ERR_INVALID_STATE;
        if (!isValidTimeout(timeout_ms))
            return ESP_ERR_INVALID_ARG;
        return detail::toEspError(qmi8658c_->readDataReady(ready, timeout_ms));
    }

    esp_err_t Board::readQmi8658cMeasurement(pdl::sensor::Qmi8658cMeasurement &measurement,
                                            std::uint32_t timeout_ms) const
    {
        if (!isQmi8658cInitialized())
            return ESP_ERR_INVALID_STATE;
        if (!isValidTimeout(timeout_ms))
            return ESP_ERR_INVALID_ARG;
        return detail::toEspError(qmi8658c_->readMeasurement(measurement, timeout_ms));
    }

    esp_err_t Board::readQmc6309Status(pdl::sensor::Qmc6309Status &status,
                                       std::uint32_t timeout_ms)
    {
        if (!isQmc6309Initialized())
            return ESP_ERR_INVALID_STATE;
        if (!isValidTimeout(timeout_ms))
            return ESP_ERR_INVALID_ARG;
        return detail::toEspError(qmc6309_->readStatus(status, timeout_ms));
    }

    esp_err_t Board::readQmc6309Measurement(pdl::sensor::Qmc6309Measurement &measurement,
                                            std::uint32_t timeout_ms)
    {
        if (!isQmc6309Initialized())
            return ESP_ERR_INVALID_STATE;
        if (!isValidTimeout(timeout_ms))
            return ESP_ERR_INVALID_ARG;
        return detail::toEspError(qmc6309_->readMeasurement(measurement, timeout_ms));
    }

    pdl::sensor::Qmi8658c *Board::qmi8658c() noexcept
    {
        return isQmi8658cInitialized() ? &*qmi8658c_ : nullptr;
    }

    pdl::sensor::Qmc6309 *Board::qmc6309() noexcept
    {
        return isQmc6309Initialized() ? &*qmc6309_ : nullptr;
    }
}

#endif
