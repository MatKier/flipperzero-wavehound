#include "wh_uart.h"

#include <furi.h>
#include <furi_hal.h>
#include <string.h>

#define WH_UART_BAUD 115200
#define WH_RX_BUF_SIZE 1024
/* Must hold the board's longest line: WPS,OK,<ssid_hex>,<psk_hex> is up to
 * 7 + 64 + 1 + 128 = 200 chars (32-byte SSID, 64-byte PSK, hex-encoded). Match
 * the board's own LINE_MAX (256) so a long credential line is never truncated
 * -- a truncated psk_hex decodes to a silently wrong saved password. */
#define WH_LINE_MAX 256
#define WH_PING_INTERVAL_MS 1500
#define WH_ALIVE_TIMEOUT_MS 4000

typedef enum {
    WorkerEvtStop = (1 << 0),
    WorkerEvtRx = (1 << 1),
} WorkerEvt;

#define WORKER_EVENTS_ALL (WorkerEvtStop | WorkerEvtRx)

struct WhUart {
    FuriHalSerialHandle* serial;
    FuriThread* thread;
    FuriStreamBuffer* rx_stream;
    FuriMutex* tx_mutex;
    WhUartLineCallback callback;
    void* context;
    volatile uint32_t last_rx_tick;
};

static void wh_uart_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    WhUart* uart = context;
    if(event == FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(uart->rx_stream, &data, 1, 0);
        furi_thread_flags_set(furi_thread_get_id(uart->thread), WorkerEvtRx);
    }
}

static int32_t wh_uart_worker(void* context) {
    WhUart* uart = context;
    char line[WH_LINE_MAX];
    size_t len = 0;
    uint32_t last_ping = 0;

    for(;;) {
        uint32_t events =
            furi_thread_flags_wait(WORKER_EVENTS_ALL, FuriFlagWaitAny, furi_ms_to_ticks(100));
        if(!(events & FuriFlagError) && (events & WorkerEvtStop)) break;

        uint8_t buf[64];
        size_t n;
        while((n = furi_stream_buffer_receive(uart->rx_stream, buf, sizeof(buf), 0)) > 0) {
            for(size_t i = 0; i < n; i++) {
                char c = (char)buf[i];
                if(c == '\r') continue;
                if(c == '\n') {
                    if(len > 0) {
                        line[len] = '\0';
                        uart->last_rx_tick = furi_get_tick();
                        uart->callback(line, uart->context);
                    }
                    len = 0;
                } else if(len < sizeof(line) - 1) {
                    line[len++] = c;
                }
            }
        }

        /* Keep-alive: only ping when the board has been quiet (it streams in hunt/spectrum). */
        uint32_t now = furi_get_tick();
        if(now - uart->last_rx_tick > furi_ms_to_ticks(WH_PING_INTERVAL_MS) &&
           now - last_ping > furi_ms_to_ticks(WH_PING_INTERVAL_MS)) {
            last_ping = now;
            wh_uart_send(uart, "PING");
        }
    }
    return 0;
}

WhUart* wh_uart_alloc(WhUartLineCallback callback, void* context) {
    FuriHalSerialHandle* serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!serial) return NULL;

    WhUart* uart = malloc(sizeof(WhUart));
    uart->serial = serial;
    uart->callback = callback;
    uart->context = context;
    uart->last_rx_tick = 0;
    uart->rx_stream = furi_stream_buffer_alloc(WH_RX_BUF_SIZE, 1);
    uart->tx_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    /* Order matters: the worker may transmit as soon as it runs, and the RX
     * callback needs a started thread to signal. */
    furi_hal_serial_init(uart->serial, WH_UART_BAUD);
    uart->thread = furi_thread_alloc_ex("WhUartWorker", 2048, wh_uart_worker, uart);
    furi_thread_start(uart->thread);
    furi_hal_serial_async_rx_start(uart->serial, wh_uart_rx_cb, uart, false);
    return uart;
}

void wh_uart_free(WhUart* uart) {
    furi_hal_serial_async_rx_stop(uart->serial);

    furi_thread_flags_set(furi_thread_get_id(uart->thread), WorkerEvtStop);
    furi_thread_join(uart->thread);
    furi_thread_free(uart->thread);

    furi_hal_serial_deinit(uart->serial);
    furi_hal_serial_control_release(uart->serial);

    furi_stream_buffer_free(uart->rx_stream);
    furi_mutex_free(uart->tx_mutex);
    free(uart);
}

void wh_uart_send(WhUart* uart, const char* line) {
    furi_mutex_acquire(uart->tx_mutex, FuriWaitForever);
    furi_hal_serial_tx(uart->serial, (const uint8_t*)line, strlen(line));
    furi_hal_serial_tx(uart->serial, (const uint8_t*)"\n", 1);
    furi_hal_serial_tx_wait_complete(uart->serial);
    furi_mutex_release(uart->tx_mutex);
}

bool wh_uart_board_alive(WhUart* uart) {
    uint32_t last = uart->last_rx_tick;
    return last != 0 && furi_get_tick() - last < furi_ms_to_ticks(WH_ALIVE_TIMEOUT_MS);
}
