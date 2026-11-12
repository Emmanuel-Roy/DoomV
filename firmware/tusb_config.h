// TinyUSB: a device with two CDC (serial) interfaces.
//   CDC 0  "console": plain text -- perf stats and messages, any terminal.
//   CDC 1  "link":    binary -- lock-step records and video for picolink.py.
#pragma once

#define CFG_TUSB_MCU            OPT_MCU_RP2040   // same USB controller on RP2350
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             2
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

// Full-speed bulk endpoints are 64 bytes; larger FIFOs let core 1 keep the
// host fed between tud_task() calls.
#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  1024
#define CFG_TUD_CDC_EP_BUFSIZE  64
