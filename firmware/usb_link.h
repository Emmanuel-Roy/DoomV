// USB: a text console and a binary link. Serviced by core 1, fed by core 0.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Core 1.
void usb_init(void);
void usb_service(void (*host_byte)(uint8_t c));   // one turn of the USB loop

// Core 0.
void usb_console_write(const char *s, size_t n);  // never blocks; drops if full
void usb_link_write(const void *p, size_t n);     // blocks until queued
bool usb_link_connected(void);                    // host has the link port open
void usb_flush(unsigned timeout_ms);              // wait for both queues to drain
