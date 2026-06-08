#include "wavesahre_lora_1121.h"


void lora_init_io_context(const void *context)
{
    ((lr1121_t *)context)->reset = RADIO_RESET;
    ((lr1121_t *)context)->led = RADIO_LED;
    ((lr1121_t *)context)->cs = RADIO_CS;
    ((lr1121_t *)context)->irq = RADIO_IRQ;
    ((lr1121_t *)context)->busy = RADIO_BUSY;
    ((lr1121_t *)context)->miso = RADIO_MISO;
    ((lr1121_t *)context)->mosi = RADIO_MOSI;
    ((lr1121_t *)context)->clk = RADIO_CLK;
}

void lora_init_io(const void *context)
{
    pinMode(((lr1121_t *)context)->reset, OUTPUT);
    pinMode(((lr1121_t *)context)->cs, OUTPUT);
    pinMode(((lr1121_t *)context)->led, OUTPUT);
    pinMode(((lr1121_t *)context)->busy, INPUT);
    pinMode(((lr1121_t *)context)->irq, INPUT);

    digitalWrite(((lr1121_t *)context)->reset, HIGH );
    digitalWrite(((lr1121_t *)context)->led, HIGH );
}

void lora_init_irq(const void *context, void (*handler)(void))
{
    pinMode(((lr1121_t *)context)->irq, INPUT_PULLDOWN);
    attachInterrupt(((lr1121_t *)context)->irq, handler, RISING);
}

void lora_spi_init(const void* context)
{
    SPI.begin(RADIO_CLK,RADIO_MISO,RADIO_MOSI,RADIO_CS);
    SPI.beginTransaction(SPISettings(RADIO_SPI_SPEED, MSBFIRST, SPI_MODE0));
}

void lora_spi_write_bytes(const void* context,const uint8_t *wirte,const uint16_t wirte_length)
{
    for (size_t i = 0; i < wirte_length; i++) {
      SPI.transfer(wirte[i]);
    }
}

void lora_spi_read_bytes(const void* context, uint8_t *read,const uint16_t read_length)
{
    for (size_t i = 0; i < read_length; i++) {
      read[i] = SPI.transfer(0x00);
    }
}


lr1121_modem_response_code_t lr1121_modem_board_event_flush( const void* context )
{
    lr1121_modem_response_code_t modem_response_code = LR1121_MODEM_RESPONSE_CODE_OK;
    lr1121_modem_event_fields_t  event_fields;

    do
    {
        modem_response_code = lr1121_modem_get_event( context, &event_fields );
    } while( modem_response_code != LR1121_MODEM_RESPONSE_CODE_NO_EVENT );

    return modem_response_code;
}

