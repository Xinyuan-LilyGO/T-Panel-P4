#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace bsp::t_panel_p4_round::pins
{
constexpr gpio_num_t i2cSda = GPIO_NUM_3;
constexpr gpio_num_t i2cScl = GPIO_NUM_2;

constexpr gpio_num_t spiSck = GPIO_NUM_28;
constexpr gpio_num_t spiMiso = GPIO_NUM_27;
constexpr gpio_num_t spiMosi = GPIO_NUM_26;

constexpr gpio_num_t i2sDataOut = GPIO_NUM_9;
constexpr gpio_num_t i2sWordSelect = GPIO_NUM_10;
constexpr gpio_num_t i2sDataIn = GPIO_NUM_11;
constexpr gpio_num_t i2sBitClock = GPIO_NUM_12;
constexpr gpio_num_t i2sMasterClock = GPIO_NUM_13;

constexpr std::uint16_t xl9555Address = 0x20;
constexpr std::uint8_t speakerControl = 0;
constexpr std::uint8_t sdPowerEnable = 1;
constexpr std::uint8_t lcdReset = 3;
constexpr std::uint8_t touchReset = 4;
constexpr std::uint8_t loraPowerEnable = 5;

constexpr gpio_num_t axp517Interrupt = GPIO_NUM_4;
constexpr std::uint16_t sgm38121Address = 0x28;

constexpr gpio_num_t loraChipSelect = GPIO_NUM_29;
constexpr gpio_num_t loraBusy = GPIO_NUM_30;
constexpr gpio_num_t loraReset = GPIO_NUM_31;
constexpr gpio_num_t loraInterrupt = GPIO_NUM_5;
constexpr std::uint8_t loraIrqDio = 10;

constexpr gpio_num_t lcdTearingEffect = GPIO_NUM_33;
constexpr gpio_num_t lcdBacklightPwm = GPIO_NUM_34;
constexpr gpio_num_t touchInterrupt = GPIO_NUM_6;
constexpr gpio_num_t boot_button = GPIO_NUM_35;
constexpr gpio_num_t button = GPIO_NUM_36;
constexpr gpio_num_t esp32c5Enable = GPIO_NUM_54;

constexpr gpio_num_t sd1Command = GPIO_NUM_44;
constexpr gpio_num_t sd1Clock = GPIO_NUM_43;
constexpr gpio_num_t sd1Data0 = GPIO_NUM_39;
constexpr gpio_num_t sd1Data1 = GPIO_NUM_40;
constexpr gpio_num_t sd1Data2 = GPIO_NUM_41;
constexpr gpio_num_t sd1Data3 = GPIO_NUM_42;

constexpr gpio_num_t sd2Command = GPIO_NUM_19;
constexpr gpio_num_t sd2Clock = GPIO_NUM_18;
constexpr gpio_num_t sd2Data0 = GPIO_NUM_14;
constexpr gpio_num_t sd2Data1 = GPIO_NUM_15;
constexpr gpio_num_t sd2Data2 = GPIO_NUM_16;
constexpr gpio_num_t sd2Data3 = GPIO_NUM_17;
}
