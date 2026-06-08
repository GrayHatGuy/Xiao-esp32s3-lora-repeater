// lr1121_config.h — OEM Semtech-driver control test, RX config matched to the
// bench MeshCore source (Heltec, decoded fine by R1 = SX1262).
// Derived from the WaveShare Core1121 demo's lr1121_config.h (Semtech driver),
// trimmed to the LoRa RX path and re-pointed to our 910.525/BW62.5/SF7 channel.
//
// Purpose: drive the SAME physically-wired Core1121 with Semtech's lr11xx_driver
// instead of RadioLib, to decide whether the "preamble-but-never-completes" RX
// failure is our RadioLib usage or the chip. See CORE1121.md.
#ifndef LR1121_CONFIG_H
#define LR1121_CONFIG_H

#include "wavesahre_lora_1121.h"

// General
#define PACKET_TYPE         LR11XX_RADIO_PKT_TYPE_LORA
#define RF_FREQ_IN_HZ       910525000UL   // 910.525 MHz — match the Heltec source
#define FALLBACK_MODE       LR11XX_RADIO_FALLBACK_STDBY_RC
#define ENABLE_RX_BOOST_MODE false
#define RX_BUFFER_LENGTH    255

// LoRa modulation — MeshCore: BW 62.5 kHz / SF7 / CR 4/5
#define LORA_SPREADING_FACTOR LR11XX_RADIO_LORA_SF7
#define LORA_BANDWIDTH        LR11XX_RADIO_LORA_BW_62
#define LORA_CODING_RATE      LR11XX_RADIO_LORA_CR_4_5

// LoRa packet — explicit header, standard IQ, CRC on (header carries the flag),
// preamble 8 (matches the bridge LORA_PREAMBLE_LEN and the R1 decode).
#define LORA_PREAMBLE_LENGTH 8
#define LORA_PKT_LEN_MODE    LR11XX_RADIO_LORA_PKT_EXPLICIT
#define LORA_IQ              LR11XX_RADIO_LORA_IQ_STANDARD
#define LORA_CRC             LR11XX_RADIO_LORA_CRC_ON
#define LORA_SYNCWORD        0x12   // private network — same byte the OEM & our code use

extern lr1121_t lr1121;

void lora_system_init( const void* context );
void lora_radio_init( const void* context );

#endif
