#include "radiolib_esp_idf_hal.hpp"

#include <cstring>

#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace bsp::t_panel_p4_round::detail
{
RadioLibEspIdfHal::RadioLibEspIdfHal(
    pdl::esp_idf::SpiBus &bus,
    std::uint32_t frequency_hz) noexcept
    : RadioLibHal(
          static_cast<std::uint32_t>(GPIO_MODE_INPUT),
          static_cast<std::uint32_t>(GPIO_MODE_OUTPUT),
          0,
          1,
          static_cast<std::uint32_t>(GPIO_INTR_POSEDGE),
          static_cast<std::uint32_t>(GPIO_INTR_NEGEDGE)),
      bus_(&bus),
      frequency_hz_(frequency_hz)
{
}

RadioLibEspIdfHal::~RadioLibEspIdfHal()
{
    term();
}

void RadioLibEspIdfHal::init()
{
    last_error_ = ESP_OK;
    spiBegin();
}

void RadioLibEspIdfHal::term()
{
    if (interrupt_gpio_ != GPIO_NUM_NC)
        detachInterrupt(static_cast<std::uint32_t>(interrupt_gpio_));
    spiEnd();
}

void RadioLibEspIdfHal::pinMode(std::uint32_t pin, std::uint32_t mode)
{
    const int pin_number = static_cast<int>(pin);
    if (pin == RADIOLIB_NC || !GPIO_IS_VALID_GPIO(pin_number))
        return;

    gpio_config_t config{};
    config.pin_bit_mask = std::uint64_t{1} << pin;
    config.mode = static_cast<gpio_mode_t>(mode);
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    recordError(gpio_config(&config));
}

void RadioLibEspIdfHal::digitalWrite(std::uint32_t pin, std::uint32_t value)
{
    const int pin_number = static_cast<int>(pin);
    if (pin == RADIOLIB_NC || !GPIO_IS_VALID_OUTPUT_GPIO(pin_number))
        return;
    recordError(gpio_set_level(static_cast<gpio_num_t>(pin), value != 0U));
}

std::uint32_t RadioLibEspIdfHal::digitalRead(std::uint32_t pin)
{
    const int pin_number = static_cast<int>(pin);
    if (pin == RADIOLIB_NC || !GPIO_IS_VALID_GPIO(pin_number))
        return 0;
    return gpio_get_level(static_cast<gpio_num_t>(pin)) != 0 ? 1U : 0U;
}

void RadioLibEspIdfHal::attachInterrupt(
    std::uint32_t interrupt_number,
    void (*callback)(void),
    std::uint32_t mode)
{
    const int pin_number = static_cast<int>(interrupt_number);
    if (interrupt_number == RADIOLIB_NC || callback == nullptr ||
        !GPIO_IS_VALID_GPIO(pin_number))
    {
        return;
    }

    if (interrupt_gpio_ != GPIO_NUM_NC)
        detachInterrupt(static_cast<std::uint32_t>(interrupt_gpio_));

    const gpio_num_t gpio = static_cast<gpio_num_t>(interrupt_number);
    const gpio_int_type_t trigger = static_cast<gpio_int_type_t>(mode & 0x07U);
    esp_err_t error = gpio_install_isr_service(0);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    {
        recordError(error);
        return;
    }

    error = gpio_set_direction(gpio, GPIO_MODE_INPUT);
    if (error == ESP_OK)
        error = gpio_set_intr_type(gpio, trigger);
    if (error == ESP_OK)
        error = gpio_isr_handler_add(gpio, gpioIsr, this);
    if (error == ESP_OK)
        error = gpio_intr_enable(gpio);
    if (error != ESP_OK)
    {
        recordError(error);
        (void)gpio_isr_handler_remove(gpio);
        (void)gpio_set_intr_type(gpio, GPIO_INTR_DISABLE);
        return;
    }

    interrupt_gpio_ = gpio;
    interrupt_callback_ = callback;
}

void RadioLibEspIdfHal::detachInterrupt(std::uint32_t interrupt_number)
{
    const int pin_number = static_cast<int>(interrupt_number);
    if (interrupt_number == RADIOLIB_NC ||
        !GPIO_IS_VALID_GPIO(pin_number))
    {
        return;
    }

    const gpio_num_t gpio = static_cast<gpio_num_t>(interrupt_number);
    if (interrupt_gpio_ == gpio)
    {
        recordError(gpio_intr_disable(gpio));
        const esp_err_t remove_error = gpio_isr_handler_remove(gpio);
        if (remove_error != ESP_OK && remove_error != ESP_ERR_INVALID_STATE)
            recordError(remove_error);
        recordError(gpio_set_intr_type(gpio, GPIO_INTR_DISABLE));
        interrupt_gpio_ = GPIO_NUM_NC;
        interrupt_callback_ = nullptr;
    }
}

void RadioLibEspIdfHal::delay(RadioLibTime_t milliseconds)
{
    if (milliseconds == 0)
    {
        taskYIELD();
        return;
    }

    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    if (ticks == 0)
        ticks = 1;
    vTaskDelay(ticks);
}

void RadioLibEspIdfHal::delayMicroseconds(RadioLibTime_t microseconds)
{
    esp_rom_delay_us(static_cast<std::uint32_t>(microseconds));
}

RadioLibTime_t RadioLibEspIdfHal::millis()
{
    return static_cast<RadioLibTime_t>(esp_timer_get_time() / 1000ULL);
}

RadioLibTime_t RadioLibEspIdfHal::micros()
{
    return static_cast<RadioLibTime_t>(esp_timer_get_time());
}

long RadioLibEspIdfHal::pulseIn(
    std::uint32_t pin,
    std::uint32_t state,
    RadioLibTime_t timeout)
{
    const int pin_number = static_cast<int>(pin);
    if (pin == RADIOLIB_NC || !GPIO_IS_VALID_GPIO(pin_number))
        return 0;

    pinMode(pin, GpioModeInput);
    const std::uint64_t wait_start = esp_timer_get_time();
    while (digitalRead(pin) == state)
    {
        if (timedOut(wait_start, timeout))
            return 0;
    }
    while (digitalRead(pin) != state)
    {
        if (timedOut(wait_start, timeout))
            return 0;
    }

    const std::uint64_t pulse_start = esp_timer_get_time();
    while (digitalRead(pin) == state)
    {
        if (timedOut(wait_start, timeout))
            return 0;
    }
    return static_cast<long>(esp_timer_get_time() - pulse_start);
}

void RadioLibEspIdfHal::spiBegin()
{
    if (spi_device_ != nullptr)
        return;
    if (bus_ == nullptr || !bus_->isInitialized() || frequency_hz_ == 0U)
    {
        recordError(frequency_hz_ == 0U ? ESP_ERR_INVALID_ARG
                                        : ESP_ERR_INVALID_STATE);
        return;
    }

    spi_device_interface_config_t device_config{};
    device_config.clock_speed_hz = static_cast<int>(frequency_hz_);
    device_config.mode = 0;
    device_config.spics_io_num = GPIO_NUM_NC;
    device_config.queue_size = 1;

    const esp_err_t error = spi_bus_add_device(
        bus_->nativeHost(),
        &device_config,
        &spi_device_);
    if (error != ESP_OK)
        recordError(error);
}

void RadioLibEspIdfHal::spiBeginTransaction()
{
    if (spi_device_ == nullptr || bus_acquired_)
        return;

    const esp_err_t error = spi_device_acquire_bus(spi_device_, portMAX_DELAY);
    recordError(error);
    bus_acquired_ = error == ESP_OK;
}

void RadioLibEspIdfHal::spiTransfer(
    std::uint8_t *output,
    std::size_t length,
    std::uint8_t *input)
{
    if (length == 0U)
        return;
    if (spi_device_ == nullptr)
    {
        if (input != nullptr)
            std::memset(input, 0, length);
        recordError(ESP_ERR_INVALID_STATE);
        return;
    }

    spi_transaction_t transaction{};
    transaction.length = length * 8U;
    transaction.tx_buffer = output;
    transaction.rx_buffer = input;
    recordError(spi_device_polling_transmit(spi_device_, &transaction));
}

void RadioLibEspIdfHal::spiEndTransaction()
{
    if (spi_device_ != nullptr && bus_acquired_)
    {
        spi_device_release_bus(spi_device_);
        bus_acquired_ = false;
    }
}

void RadioLibEspIdfHal::spiEnd()
{
    spiEndTransaction();
    if (spi_device_ != nullptr)
    {
        recordError(spi_bus_remove_device(spi_device_));
        spi_device_ = nullptr;
    }
}

esp_err_t RadioLibEspIdfHal::lastError() const noexcept
{
    return last_error_;
}

void RadioLibEspIdfHal::gpioIsr(void *argument)
{
    auto *hal = static_cast<RadioLibEspIdfHal *>(argument);
    if (hal != nullptr && hal->interrupt_callback_ != nullptr)
        hal->interrupt_callback_();
}

void RadioLibEspIdfHal::recordError(esp_err_t error) noexcept
{
    if (last_error_ == ESP_OK && error != ESP_OK)
        last_error_ = error;
}

bool RadioLibEspIdfHal::timedOut(
    std::uint64_t start_us,
    RadioLibTime_t timeout_us) const noexcept
{
    return static_cast<std::uint64_t>(esp_timer_get_time()) - start_us >=
           static_cast<std::uint64_t>(timeout_us);
}
}
