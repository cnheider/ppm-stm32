/*
 *  Copyright (C) 2019  Markus Kasten
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/hid.h>


/***************** PPM decoding *****************/

#define NUM_CHANNELS 8
#define MS_TO_TICKS(x) ((x) * 1000)

volatile int ch_values[NUM_CHANNELS];
volatile int cur_ch = 0;
volatile int is_pause = 0;

void tim2_isr(void) {
    if (TIM2_SR & TIM_SR_CC1IF || TIM2_SR & TIM_SR_CC2IF) {
        uint16_t val;

        // get counter value depending on edge
        if (TIM2_SR & TIM_SR_CC1IF) {
            TIM2_SR &= ~TIM_SR_CC1IF;
            val = TIM2_CCR1;
        } else {
            TIM2_SR &= ~TIM_SR_CC2IF;
            val = TIM2_CCR2;
        }

        TIM2_CNT = 0; // reset counter

        if (val > MS_TO_TICKS(5)) {
            // sync signal
            cur_ch = 0;

            is_pause = 1;

            gpio_toggle(GPIOC, GPIO13);
        } else if (is_pause) {
            is_pause = 0;
        } else if (!is_pause) {
            if (cur_ch < NUM_CHANNELS) {
                ch_values[cur_ch] = val;
                cur_ch++;
            }
            is_pause = 1;
        }
    }
}

static void ic_setup(void) {
    RCC_APB1ENR |= RCC_APB1ENR_TIM2EN;

    // enable TIM2 interrupt and give it a very high priority in order to
    // avoid glitches
    nvic_enable_irq(NVIC_TIM2_IRQ);
    nvic_set_priority(NVIC_TIM2_IRQ, 0);

    TIM2_CR1 = 0;
    TIM2_CR2 = 0;

    // enable input capture
    TIM2_CCMR1 = TIM_CCMR1_CC1S_IN_TI1 | TIM_CCMR1_CC2S_IN_TI1; // both channels on TI1
    TIM2_CCER = TIM_CCER_CC1E | TIM_CCER_CC2E | TIM_CCER_CC2P; // 1 on pos edge, 2 on neg edge

    // enable interrupt
    TIM2_DIER = TIM_DIER_CC1IE | TIM_DIER_CC2IE;

    TIM2_PSC = 72 - 1; // 1us per tick
    TIM2_ARR = 0xFFFF;

    TIM2_CR1 |= TIM_CR1_CEN;
    TIM2_EGR |= TIM_EGR_UG;

    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO0);
}


/***************** USB *****************/

static usbd_device *usbd_dev;

const struct usb_device_descriptor dev_descr = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = 0,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = 64,
	.idVendor = 0x0483,
	.idProduct = 0x5710,
	.bcdDevice = 0x0200,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

static const uint8_t hid_report_descriptor[] = {
    0x05, 0x01, // USAGE_PAGE (Generic Desktop)
    0x09, 0x04, // USAGE (Joystick)

    0xa1, 0x01, // COLLECTION (Application)
    0xa1, 0x00, // COLLECTION (Physical)

    0x05, 0x09, // USAGE_PAGE (Button)
    0x19, 0x01, // USAGE_MINIMUM (Button 1)
    0x29, 0x08, // USAGE_MAXIMUM (Button 8)
    0x15, 0x00, // LOGICAL_MINIMUM (0)
    0x25, 0x01, // LOGICAL_MAXIMUM (1)
    0x95, 0x08, // REPORT_COUNT (8)
    0x75, 0x01, // REPORT_SIZE (1)
    0x81, 0x02, // INPUT (Data,Var,Abs)

    0x05, 0x01, // USAGE_PAGE (Generic Desktop)
    0x15, 0x00, // LOGICAL_MINIMUM (0)
    0x26, 0xff, 0x00, // LOGICAL_MAXIMUM
    0x75, 0x10, // REPORT_SIZE

    0x09, 0x30, // USAGE (X)
    0x09, 0x31, // USAGE (Y)
    0x09, 0x32, // USAGE (Z)
    0x09, 0x33, // USAGE (Rx)
    0x09, 0x34, // USAGE (Ry)
    0x09, 0x35, // USAGE (Rz)
    0x09, 0x36, // USAGE (Throttle)
    0x09, 0x37, // USAGE (Rudder)
    0x95, 0x08, // REPORT_COUNT
    0x81, 0x82, // INPUT (Data,Var,Abs,Vol)

    0xc0, // END_COLLECTION
    0xc0 // END_COLLECTION
};


