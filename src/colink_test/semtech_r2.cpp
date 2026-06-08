// semtech_r2.cpp — Path B M0: the Semtech-driver half of the co-link smoke test.
// This TU includes the vendored Semtech driver headers and NOTHING from RadioLib.
// Mirrors src/oem_rx for the init sequence; mirrors the planned SemtechLR1121Hal
// for the shared-mutex + self-contained-transaction discipline.
#include "semtech_r2.h"
#include "wavesahre_lora_1121.h"   // vendored Semtech lr11xx_driver (lr1121_t, lr11xx_*, lora_*, RADIO_*)
#include <SPI.h>

static lr1121_t          s_r2;            // pins filled by lora_init_io_context()
static SemaphoreHandle_t s_mutex = nullptr;
static uint16_t          s_fw    = 0;
static int               s_bootBusy   = -1;   // BUSY after reset
static int               s_bootStatus = -1;   // get_version status at boot
static uint8_t           s_bootHw     = 0;
static uint8_t           s_bootType   = 0;

// The vendored HAL does raw SPI.transfer() and relies on an active beginTransaction
// for the bus settings (it never brackets individual commands). So we bracket each
// driver CALL with its own mutex + beginTransaction/endTransaction at 1 MHz — the
// speed the HAL was tuned for over this board's hand-soldered jumpers.
static bool getVersion(lr11xx_system_version_t* v)
{
    bool ok = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        SPI.beginTransaction(SPISettings(RADIO_SPI_SPEED, MSBFIRST, SPI_MODE0));
        ok = (lr11xx_system_get_version(&s_r2, v) == LR11XX_STATUS_OK);
        SPI.endTransaction();
        xSemaphoreGive(s_mutex);
    }
    return ok;
}

bool r2_begin(SemaphoreHandle_t spiMutex)
{
    s_mutex = spiMutex;
    lora_init_io_context(&s_r2);   // fill s_r2 pins from the RADIO_* defines
    lora_init_io(&s_r2);            // pinMode + NRESET/LED high (no SPI yet)
    Serial.printf("[colink] R2 pins: cs=%u reset=%u busy=%u irq=%u (sck/mosi/miso=%u/%u/%u)\n",
                  s_r2.cs, s_r2.reset, s_r2.busy, s_r2.irq, s_r2.clk, s_r2.mosi, s_r2.miso);

    lr11xx_system_version_t v = {};
    lr11xx_status_t st = LR11XX_STATUS_ERROR;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        // Use the EXACT proven OEM SPI setup (SPI.begin(7,8,9,ss=5) + an open
        // beginTransaction), not a hand-rolled one — then end the transaction so
        // R1 can still share the bus. (Diagnostic: isolates SPI-init as the cause
        // of the all-zeros reads.)
        lora_spi_init(&s_r2);

        lr11xx_system_reset(&s_r2);                 // GPIO reset + ~142 ms BUSY wait
        s_bootBusy = digitalRead(s_r2.busy);

        lr11xx_hal_wakeup(&s_r2);
        // Mirror the OEM's proven early init (src/oem_rx/lr1121_config.cpp:33-39).
        lr11xx_system_enable_spi_crc(&s_r2, false);
        lr11xx_system_set_standby(&s_r2, LR11XX_SYSTEM_STANDBY_CFG_XOSC);

        st = lr11xx_system_get_version(&s_r2, &v);

        SPI.endTransaction();   // release the lock lora_spi_init opened (so R1 can share)
        xSemaphoreGive(s_mutex);
    }
    s_fw = v.fw;
    s_bootStatus = (int)st;
    s_bootHw     = v.hw;
    s_bootType   = v.type;
    const bool ok = (st == LR11XX_STATUS_OK) && (v.fw != 0x0000);
    Serial.printf("[colink] R2 BUSY after reset=%d  get_version: st=%d hw=0x%02X type=0x%02X fw=0x%04X -> %s\n",
                  s_bootBusy, (int)st, v.hw, v.type, v.fw, ok ? "OK" : "NO ANSWER");
    return ok;
}

uint16_t r2_fw() { return s_fw; }
int      r2_boot_busy()   { return s_bootBusy; }
int      r2_boot_status() { return s_bootStatus; }
uint8_t  r2_boot_hw()     { return s_bootHw; }
uint8_t  r2_boot_type()   { return s_bootType; }

bool r2_ping(uint16_t expectFw)
{
    lr11xx_system_version_t v = {};
    return getVersion(&v) && v.fw == expectFw;
}
