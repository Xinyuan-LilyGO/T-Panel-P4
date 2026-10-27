#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <RadioLib.h>

#include <pdl/ports/esp_idf/spi_bus.hpp>

namespace bsp::t_panel_p4_standard::detail
{
class RadioLibEspIdfHal final : public RadioLibHal
{
public:
    RadioLibEspIdfHal(
        pdl::esp_idf::SpiBus &bus,
        std::uint32_t frequency_hz) noexcept;
    ~RadioLibEspIdfHal() override;

    void init() override;
    void term() override;

    void pinMode(std::uint32_t pin, std::uint32_t mode) override;
    void digitalWrite(std::uint32_t pin, std::uint32_t value) override;
    std::uint32_t digitalRead(std::uint32_t pin) override;
    void attachInterrupt(
        std::uint32_t interrupt_number,
        void (*callback)(void),
        std::uint32_t mode) override;
    void detachInterrupt(std::uint32_t interrupt_number) override;

    void delay(RadioLibTime_t milliseconds) override;
    void delayMicroseconds(RadioLibTime_t microseconds) override;
    RadioLibTime_t millis() override;
    RadioLibTime_t micros() override;
    long pulseIn(
        std::uint32_t pin,
        std::uint32_t state,
        RadioLibTime_t timeout) override;

    void spiBegin() override;
    void spiBeginTransaction() override;
    void spiTransfer(
        std::uint8_t *output,
        std::size_t length,
        std::uint8_t *input) override;
    void spiEndTransaction() override;
    void spiEnd() override;

    [[nodiscard]] esp_err_t lastError() const noexcept;

private:
    static void gpioIsr(void *argument);
    void recordError(esp_err_t error) noexcept;
    [[nodiscard]] bool timedOut(
        std::uint64_t start_us,
        RadioLibTime_t timeout_us) const noexcept;

    pdl::esp_idf::SpiBus *bus_;
    std::uint32_t frequency_hz_;
    spi_device_handle_t spi_device_{};
    gpio_num_t interrupt_gpio_{GPIO_NUM_NC};
    void (*interrupt_callback_)(void){};
    esp_err_t last_error_{ESP_OK};
    bool bus_acquired_{};
};
}
