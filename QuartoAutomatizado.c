#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/uart.h"
#include <stdio.h>
#include <string.h>

/* -----------------------------
      DEFINIÇÃO DOS PINOS
--------------------------------*/
#define DHT_PIN 2
#define LDR_PIN 26
#define PIR_PIN 27
#define LED_VENT 14
#define LED_LAMP 15

// UART ESP8266
#define UART_ID uart1
#define TX_PIN 4
#define RX_PIN 5
#define BAUD_RATE 9600

// LCD 16x2 (modo 4 bits)
#define LCD_RS 6
#define LCD_E  7
#define LCD_D4 8
#define LCD_D5 9
#define LCD_D6 10
#define LCD_D7 11

// Botões de modo
#define BTN_MODO1 16
#define BTN_MODO2 17
#define BTN_MODO3 18

/* -----------------------------
          VARIÁVEIS GLOBAIS
--------------------------------*/
int modo = 1;
uint32_t luz_timeout = 0;
const uint32_t TEMPO_LUZ_MS = 4000;

char rx_buffer[64];
int rx_idx = 0;

/* -----------------------------
       LCD – Funções auxiliares
--------------------------------*/
void lcd_pulso_enable() {
    gpio_put(LCD_E, 1);
    sleep_us(1);
    gpio_put(LCD_E, 0);
    sleep_us(50);
}

void lcd_envia_nibble(uint8_t nib) {
    gpio_put(LCD_D4, (nib >> 0) & 1);
    gpio_put(LCD_D5, (nib >> 1) & 1);
    gpio_put(LCD_D6, (nib >> 2) & 1);
    gpio_put(LCD_D7, (nib >> 3) & 1);
    lcd_pulso_enable();
}

void lcd_envia_byte(uint8_t val, bool rs) {
    gpio_put(LCD_RS, rs);
    lcd_envia_nibble(val >> 4);
    lcd_envia_nibble(val & 0x0F);
}

void lcd_cmd(uint8_t cmd) {
    lcd_envia_byte(cmd, false);
}

void lcd_char(char c) {
    lcd_envia_byte(c, true);
}

void lcd_print(const char* text) {
    while (*text) lcd_char(*text++);
}

void lcd_init() {
    gpio_put(LCD_RS, 0);
    gpio_put(LCD_E, 0);
    sleep_ms(20);

    lcd_envia_nibble(0x03); sleep_ms(5);
    lcd_envia_nibble(0x03); sleep_ms(1);
    lcd_envia_nibble(0x03);
    lcd_envia_nibble(0x02);  // Modo 4 bits

    lcd_cmd(0x28); // 4 bits, 2 linhas
    lcd_cmd(0x0C); // Display ON
    lcd_cmd(0x06); // Auto incremento
    lcd_cmd(0x01); // Limpar display
    sleep_ms(2);
}

/* -----------------------------
         UART – ESP8266
--------------------------------*/
void setup_uart() {
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(RX_PIN, GPIO_FUNC_UART);
}

void processa_comando(char* cmd) {
    if (strncmp(cmd, "SETMODE:", 8) == 0) {
        modo = cmd[8] - '0';
        printf(">>> Modo alterado via ESP: %d\n", modo);
    }
}

/* -----------------------------
         LEITURA DHT11
--------------------------------*/
int dht11_read(float *temperature, float *humidity) {
    uint8_t data[5] = {0};
    gpio_set_dir(DHT_PIN, GPIO_OUT);
    gpio_put(DHT_PIN, 0);
    sleep_ms(18);
    gpio_put(DHT_PIN, 1);
    sleep_us(40);
    gpio_set_dir(DHT_PIN, GPIO_IN);

    uint32_t timeout = 10000;
    while(gpio_get(DHT_PIN)==1) if(--timeout==0) return -1;
    while(gpio_get(DHT_PIN)==0) if(--timeout==0) return -1;
    while(gpio_get(DHT_PIN)==1) if(--timeout==0) return -1;

    for (int i=0; i<40; i++) {
        while(gpio_get(DHT_PIN)==0);
        absolute_time_t start = get_absolute_time();
        while(gpio_get(DHT_PIN)==1);
        int dur = absolute_time_diff_us(start, get_absolute_time());
        data[i/8] <<= 1;
        if (dur > 40) data[i/8] |= 1;
    }

    if ((uint8_t)(data[0]+data[1]+data[2]+data[3]) != data[4]) return -2;

    *humidity = data[0] + data[1]*0.1f;
    *temperature = data[2] + data[3]*0.1f;
    return 0;
}

