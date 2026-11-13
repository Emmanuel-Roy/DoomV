// USB descriptors: one device, three CDC-ACM serial ports (console, link,
// and the half-size video core 1 streams: core1_io.c).
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#define USB_VID 0x2E8A   // Raspberry Pi
#define USB_PID 0x4D44   // arbitrary, for this project ("MD")

static const tusb_desc_device_t device = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	// Interface Association: required for composite CDC.
	.bDeviceClass = TUSB_CLASS_MISC,
	.bDeviceSubClass = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = USB_VID,
	.idProduct = USB_PID,
	.bcdDevice = 0x0100,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
	return (const uint8_t *)&device;
}

enum { ITF_CONSOLE, ITF_CONSOLE_DATA, ITF_LINK, ITF_LINK_DATA, ITF_VIDEO, ITF_VIDEO_DATA, ITF_COUNT };

#define EP_CONSOLE_NOTIF 0x81
#define EP_CONSOLE_OUT   0x02
#define EP_CONSOLE_IN    0x82
#define EP_LINK_NOTIF    0x83
#define EP_LINK_OUT      0x04
#define EP_LINK_IN       0x84
#define EP_VIDEO_NOTIF   0x85
#define EP_VIDEO_OUT     0x06
#define EP_VIDEO_IN      0x86

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + 3 * TUD_CDC_DESC_LEN)

static const uint8_t configuration[] = {
	TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0x00, 100),
	TUD_CDC_DESCRIPTOR(ITF_CONSOLE, 4, EP_CONSOLE_NOTIF, 8, EP_CONSOLE_OUT, EP_CONSOLE_IN, 64),
	TUD_CDC_DESCRIPTOR(ITF_LINK, 5, EP_LINK_NOTIF, 8, EP_LINK_OUT, EP_LINK_IN, 64),
	TUD_CDC_DESCRIPTOR(ITF_VIDEO, 6, EP_VIDEO_NOTIF, 8, EP_VIDEO_OUT, EP_VIDEO_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
	return configuration;
}

static const char *strings[] = {
	NULL,                    // 0: language (handled below)
	"Pico2Doom",             // 1: manufacturer
	"Pico2Doom (RP2350)",    // 2: product
	NULL,                    // 3: serial: the board's unique ID
	"Pico2Doom console",     // 4: CDC 0
	"Pico2Doom link",        // 5: CDC 1
	"Pico2Doom video",       // 6: CDC 2
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	(void)langid;
	static uint16_t out[33];
	char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
	const char *s;
	int n;

	if (index == 0) {
		out[1] = 0x0409;   // English
		n = 1;
	} else {
		if (index >= sizeof strings / sizeof strings[0]) return NULL;
		if (index == 3) {
			pico_get_unique_board_id_string(serial, sizeof serial);
			s = serial;
		} else {
			s = strings[index];
		}
		n = (int)strlen(s);
		if (n > 32) n = 32;
		for (int i = 0; i < n; i++) out[1 + i] = (uint8_t)s[i];
	}
	out[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
	return out;
}
