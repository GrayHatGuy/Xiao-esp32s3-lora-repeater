// main.cpp — Core1121 OEM control test (Semtech lr11xx_driver, NOT RadioLib).
//
// Drives the SAME physically-wired Core1121 (XIAO R2 slot) with Semtech's
// official lr11xx_driver to receive the bench MeshCore traffic (910.525 / BW62.5
// / SF7 / sync 0x12). RESULT so far: this driver reaches sync/header-VALID where
// our RadioLib build only ever reached preamble — but it doesn't complete to
// RX_DONE. This build adds per-event correlation (raw IRQ bits, instantaneous
// RSSI, held-duration, decoded length) to see WHY a header decodes but the
// payload never finishes.
//
// Build/flash:  pio run -e core1121_oem_rx -t upload -t monitor

#include <Arduino.h>
#include "lr1121_config.h"

#define I_PRE  LR11XX_SYSTEM_IRQ_PREAMBLE_DETECTED
#define I_SYNC LR11XX_SYSTEM_IRQ_SYNC_WORD_HEADER_VALID
#define I_HERR LR11XX_SYSTEM_IRQ_HEADER_ERROR
#define I_CERR LR11XX_SYSTEM_IRQ_CRC_ERROR
#define I_DONE LR11XX_SYSTEM_IRQ_RX_DONE
#define I_TMO  LR11XX_SYSTEM_IRQ_TIMEOUT

#define OEM_RX_IRQ (I_DONE | I_TMO | I_PRE | I_SYNC | I_HERR | I_CERR)

static uint8_t                   rxbuf[RX_BUFFER_LENGTH];
static uint32_t                  s_pkts     = 0;
static uint32_t                  s_lastBeat = 0;
static lr11xx_system_irq_mask_t  s_lastIrq  = 0;   // for edge detection
static uint32_t                  s_irqStart = 0;

// --- Auto frequency sweep -------------------------------------------------
// The RX center is bench-confirmed to be off (and boot-variable). Walk a band
// around nominal until something COMPLETES (RX_DONE), then lock onto it. The
// current swept frequency is shown in every event row + heartbeat so a decode
// is unambiguously tagged to the frequency that produced it.
static const uint32_t SWEEP_HZ[] = {
    910490000UL, 910500000UL, 910510000UL, 910520000UL, 910525000UL,
    910535000UL, 910545000UL, 910555000UL, 910565000UL, 910575000UL, 910585000UL,
};
static const int SWEEP_N    = (int)(sizeof(SWEEP_HZ) / sizeof(SWEEP_HZ[0]));
static int       s_freqIdx  = 4;       // start at 910.525 (nominal)
static uint32_t  s_lastStep = 0;
static bool      s_locked   = false;   // stop sweeping once a packet completes
static const uint32_t SWEEP_DWELL_MS = 7000;

static void set_freq_and_rx(uint32_t hz)
{
    lr11xx_system_set_standby(&lr1121, LR11XX_SYSTEM_STANDBY_CFG_XOSC);
    lr11xx_radio_set_rf_freq(&lr1121, hz);
    lr11xx_system_clear_irq_status(&lr1121, LR11XX_SYSTEM_IRQ_ALL_MASK);
    lr11xx_radio_set_rx(&lr1121, 0x00FFFFFF);
}

static void irq_names(lr11xx_system_irq_mask_t irq, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s%s%s%s%s",
             (irq & I_PRE)  ? "PRE "      : "",
             (irq & I_SYNC) ? "SYNC/HDR " : "",
             (irq & I_HERR) ? "HDR_ERR "  : "",
             (irq & I_CERR) ? "CRC_ERR "  : "",
             (irq & I_DONE) ? "RX_DONE "  : "",
             (irq & I_TMO)  ? "TMO "      : "");
}

static void arm_rx(void) { lr11xx_radio_set_rx(&lr1121, 0x00FFFFFF); }  // continuous

void setup(void)
{
    Serial.begin(115200);
    delay(600);
    Serial.println("\n=== Core1121 OEM control: Semtech lr11xx_driver RX (correlated) ===");
    Serial.println("(NOT RadioLib - same wiring; MeshCore 910.525/BW62.5/SF7/0x12)");

    lora_init_io_context(&lr1121);
    lora_init_io(&lr1121);
    lora_spi_init(&lr1121);

    lora_system_init(&lr1121);
    lora_radio_init(&lr1121);

    lr11xx_system_set_dio_irq_params(&lr1121, OEM_RX_IRQ, 0);
    lr11xx_system_clear_irq_status(&lr1121, LR11XX_SYSTEM_IRQ_ALL_MASK);

    set_freq_and_rx(SWEEP_HZ[s_freqIdx]);
    Serial.printf("listening @ %lu Hz; auto-sweeping every %lu s until RX_DONE\n\n",
                  (unsigned long)SWEEP_HZ[s_freqIdx], (unsigned long)(SWEEP_DWELL_MS / 1000));
}