/* -----------------------------
             MAIN
--------------------------------*/
int main() {

    stdio_init_all();
    sleep_ms(2000);

    setup_uart();

    /* Sensores */
    adc_init();
    adc_gpio_init(LDR_PIN);
    adc_select_input(0);

    gpio_init(PIR_PIN);
    gpio_set_dir(PIR_PIN, GPIO_IN);
    gpio_pull_down(PIR_PIN);

    gpio_init(DHT_PIN);
    gpio_pull_up(DHT_PIN);

    gpio_init(LED_VENT); gpio_set_dir(LED_VENT, GPIO_OUT);
    gpio_init(LED_LAMP); gpio_set_dir(LED_LAMP, GPIO_OUT);

    /* LCD */
    gpio_init(LCD_RS); gpio_set_dir(LCD_RS, GPIO_OUT);
    gpio_init(LCD_E);  gpio_set_dir(LCD_E, GPIO_OUT);
    gpio_init(LCD_D4); gpio_set_dir(LCD_D4, GPIO_OUT);
    gpio_init(LCD_D5); gpio_set_dir(LCD_D5, GPIO_OUT);
    gpio_init(LCD_D6); gpio_set_dir(LCD_D6, GPIO_OUT);
    gpio_init(LCD_D7); gpio_set_dir(LCD_D7, GPIO_OUT);

    lcd_init();

    /* Botões */
    gpio_init(BTN_MODO1); gpio_set_dir(BTN_MODO1, GPIO_IN); gpio_pull_up(BTN_MODO1);
    gpio_init(BTN_MODO2); gpio_set_dir(BTN_MODO2, GPIO_IN); gpio_pull_up(BTN_MODO2);
    gpio_init(BTN_MODO3); gpio_set_dir(BTN_MODO3, GPIO_IN); gpio_pull_up(BTN_MODO3);

    float temp = 0, hum = 0;

    while (true) {

        /* BOTÕES – modo */
        if (!gpio_get(BTN_MODO1)) modo = 1;
        if (!gpio_get(BTN_MODO2)) modo = 2;
        if (!gpio_get(BTN_MODO3)) modo = 3;

        /* UART – ESP */
        while (uart_is_readable(UART_ID)) {
            char ch = uart_getc(UART_ID);

            if (ch == '\n') {
                rx_buffer[rx_idx] = 0;
                processa_comando(rx_buffer);
                rx_idx = 0;
            } else {
                rx_buffer[rx_idx++] = ch;
                if (rx_idx >= 63) rx_idx = 0;
            }
        }

        /* Sensores */
        uint16_t ldr = adc_read();
        bool pir = gpio_get(PIR_PIN);
        int ok = dht11_read(&temp, &hum);

        gpio_put(LED_VENT, (ok == 0 && temp > 15));

        if (modo == 1) {
            if (pir && ldr > 350)
                luz_timeout = to_ms_since_boot(get_absolute_time()) + TEMPO_LUZ_MS;

            gpio_put(LED_LAMP,
                to_ms_since_boot(get_absolute_time()) < luz_timeout);

        } else if (modo == 2) {
            gpio_put(LED_LAMP, 1);

        } else {
            gpio_put(LED_LAMP, 0);
        }

        /* LCD */
        lcd_cmd(0x01);
        sleep_ms(2);

        char linha1[20];
        char linha2[20];

        sprintf(linha1, "Modo:%d T:%.1f", modo, temp);
        sprintf(linha2, "LDR:%d PIR:%d", ldr, pir);

        lcd_cmd(0x80);  // linha 1
        lcd_print(linha1);

        lcd_cmd(0xC0);  // linha 2
        lcd_print(linha2);

        sleep_ms(300);
    }
}
