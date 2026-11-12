// USB on core 1: a text console and a binary link, fed from core 0.
#pragma once
#include <stdbool.h>
#include <stddef.h>

void usb_start(void);                         // launch core 1 and TinyUSB
void usb_console_write(const char *s, size_t n); // never blocks; drops if full
void usb_link_write(const void *p, size_t n);    // blocks until queued
bool usb_link_connected(void);                // host has the link port open
int usb_link_getc(void);                      // next byte from the host, or -1
void usb_flush(unsigned timeout_ms);          // wait for both queues to drain
