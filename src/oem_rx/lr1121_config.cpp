// lr1121_config.cpp — OEM Semtech-driver init for the Core1121 RX control test.
// Mirrors the WaveShare/Semtech demo's lora_system_init()/lora_radio_init()
// (lr1121_config.cpp) verbatim in ordering and calls, changed ONLY for: the
// 910 MHz image-calibration band, the MeshCore LoRa params (BW62.5/SF7/CR45,
// sync 0x12, CRC on), and dropping the TX/PA setup (this is an RX-only test).
#include "lr1121_config.h"

lr1121_t lr1121;

// LoRa modulation parameters (ldro filled at init, like the OEM)
static lr11xx_radio_mod_params_lora_t lora_mod_params = {
    .sf   = LORA_SPREADING_FACTOR,
    .bw   = LORA_BANDWIDTH,
    .cr   = LORA_CODING_RATE,
    .ldro = 0,
};

// LoRa packet parameters
static const lr11xx_radio_pkt_params_lora_t lora_pkt_params = {
    .preamble_len_in_symb = LORA_PREAMBLE_LENGTH,
    .header_type          = LORA_PKT_LEN_MODE,
    .pld_len_in_bytes     = RX_BUFFER_LENGTH,   // explicit header overrides on RX
    .crc                  = LORA_CRC,
    .iq                   = LORA_IQ,
};

// --- System init (verbatim OEM order; image cal re-pointed to ~900-932 MHz) ---
void lora_system_init( const void* context )
{
    lr11xx_system_reset( ( void* ) context );
    lr11xx_hal_wakeup( ( void* ) context );

#if defined(USE_LR11XX_CRC_OVER_SPI)
    lr11xx_system_enable_spi_crc( ( void* ) context, true );
#else
    lr11xx_system_enable_spi_crc( ( void* ) context, false );
#endif

    lr11xx_system_set_standby( ( void* ) context, LR11XX_SYSTEM_STANDBY_CFG_XOSC );

    // Image calibration for our band: bytes are freq/4 MHz steps (0xE1=900, 0xE9=932)
    // so this brackets 910.525 MHz. (OEM demo used 0x6B/0x6E for 430-440 MHz.)
    lr11xx_system_calibrate_image( ( void* ) context, 0xE1, 0xE9 );

    const lr11xx_system_reg_mode_t regulator = smtc_shield_lr11xx_common_get_reg_mode();
    lr11xx_system_set_reg_mode( ( void* ) context, regulator );

    const lr11xx_system_rfswitch_cfg_t* rf_switch_setup =
        smtc_shield_lr11xx_common_get_rf_switch_cfg();
    lr11xx_system_set_dio_as_rf_switch( context, rf_switch_setup );

    // Core1121 TCXO = 3.0 V, 300-step (~9.15 ms) startup budget (OEM value)
    lr11xx_system_set_tcxo_mode( context, LR11XX_SYSTEM_TCXO_CTRL_3_0V, 300 );

    lr11xx_system_cfg_lfclk( context, LR11XX_SYSTEM_LFCLK_XTAL, true );

    lr11xx_system_clear_errors( context );
    lr11xx_system_calibrate( context, 0x3F );

    uint16_t errors = 0;
    lr11xx_system_get_errors( context, &errors );
    if( errors & LR11XX_SYSTEM_ERRORS_IMG_CALIB_MASK )
    {
        Serial.println( "[oem] Image calibration error" );
    }
    Serial.printf( "[oem] system_init errors = 0x%04X\n", errors );
    lr11xx_system_clear_errors( context );
    lr11xx_system_clear_irq_status( context, LR11XX_SYSTEM_IRQ_ALL_MASK );
}

// --- Radio init (RX-only: no PA/TX setup) -----------------------------------
void lora_radio_init( const void* context )
{
    lr11xx_radio_set_pkt_type( context, PACKET_TYPE );

    lr11xx_radio_set_rf_freq( context, RF_FREQ_IN_HZ );

    // The RSSI/AGC gain-tune table RadioLib never programs — picked by frequency.
    lr11xx_radio_set_rssi_calibration(
        context, smtc_shield_lr11xx_get_rssi_calibration_table( RF_FREQ_IN_HZ ) );

    lr11xx_radio_set_rx_tx_fallback_mode( context, FALLBACK_MODE );
    lr11xx_radio_cfg_rx_boosted( context, ENABLE_RX_BOOST_MODE );

    lora_mod_params.ldro =
        smtc_shield_lr11xx_common_compute_lora_ldro( LORA_SPREADING_FACTOR, LORA_BANDWIDTH );
    lr11xx_radio_set_lora_mod_params( context, &lora_mod_params );
    lr11xx_radio_set_lora_pkt_params( context, &lora_pkt_params );
    lr11xx_radio_set_lora_sync_word( context, LORA_SYNCWORD );

    Serial.printf( "[oem] radio_init: %lu Hz  BW62.5 SF7 CR45  sync 0x%02X  (ldro=%d)\n",
            (unsigned long) RF_FREQ_IN_HZ, LORA_SYNCWORD, (int) lora_mod_params.ldro );
}