static const struct {
	struct usb_hid_descriptor hid_descriptor;
	struct {
		uint8_t bReportDescriptorType;
		uint16_t wDescriptorLength;
	} __attribute__((packed)) hid_report;
} __attribute__((packed)) hid_function = {
	.hid_descriptor = {
		.bLength = sizeof(hid_function),
		.bDescriptorType = USB_DT_HID,
		.bcdHID = 0x0111,
		.bCountryCode = 0,
		.bNumDescriptors = 1,
	},
	.hid_report = {
		.bReportDescriptorType = USB_DT_REPORT,
		.wDescriptorLength = sizeof(hid_report_descriptor),
	}
};


const struct usb_endpoint_descriptor hid_endpoint = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x81,
	.bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
	.wMaxPacketSize = 17,
	.bInterval = 10,
};

const struct usb_interface_descriptor hid_iface = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_HID,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.endpoint = &hid_endpoint,

	.extra = &hid_function,
	.extralen = sizeof(hid_function),
};


const struct usb_interface ifaces[] = {{
	.num_altsetting = 1,
	.altsetting = &hid_iface,
}};


const struct usb_config_descriptor config = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0,
	.bNumInterfaces = 1,
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0x80,
	.bMaxPower = 0x32,

	.interface = ifaces,
};


static const char *usb_strings[] = {
	"Ducktronics Inc.",
	"PPM to Joystick Interface",
	"000001",
};

/* Buffer to be used for control requests. */
uint8_t usbd_control_buffer[128];

static enum usbd_request_return_codes hid_control_request(usbd_device *dev, struct usb_setup_data *req, uint8_t **buf, uint16_t *len,
			void (**complete)(usbd_device *, struct usb_setup_data *))
{
	(void)complete;
	(void)dev;

	if((req->bmRequestType != 0x81) ||
	   (req->bRequest != USB_REQ_GET_DESCRIPTOR) ||
	   (req->wValue != 0x2200))
		return USBD_REQ_NOTSUPP;

	// Handle the HID report descriptor.
	*buf = (uint8_t *)hid_report_descriptor;
	*len = sizeof(hid_report_descriptor);

	return USBD_REQ_HANDLED;
}


static void hid_set_config(usbd_device *dev, uint16_t wValue)
{
	(void)wValue;
	(void)dev;

	usbd_ep_setup(dev, 0x81, USB_ENDPOINT_ATTR_INTERRUPT, 17, NULL);

	usbd_register_control_callback(
				dev,
				USB_REQ_TYPE_STANDARD | USB_REQ_TYPE_INTERFACE,
				USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
				hid_control_request);

    TIM3_CR1 |= TIM_CR1_CEN;
}

static void usb_setup(void) {
    RCC_APB1ENR |= RCC_APB1ENR_USBEN;

    RCC_APB1ENR |= RCC_APB1ENR_TIM3EN;
    nvic_enable_irq(NVIC_TIM3_IRQ);
    TIM3_CR1 = TIM_CR1_ARPE;
    TIM3_PSC = 72 - 1;
    TIM3_ARR = 10000 - 1; // 100 Hz
    TIM3_DIER = TIM_DIER_UIE;
}

void tim3_isr() {
    if (TIM3_SR & TIM_SR_UIF) {
        TIM3_SR &= ~TIM_SR_UIF;

        char buf[17];
        buf[0] = 0;
        for (int i = 0; i < NUM_CHANNELS; i++) {
            buf[(i*2)+1] = (ch_values[i] - 600) / 4;
            buf[(i*2)+2] = 0;
        }

        uint16_t ret = usbd_ep_write_packet(usbd_dev, 0x81, (uint8_t *)buf, sizeof(buf));
    }
}

/***************** main *****************/

int main(void) {
    rcc_clock_setup_in_hse_8mhz_out_72mhz();
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO13);
    gpio_clear(GPIOC, GPIO13);

    /* Pull down for nSRST */
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_2_MHZ,
                  GPIO_CNF_OUTPUT_PUSHPULL, 0);
    gpio_clear(GPIOB, 0);

    ic_setup();
    usb_setup();

    /*
	 * Vile hack to reenumerate, physically _drag_ d+ low.
	 * (need at least 2.5us to trigger usb disconnect)
	 */
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_2_MHZ,
		GPIO_CNF_OUTPUT_PUSHPULL, GPIO12);
    gpio_clear(GPIOA, GPIO12);

    for (int i = 0; i < 800000; i++)
        __asm__("nop");

    usbd_dev = usbd_init(&st_usbfs_v1_usb_driver, &dev_descr, &config, usb_strings, 3, usbd_control_buffer, sizeof(usbd_control_buffer));
    usbd_register_set_config_callback(usbd_dev, hid_set_config);

	while (1) {
        usbd_poll(usbd_dev);
    }
}
