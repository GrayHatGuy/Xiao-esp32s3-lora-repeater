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

    lr11xx_system_version_t v = {};
    bool ok = false;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        SPI.beginTransaction(SPISettings(RADIO_SPI_SPEED, MSBFIRST, SPI_MODE0));
        lr11xx_system_reset(&s_r2);   // GPIO reset + ~142 ms BUSY wait (vendored HAL)
        lr11xx_hal_wakeup(&s_r2);
        ok = (lr11xx_system_get_version(&s_r2, &v) == LR11XX_STATUS_OK);
        SPI.endTransaction();
        xSemaphoreGive(s_mutex);
    }
    s_fw = v.fw;
    Serial.printf("[colink] R2 LR1121 get_version: ok=%d  hw=0x%02X  type=0x%02X  fw=0x%04X\n",
                  (int)ok, v.hw, (int)v.type, v.fw);
    return ok;
}

uint16_t r2_fw() { return s_fw; }

bool r2_ping(uint16_t expectFw)
{
    lr11xx_system_version_t v = {};
    return getVersion(&v) && v.fw == expectFw;
}
