#include "proto.h"

#include <stdarg.h>
#include <stdio.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#define PROTO_UART UART_NUM_0
#define PROTO_RX_BUF 1024
#define PROTO_TX_BUF 2048
/* Long enough for the longest line, the CONNECT command, whose hex-encoded SSID
 * and PSK fields can reach ~220 chars (see docs/PROTOCOL.md). */
#define PROTO_LINE_MAX 256

static SemaphoreHandle_t s_tx_lock;

void proto_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = CONFIG_WH_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(PROTO_UART, PROTO_RX_BUF, PROTO_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PROTO_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(PROTO_UART, CONFIG_WH_UART_TX_GPIO, CONFIG_WH_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    s_tx_lock = xSemaphoreCreateMutex();
}

void proto_sendf(const char *fmt, ...)
{
    char line[PROTO_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    if (len < 0) {
        return;
    }
    if (len > (int)sizeof(line) - 2) {
        len = sizeof(line) - 2;
    }
    line[len++] = '\n';

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uart_write_bytes(PROTO_UART, line, len);
    xSemaphoreGive(s_tx_lock);
}

size_t proto_read_line(char *buf, size_t size)
{
    size_t len = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(PROTO_UART, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (len == 0) {
                continue;
            }
            buf[len] = '\0';
            return len;
        }
        if (len < size - 1) {
            buf[len++] = (char)c;
        }
        /* Overlong lines are truncated; the remainder is dropped until '\n'. */
    }
}
