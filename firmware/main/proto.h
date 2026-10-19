#pragma once

#include <stddef.h>

#define WH_FW_VERSION "4"

/** Configure the UART link to the Flipper. */
void proto_init(void);

/** Send one formatted line to the Flipper (newline appended). Thread-safe. */
void proto_sendf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * Block until a full line arrives from the Flipper.
 * The line is NUL-terminated with the trailing CR/LF stripped.
 * Returns the line length.
 */
size_t proto_read_line(char *buf, size_t size);
