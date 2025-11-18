#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <util/delay.h>

#define TX_PIN 2      // P2 -> RX DFPlayer
#define TRACKS 10     // <-- Укажи количество треков

#define BAUD 9600
#define BIT_US 104

uint8_t EEMEM eeTrack = 1;


// ======================================================
//               Бит-бэнг UART для DFPlayer
// ======================================================
void tx_init() {
    DDRB |= (1 << TX_PIN);
    PORTB |= (1 << TX_PIN);
}

void tx_byte(uint8_t b) {
    cli();

    // start bit
    PORTB &= ~(1 << TX_PIN);
    _delay_us(BIT_US);

    // data bits
    for (uint8_t i = 0; i < 8; i++) {
        if (b & 1) PORTB |=  (1 << TX_PIN);
        else       PORTB &= ~(1 << TX_PIN);

        _delay_us(BIT_US);
        b >>= 1;
    }

    // stop bit
    PORTB |= (1 << TX_PIN);
    _delay_us(BIT_US);

    sei();
}


// ======================================================
//            Правильный расчёт контрольной суммы
// ======================================================
uint16_t calcChecksum(const uint8_t *buf) {
    uint16_t sum = 0;

    // складываем байты 1..6 (как требует DFPlayer)
    for (uint8_t i = 1; i <= 6; i++) {
        sum = (uint16_t)(sum + buf[i]);  // 16-битное переполнение допускается
    }

    // 16-битный two's complement:
    // checksum = 0xFFFF - sum + 1  (или просто -sum)
    return (uint16_t)(0xFFFF - sum + 1);
}


// ======================================================
//               Отправка команд DFPlayer
// ======================================================
void sendCmd(uint8_t cmd, uint16_t param = 0) {
    uint8_t buf[10];

    buf[0] = 0x7E;
    buf[1] = 0xFF;
    buf[2] = 0x06;
    buf[3] = cmd;
    buf[4] = 0x00;
    buf[5] = (param >> 8) & 0xFF;
    buf[6] = param & 0xFF;

    uint16_t cs = calcChecksum(buf);

    buf[7] = (cs >> 8) & 0xFF;
    buf[8] = cs & 0xFF;
    buf[9] = 0xEF;

    for (uint8_t i = 0; i < 10; i++) {
        tx_byte(buf[i]);
        _delay_us(20);
    }
}


// ======================================================
//                     Глубокий сон
// ======================================================
void goToSleepForever() {
    wdt_disable();

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();

    cli();
    sleep_bod_disable();     // отключаем brown-out detector
    sei();

    sleep_cpu();
    // ! СЮДА МЫ НИКОГДА НЕ ВЕРНЁМСЯ
}


// ======================================================
//                     Основная логика
// ======================================================
void setup() {

    wdt_disable();
    tx_init();

    // Минимальная задержка, чтобы DFPlayer проинициализировал SD
    _delay_ms(350);

    // Считываем какой трек воспроизводить
    uint8_t track = eeprom_read_byte(&eeTrack);
    if (track < 1 || track > TRACKS) track = 1;

    // Громкость
    sendCmd(0x06, 30);
    _delay_ms(60);

    // Выбор TF-карты
    sendCmd(0x09, 2);
    _delay_ms(60);

    // Воспроизвести трек
    sendCmd(0x03, track);

    // Записать номер следующего трека
    uint8_t next = track + 1;
    if (next > TRACKS) next = 1;
    eeprom_update_byte(&eeTrack, next);

    // Ждать окончания трека — время укажешь сам:
    _delay_ms(5000);  // поставь длительность трека в мс

    // Остановить DFPlayer, уходит в сон.
    sendCmd(0x0A, 0);

    // Заснуть навечно, до отключения питания
    goToSleepForever();
}

void loop() {}
