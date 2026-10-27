#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace bsp::t_panel_p4_rect::pins
{
constexpr gpio_num_t i2cSda = GPIO_NUM_3;
constexpr gpio_num_t i2cScl = GPIO_NUM_2;

constexpr gpio_num_t i2sDataIn = GPIO_NUM_28;
constexpr gpio_num_t i2sWordSelect = GPIO_NUM_29;
constexpr gpio_num_t i2sDataOut = GPIO_NUM_26;
constexpr gpio_num_t i2sBitClock = GPIO_NUM_27;
constexpr gpio_num_t i2sMasterClock = GPIO_NUM_30;

constexpr gpio_num_t touchInterrupt = GPIO_NUM_4;
constexpr gpio_num_t axp517Interrupt = GPIO_NUM_5;
constexpr gpio_num_t button = GPIO_NUM_6;
constexpr gpio_num_t lcdTearingEffect = GPIO_NUM_20;
constexpr gpio_num_t motorEnable = GPIO_NUM_21;
constexpr gpio_num_t lcdBacklightPwm = GPIO_NUM_23;
constexpr gpio_num_t lcdReset = GPIO_NUM_45;
constexpr gpio_num_t touchReset = GPIO_NUM_22;

constexpr gpio_num_t usbDeviceN = GPIO_NUM_24;
constexpr gpio_num_t usbDeviceP = GPIO_NUM_25;
constexpr gpio_num_t boot = GPIO_NUM_35;
constexpr gpio_num_t audioPowerEnable = GPIO_NUM_31;
constexpr gpio_num_t cameraFlash2Enable = GPIO_NUM_47;
constexpr gpio_num_t cameraFlash1Enable = GPIO_NUM_48;
constexpr gpio_num_t qmi8658cInterrupt = GPIO_NUM_49;

constexpr gpio_num_t sd1Data0 = GPIO_NUM_39;
constexpr gpio_num_t sd1Data1 = GPIO_NUM_40;
constexpr gpio_num_t sd1Data2 = GPIO_NUM_41;
constexpr gpio_num_t sd1Data3 = GPIO_NUM_42;
constexpr gpio_num_t sd1Clock = GPIO_NUM_43;
constexpr gpio_num_t sd1Command = GPIO_NUM_44;

// The Rect panel reset is implemented by power-cycling the display rail.
constexpr gpio_num_t lcdPower = GPIO_NUM_45;

}
