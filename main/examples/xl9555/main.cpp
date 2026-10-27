#include <cstdint>
#include <cstdio>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/t_panel_p4_standard.hpp>
#include <pdl/core/status.hpp>
#include <pdl/drivers/expand_io/xl9555.hpp>

namespace
{
    // Set only pins that are safe to drive during the hardware output test.
    // A zero mask keeps the default test non-destructive.
    constexpr std::uint16_t kOutputTestMask = 0x0000;

    class TestResults
    {
    public:
        bool check(const char *name, pdl::Status status)
        {
            if (pdl::isOk(status))
            {
                ++passed_;
                std::printf("[PASS] %s\n", name);
                return true;
            }

            ++failed_;
            std::printf("[FAIL] %s: %s\n", name, pdl::statusName(status));
            return false;
        }

        bool checkValue(const char *name, bool valid)
        {
            if (valid)
            {
                ++passed_;
                std::printf("[PASS] %s\n", name);
                return true;
            }

            ++failed_;
            std::printf("[FAIL] %s\n", name);
            return false;
        }

        bool checkEsp(const char *name, esp_err_t error)
        {
            if (error == ESP_OK)
            {
                ++passed_;
                std::printf("[PASS] %s\n", name);
                return true;
            }

            ++failed_;
            std::printf("[FAIL] %s: %s\n", name, esp_err_to_name(error));
            return false;
        }

        void skip(const char *name, const char *reason)
        {
            ++skipped_;
            std::printf("[SKIP] %s: %s\n", name, reason);
        }

        void printSummary() const
        {
            std::printf(
                "\nXL9555 test result: %s (%u passed, %u failed, %u skipped)\n",
                failed_ == 0U ? "PASS" : "FAIL",
                passed_,
                failed_,
                skipped_);
        }

    private:
        unsigned int passed_{};
        unsigned int failed_{};
        unsigned int skipped_{};
    };

    std::uint8_t firstPin(std::uint16_t mask)
    {
        for (std::uint8_t pin = 0; pin < pdl::expand_io::Xl9555::pinCount; ++pin)
        {
            if ((mask & static_cast<std::uint16_t>(std::uint16_t{1} << pin)) != 0U)
                return pin;
        }
        return pdl::expand_io::Xl9555::pinCount;
    }

    void testInputPinLatches(pdl::expand_io::Xl9555 &xl9555, std::uint16_t original_input_mask, std::uint16_t original_output_latches, TestResults &results)
    {
        if (original_input_mask == 0U)
        {
            results.skip("input-pin output latch tests", "no pins are configured as inputs");
            return;
        }

        const std::uint8_t pin = firstPin(original_input_mask);
        const std::uint16_t pin_mask = static_cast<std::uint16_t>(
            std::uint16_t{1} << pin);
        const bool original_high = (original_output_latches & pin_mask) != 0U;

        if (results.check(
                "write one input pin output latch",
                xl9555.writePin(pin, !original_high)))
        {
            results.checkValue(
                "verify one input pin output latch cache",
                ((xl9555.outputLatches() & pin_mask) != 0U) != original_high);
        }
        results.check(
            "restore one input pin output latch",
            xl9555.writePin(pin, original_high));

        const std::uint16_t toggled_input_latches = static_cast<std::uint16_t>(
            original_output_latches ^ original_input_mask);
        if (results.check(
                "write 16-bit output latches without driving input pins",
                xl9555.writeOutputs(toggled_input_latches)))
        {
            results.checkValue(
                "verify 16-bit output latch cache",
                xl9555.outputLatches() == toggled_input_latches);
        }
        results.check(
            "restore 16-bit output latches",
            xl9555.writeOutputs(original_output_latches));
    }

