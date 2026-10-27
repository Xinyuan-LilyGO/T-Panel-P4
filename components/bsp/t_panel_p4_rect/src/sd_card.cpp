#include <bsp/t_panel_p4_rect.hpp>

#include <climits>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sdkconfig.h>

#if CONFIG_TINYUSB_MSC_ENABLED
#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <tinyusb_msc.h>
#endif

namespace
{
constexpr char kTag[] = "t_panel_p4_sd";
constexpr int kLdoChannel = 4;
constexpr std::uint32_t kMinimumFrequencyKhz = 400;
constexpr std::uint32_t kMaximumFrequencyKhz = 40000;

#if CONFIG_TINYUSB_MSC_ENABLED
constexpr std::uint16_t kUsbVendorId = 0x303A;
constexpr std::uint16_t kUsbProductId = 0x4002;
constexpr std::uint8_t kMscOutEndpoint = 0x01;
constexpr std::uint8_t kMscInEndpoint = 0x81;
constexpr std::uint16_t kDescriptorTotalLength =
    TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN;

enum : std::uint8_t
{
    kMscInterface = 0,
    kInterfaceCount,
};

const tusb_desc_device_t kDeviceDescriptor = [] {
    tusb_desc_device_t descriptor{};
    descriptor.bLength = sizeof(descriptor);
    descriptor.bDescriptorType = TUSB_DESC_DEVICE;
    descriptor.bcdUSB = 0x0200;
    descriptor.bDeviceClass = TUSB_CLASS_MISC;
    descriptor.bDeviceSubClass = MISC_SUBCLASS_COMMON;
    descriptor.bDeviceProtocol = MISC_PROTOCOL_IAD;
    descriptor.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE;
    descriptor.idVendor = kUsbVendorId;
    descriptor.idProduct = kUsbProductId;
    descriptor.bcdDevice = 0x0100;
    descriptor.iManufacturer = 0x01;
    descriptor.iProduct = 0x02;
    descriptor.iSerialNumber = 0x03;
    descriptor.bNumConfigurations = 0x01;
    return descriptor;
}();

const std::uint8_t kFullSpeedConfigurationDescriptor[]{
    TUD_CONFIG_DESCRIPTOR(
        1,
        kInterfaceCount,
        0,
        kDescriptorTotalLength,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    TUD_MSC_DESCRIPTOR(
        kMscInterface,
        4,
        kMscOutEndpoint,
        kMscInEndpoint,
        64),
};

#if TUD_OPT_HIGH_SPEED
const tusb_desc_device_qualifier_t kDeviceQualifier = [] {
    tusb_desc_device_qualifier_t descriptor{};
    descriptor.bLength = sizeof(descriptor);
    descriptor.bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER;
    descriptor.bcdUSB = 0x0200;
    descriptor.bDeviceClass = TUSB_CLASS_MISC;
    descriptor.bDeviceSubClass = MISC_SUBCLASS_COMMON;
    descriptor.bDeviceProtocol = MISC_PROTOCOL_IAD;
    descriptor.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE;
    descriptor.bNumConfigurations = 0x01;
    return descriptor;
}();

const std::uint8_t kHighSpeedConfigurationDescriptor[]{
    TUD_CONFIG_DESCRIPTOR(
        1,
        kInterfaceCount,
        0,
        kDescriptorTotalLength,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    TUD_MSC_DESCRIPTOR(
        kMscInterface,
        4,
        kMscOutEndpoint,
        kMscInEndpoint,
        512),
};
#endif

const char kLanguageDescriptor[]{0x09, 0x04};
const char *kStringDescriptors[]{
    kLanguageDescriptor,
    "LILYGO",
    "T-Panel-P4 SD Card",
    "TPANELP4MSC",
    "SD Card",
};

void onStorageEvent(
    tinyusb_msc_storage_handle_t,
    tinyusb_msc_event_t *event,
    void *)
{
    if (event == nullptr)
        return;

    switch (event->id)
    {
    case TINYUSB_MSC_EVENT_MOUNT_START:
        ESP_LOGI(kTag, "changing SD card owner");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        ESP_LOGI(
            kTag,
            "SD card is owned by %s",
            event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB
                ? "USB host"
                : "application");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        ESP_LOGE(kTag, "MSC storage owner change failed");
        break;
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGW(kTag, "SD card has no supported filesystem");
        break;
    case TINYUSB_MSC_EVENT_FORMAT_FAILED:
        ESP_LOGE(kTag, "SD card format failed");
        break;
    default:
        break;
    }
}

void onUsbEvent(tinyusb_event_t *event, void *)
{
    if (event == nullptr)
        return;
    ESP_LOGI(
        kTag,
        "USB host %s",
        event->id == TINYUSB_EVENT_ATTACHED ? "attached" : "detached");
}
#endif

void preserveFirstError(esp_err_t candidate, esp_err_t &first_error)
{
    if (first_error == ESP_OK && candidate != ESP_OK)
        first_error = candidate;
}

void delayMilliseconds(std::uint32_t milliseconds)
{
    if (milliseconds == 0U)
        return;
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    if (ticks == 0)
        ticks = 1;
    vTaskDelay(ticks);
}
}

namespace bsp::t_panel_p4_rect
{
esp_err_t Board::initializeSdCard()
{
    return initializeSdCard(SdCardConfig{});
}

esp_err_t Board::initializeSdCard(const SdCardConfig &config)
{
    if (!initialized_)
        return ESP_ERR_INVALID_STATE;
    if (isSdCardInitialized())
        return ESP_OK;
    if ((config.mount_filesystem &&
         (config.mount_point == nullptr || config.mount_point[0] != '/')) ||
        config.maximum_open_files == 0U ||
        config.maximum_open_files > static_cast<std::size_t>(INT_MAX) ||
        config.allocation_unit_size == 0U ||
        config.maximum_frequency_khz < kMinimumFrequencyKhz ||
        config.maximum_frequency_khz > kMaximumFrequencyKhz)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (config.mount_filesystem)
    {
        std::size_t mount_length = std::strlen(config.mount_point);
        while (mount_length > 1U && config.mount_point[mount_length - 1U] == '/')
            --mount_length;
        if (mount_length <= 1U || mount_length >= sd_mount_point_.size())
            return ESP_ERR_INVALID_SIZE;

        std::memcpy(sd_mount_point_.data(), config.mount_point, mount_length);
        sd_mount_point_[mount_length] = '\0';
    }

    sd_pwr_ctrl_ldo_config_t ldo_config{};
    ldo_config.ldo_chan_id = kLdoChannel;
    esp_err_t error = sd_pwr_ctrl_new_on_chip_ldo(
        &ldo_config,
        &sd_power_control_);
    if (error != ESP_OK)
    {
        sd_mount_point_.fill('\0');
        ESP_LOGE(kTag, "create SD LDO control failed: %s", esp_err_to_name(error));
        return error;
    }

    error = setSdPowerEnabled(true);
    if (error != ESP_OK)
    {
        (void)sd_pwr_ctrl_del_on_chip_ldo(sd_power_control_);
        sd_power_control_ = nullptr;
        sd_mount_point_.fill('\0');
        return error;
    }
    delayMilliseconds(config.power_up_delay_ms);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = config.maximum_frequency_khz;
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.pwr_ctrl_handle = sd_power_control_;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = pins::sd1Clock;
    slot_config.cmd = pins::sd1Command;
    slot_config.d0 = pins::sd1Data0;
    slot_config.d1 = pins::sd1Data1;
    slot_config.d2 = pins::sd1Data2;
    slot_config.d3 = pins::sd1Data3;
    if (config.enable_internal_pullups)
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (config.mount_filesystem)
    {
        esp_vfs_fat_sdmmc_mount_config_t mount_config{};
        mount_config.format_if_mount_failed = config.format_if_mount_failed;
        mount_config.max_files = static_cast<int>(config.maximum_open_files);
        mount_config.allocation_unit_size = config.allocation_unit_size;

        ESP_LOGI(
            kTag,
            "mounting SD card at %s, slot=%d, width=4, max=%u kHz",
            sd_mount_point_.data(),
            host.slot,
            static_cast<unsigned int>(config.maximum_frequency_khz));
        error = esp_vfs_fat_sdmmc_mount(
            sd_mount_point_.data(),
            &host,
            &slot_config,
            &mount_config,
            &sd_card_);
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "mount SD card failed: %s", esp_err_to_name(error));
            (void)deinitializeSdCard();
            return error;
        }
        sd_card_mounted_ = true;
        ESP_LOGI(kTag, "SD card mounted at %s", sd_mount_point_.data());
    }
    else
    {
        raw_sd_card_.reset(new (std::nothrow) sdmmc_card_t{});
        if (!raw_sd_card_)
        {
            (void)deinitializeSdCard();
            return ESP_ERR_NO_MEM;
        }

        error = host.init();
        if (error == ESP_OK)
        {
            sd_host_ = host;
            sd_host_initialized_ = true;
            error = sdmmc_host_init_slot(host.slot, &slot_config);
        }
        if (error == ESP_OK)
            error = sdmmc_card_init(&host, raw_sd_card_.get());
        if (error != ESP_OK)
        {
            ESP_LOGE(
                kTag,
                "initialize raw SD card failed: %s",
                esp_err_to_name(error));
            (void)deinitializeSdCard();
            return error;
        }
        sd_card_ = raw_sd_card_.get();
        ESP_LOGI(
            kTag,
            "raw SD card initialized for exclusive block-device access");
    }

    sdmmc_card_print_info(stdout, sd_card_);
    return ESP_OK;
}

esp_err_t Board::deinitializeSdCard()
{
    if (isUsbMscInitialized())
        return ESP_ERR_INVALID_STATE;

    esp_err_t first_error = ESP_OK;
    if (sd_card_ != nullptr && sd_card_mounted_)
    {
        const esp_err_t unmount_error = esp_vfs_fat_sdcard_unmount(
            sd_mount_point_.data(),
            sd_card_);
        if (unmount_error != ESP_OK)
        {
            ESP_LOGE(
                kTag,
                "unmount SD card failed: %s",
                esp_err_to_name(unmount_error));
            return unmount_error;
        }
        sd_card_ = nullptr;
        sd_card_mounted_ = false;
    }
    else
    {
        sd_card_ = nullptr;
    }

    if (sd_host_initialized_)
    {
        const esp_err_t deinit_error =
            (sd_host_.flags & SDMMC_HOST_FLAG_DEINIT_ARG) != 0
                ? sd_host_.deinit_p(sd_host_.slot)
                : sd_host_.deinit();
        preserveFirstError(deinit_error, first_error);
        sd_host_ = {};
        sd_host_initialized_ = false;
    }
    raw_sd_card_.reset();

    if (sd_power_control_ != nullptr)
    {
        preserveFirstError(
            sd_pwr_ctrl_del_on_chip_ldo(sd_power_control_),
            first_error);
        sd_power_control_ = nullptr;
    }
    if (initialized_)
        preserveFirstError(setSdPowerEnabled(false), first_error);

    preserveFirstError(gpio_reset_pin(pins::sd1Clock), first_error);
    preserveFirstError(gpio_reset_pin(pins::sd1Command), first_error);
    preserveFirstError(gpio_reset_pin(pins::sd1Data0), first_error);
    preserveFirstError(gpio_reset_pin(pins::sd1Data1), first_error);
    preserveFirstError(gpio_reset_pin(pins::sd1Data2), first_error);
    preserveFirstError(gpio_reset_pin(pins::sd1Data3), first_error);
    sd_mount_point_.fill('\0');
    return first_error;
}

bool Board::isSdCardInitialized() const noexcept
{
    return sd_card_ != nullptr;
}

bool Board::isSdCardMounted() const noexcept
{
    return isSdCardInitialized() && sd_card_mounted_;
}

sdmmc_card_t *Board::sdCard() noexcept
{
    return isSdCardInitialized() ? sd_card_ : nullptr;
}

const char *Board::sdCardMountPoint() const noexcept
{
    return isSdCardMounted() ? sd_mount_point_.data() : nullptr;
}

esp_err_t Board::initializeUsbMsc()
{
#if !CONFIG_TINYUSB_MSC_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!initialized_)
        return ESP_ERR_INVALID_STATE;
    if (isUsbMscInitialized())
        return ESP_OK;
    if (isSdCardMounted())
        return ESP_ERR_INVALID_STATE;

    if (!isSdCardInitialized())
    {
        SdCardConfig sd_config{};
        sd_config.mount_filesystem = false;
        esp_err_t error = initializeSdCard(sd_config);
        if (error != ESP_OK)
            return error;
        usb_msc_owns_sd_card_ = true;
    }

    tinyusb_msc_driver_config_t driver_config{};
    driver_config.callback = onStorageEvent;
    esp_err_t error = tinyusb_msc_install_driver(&driver_config);
    if (error != ESP_OK)
    {
        (void)deinitializeUsbMsc();
        return error;
    }
    usb_msc_driver_initialized_ = true;

    tinyusb_msc_storage_config_t storage_config{};
    storage_config.medium.card = sd_card_;
    storage_config.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    storage_config.fat_fs.base_path = nullptr;
    storage_config.fat_fs.config.max_files = 5;
    storage_config.fat_fs.config.format_if_mount_failed = false;
    storage_config.fat_fs.do_not_format = true;
    storage_config.fat_fs.format_flags = 0;
    tinyusb_msc_storage_handle_t storage{};
    error = tinyusb_msc_new_storage_sdmmc(&storage_config, &storage);
    if (error != ESP_OK)
    {
        (void)deinitializeUsbMsc();
        return error;
    }
    usb_msc_storage_ = storage;

    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG(onUsbEvent);
    usb_config.descriptor.device = &kDeviceDescriptor;
    usb_config.descriptor.full_speed_config =
        kFullSpeedConfigurationDescriptor;
    usb_config.descriptor.string = kStringDescriptors;
    usb_config.descriptor.string_count =
        static_cast<int>(std::size(kStringDescriptors));
#if TUD_OPT_HIGH_SPEED
    usb_config.descriptor.high_speed_config =
        kHighSpeedConfigurationDescriptor;
    usb_config.descriptor.qualifier = &kDeviceQualifier;
#endif
    error = tinyusb_driver_install(&usb_config);
    if (error != ESP_OK)
    {
        (void)deinitializeUsbMsc();
        return error;
    }
    usb_driver_initialized_ = true;
    ESP_LOGI(kTag, "USB MSC initialized with exclusive raw SD-card access");
    return ESP_OK;
#endif
}

esp_err_t Board::deinitializeUsbMsc()
{
#if !CONFIG_TINYUSB_MSC_ENABLED
    return isUsbMscInitialized() ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
#else
    esp_err_t first_error = ESP_OK;
    if (usb_driver_initialized_)
    {
        const esp_err_t error = tinyusb_driver_uninstall();
        preserveFirstError(error, first_error);
        if (error == ESP_OK)
            usb_driver_initialized_ = false;
    }
    if (usb_msc_storage_ != nullptr && !usb_driver_initialized_)
    {
        const esp_err_t error = tinyusb_msc_delete_storage(
            static_cast<tinyusb_msc_storage_handle_t>(usb_msc_storage_));
        preserveFirstError(error, first_error);
        if (error == ESP_OK)
            usb_msc_storage_ = nullptr;
    }
    if (usb_msc_driver_initialized_ && usb_msc_storage_ == nullptr &&
        !usb_driver_initialized_)
    {
        const esp_err_t error = tinyusb_msc_uninstall_driver();
        preserveFirstError(error, first_error);
        if (error == ESP_OK)
            usb_msc_driver_initialized_ = false;
    }

    if (!isUsbMscInitialized() && usb_msc_owns_sd_card_)
    {
        usb_msc_owns_sd_card_ = false;
        preserveFirstError(deinitializeSdCard(), first_error);
    }
    return first_error;
#endif
}

bool Board::isUsbMscInitialized() const noexcept
{
    return usb_driver_initialized_ || usb_msc_driver_initialized_ ||
           usb_msc_storage_ != nullptr;
}
}