void loop(void)
{
    lr11xx_system_irq_mask_t irq = 0;
    lr11xx_system_get_and_clear_irq_status(&lr1121, &irq);

    // Edge-triggered: print only when the latched IRQ state CHANGES, so a flag
    // the chip holds for the whole payload window is ONE row, not hundreds.
    if (irq != s_lastIrq) {
        uint32_t now = millis();
        if (irq != 0) {
            int8_t rssi = 0;
            lr11xx_radio_get_rssi_inst(&lr1121, &rssi);
            lr11xx_radio_rx_buffer_status_t bs = { 0 };
            lr11xx_radio_get_rx_buffer_status(&lr1121, &bs);
            char names[64];
            irq_names(irq, names, sizeof(names));
            Serial.printf("[%8lu] @%lu irq=0x%02lX [%s] rssi_inst=%d dBm  buf_len=%u start=%u\n",
                          now, (unsigned long)SWEEP_HZ[s_freqIdx], (unsigned long)irq, names, (int)rssi,
                          (unsigned)bs.pld_len_in_bytes,
                          (unsigned)bs.buffer_start_pointer);
            s_irqStart = now;
        } else {
            char names[64];
            irq_names(s_lastIrq, names, sizeof(names));
            Serial.printf("[%8lu]    ^ [%s] held %lu ms then idle (no RX_DONE)\n",
                          now, names, (unsigned long)(now - s_irqStart));
        }
        s_lastIrq = irq;
    }

    if (irq & I_DONE) {
        lr11xx_radio_rx_buffer_status_t bs = { 0 };
        lr11xx_radio_pkt_status_lora_t  ps = { 0 };
        lr11xx_radio_get_rx_buffer_status(&lr1121, &bs);
        lr11xx_radio_get_lora_pkt_status(&lr1121, &ps);
        uint8_t n = bs.pld_len_in_bytes;
        if (n > sizeof(rxbuf)) n = sizeof(rxbuf);
        lr11xx_regmem_read_buffer8(&lr1121, rxbuf, bs.buffer_start_pointer, n);
        bool crc = (irq & I_CERR);
        Serial.printf("[%8lu] *** RX_DONE #%lu @ %lu Hz *** %u B  RSSI %d dBm  SNR %d dB  %s\n",
                      (unsigned long)millis(), (unsigned long)++s_pkts,
                      (unsigned long)SWEEP_HZ[s_freqIdx], (unsigned)n,
                      (int)ps.rssi_pkt_in_dbm, (int)ps.snr_pkt_in_db,
                      crc ? "CRC-FAIL" : "CRC-OK");
        Serial.print("            payload:");
        for (uint8_t i = 0; i < n; i++) Serial.printf(" %02X", rxbuf[i]);
        Serial.println();
        s_locked = true;   // a packet completed here — stop sweeping, hold this freq
        arm_rx();
    } else if (irq & I_TMO) {
        arm_rx();
    }

    // Auto-sweep the RX frequency until something completes, then lock.
    if (!s_locked && (millis() - s_lastStep >= SWEEP_DWELL_MS)) {
        s_lastStep = millis();
        s_freqIdx  = (s_freqIdx + 1) % SWEEP_N;
        set_freq_and_rx(SWEEP_HZ[s_freqIdx]);
        s_lastIrq  = 0;
        Serial.printf("[%8lu] [sweep] -> %lu Hz\n",
                      (unsigned long)millis(), (unsigned long)SWEEP_HZ[s_freqIdx]);
    }

    if (millis() - s_lastBeat >= 5000) {
        s_lastBeat = millis();
        int8_t rssi = 0;
        lr11xx_radio_get_rssi_inst(&lr1121, &rssi);
        Serial.printf("[%8lu] [hb] @%lu Hz%s  pkts=%lu  rssi_floor=%d dBm\n",
                      (unsigned long)millis(), (unsigned long)SWEEP_HZ[s_freqIdx],
                      s_locked ? " LOCKED" : "", (unsigned long)s_pkts, (int)rssi);
    }
    delay(2);
}