    void testPolarityAndInterrupts(pdl::expand_io::Xl9555 &xl9555, std::uint16_t original_input_mask, std::uint16_t original_polarity_mask, TestResults &results)
    {
        if (original_input_mask == 0U)
        {
            results.skip("input polarity test", "no pins are configured as inputs");
            results.skip("interrupt status test", "no pins are configured as inputs");
            return;
        }

        const std::uint8_t pin = firstPin(original_input_mask);
        const std::uint16_t pin_mask = static_cast<std::uint16_t>(
            std::uint16_t{1} << pin);
        const bool originally_inverted = (original_polarity_mask & pin_mask) != 0U;

        if (results.check(
                "invert one input polarity",
                xl9555.setPinPolarityInverted(pin, !originally_inverted)))
        {
            results.checkValue(
                "verify input polarity cache",
                ((xl9555.polarityInversionMask() & pin_mask) != 0U) !=
                    originally_inverted);
        }
        results.check(
            "restore input polarity",
            xl9555.setPolarityInversion(original_polarity_mask));

        if (!results.check(
                "configure input interrupt mask",
                xl9555.configureInterrupts(original_input_mask)))
        {
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
        std::uint16_t changed_pins{};
        std::uint16_t levels{};
        if (results.check(
                "read and clear interrupt status",
                xl9555.readInterruptStatus(changed_pins, levels)))
        {
            std::printf(
                "       Interrupt mask=0x%04X, changed=0x%04X, levels=0x%04X\n",
                original_input_mask,
                changed_pins,
                levels);
        }
    }

    void testSelectedOutputs(pdl::expand_io::Xl9555 &xl9555, std::uint16_t original_input_mask, std::uint16_t original_output_latches, TestResults &results)
    {
        if (kOutputTestMask == 0U)
        {
            results.skip(
                "physical output high/low test",
                "kOutputTestMask is zero; select known-safe board pins first");
            return;
        }

        const std::uint16_t low_levels = static_cast<std::uint16_t>(
            original_output_latches & ~kOutputTestMask);
        if (!results.check("preload selected outputs low", xl9555.writeOutputs(low_levels)))
            return;

        const std::uint16_t test_input_mask = static_cast<std::uint16_t>(
            original_input_mask & ~kOutputTestMask);
        if (!results.check(
                "configure selected pins as outputs",
                xl9555.setPinModes(test_input_mask)))
        {
            return;
        }

        const std::uint16_t high_levels = static_cast<std::uint16_t>(
            low_levels | kOutputTestMask);
        results.check("drive selected outputs high", xl9555.writeOutputs(high_levels));
        vTaskDelay(pdMS_TO_TICKS(250));
        results.check("drive selected outputs low", xl9555.writeOutputs(low_levels));
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    void runXl9555Test()
    {
        TestResults results;
        bsp::t_panel_p4_standard::Board board;

        std::printf("\nXL9555 hardware test\n");
        std::printf(
            "Board BSP: SDA=GPIO%d, SCL=GPIO%d, XL9555 address=0x%02X\n",
            static_cast<int>(bsp::t_panel_p4_standard::pins::i2cSda),
            static_cast<int>(bsp::t_panel_p4_standard::pins::i2cScl),
            bsp::t_panel_p4_standard::pins::xl9555Address);
        std::printf("Physical output test mask: 0x%04X\n\n", kOutputTestMask);

        if (!results.checkEsp("initialize board BSP", board.initialize()))
        {
            results.printSummary();
            return;
        }

        auto *xl9555 = board.ioExpander();
        if (!results.checkValue("get BSP XL9555 driver", xl9555 != nullptr))
        {
            results.checkEsp("deinitialize board BSP", board.deinitialize());
            results.printSummary();
            return;
        }

        const std::uint16_t original_input_mask = xl9555->inputMask();
        const std::uint16_t original_output_latches = xl9555->outputLatches();
        const std::uint16_t original_polarity_mask =
            xl9555->polarityInversionMask();
        std::printf(
            "Initial state: inputs=0x%04X, outputs=0x%04X, polarity=0x%04X\n",
            original_input_mask,
            original_output_latches,
            original_polarity_mask);

        std::uint16_t input_levels{};
        if (results.check("read all input levels", xl9555->readInputs(input_levels)))
            std::printf("       Input levels: 0x%04X\n", input_levels);

        testInputPinLatches(
            *xl9555,
            original_input_mask,
            original_output_latches,
            results);
        testPolarityAndInterrupts(
            *xl9555,
            original_input_mask,
            original_polarity_mask,
            results);
        testSelectedOutputs(
            *xl9555,
            original_input_mask,
            original_output_latches,
            results);

        // Restore direction before latches so original input pins stop driving first.
        results.check("restore pin modes", xl9555->setPinModes(original_input_mask));
        results.check(
            "restore output latches",
            xl9555->writeOutputs(original_output_latches));
        results.check(
            "restore polarity inversion",
            xl9555->setPolarityInversion(original_polarity_mask));
        results.checkEsp("deinitialize board BSP", board.deinitialize());
        results.printSummary();
    }
}

extern "C" void app_main(void)
{
    runXl9555Test();
}
