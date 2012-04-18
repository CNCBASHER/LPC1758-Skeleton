//#define POLLED_USBSERIAL TRUE


/*
	LPCUSB, an USB device driver for LPC microcontrollers
	Copyright (C) 2006 Bertrik Sikken (bertrik@sikken.nl)

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	1. Redistributions of source code must retain the above copyright
	   notice, this list of conditions and the following disclaimer.
	2. Redistributions in binary form must reproduce the above copyright
	   notice, this list of conditions and the following disclaimer in the
	   documentation and/or other materials provided with the distribution.
	3. The name of the author may not be used to endorse or promote products
	   derived from this software without specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
	IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
	OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
	IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
	INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
	NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
	DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
	THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
	(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
	THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
	Minimal implementation of a USB serial port, using the CDC class.
	This example application simply echoes everything it receives right back
	to the host.

	Windows:
	Extract the usbser.sys file from .cab file in C:\WINDOWS\Driver Cache\i386
	and store it somewhere (C:\temp is a good place) along with the usbser.inf
	file. Then plug in the LPC176x and direct windows to the usbser driver.
	Windows then creates an extra COMx port that you can open in a terminal
	program, like hyperterminal.

	Linux:
	The device should be recognised automatically by the cdc_acm driver,
	which creates a /dev/ttyACMx device file that acts just like a regular
	serial port.

*/

// CodeRed
// Added ref to stdio.h to pull in semihosted printf rather than using serial


#include <stdio.h>

//#include <cr_section_macros.h>
//#include <NXP/crp.h>

// Variable to store CRP value in. Will be placed automatically
// by the linker when "Enable Code Read Protect" selected.
// See crp.h header for more information
//__CRP const unsigned int CRP_WORD = CRP_NO_CRP ;

#include <string.h>			// memcpy

#include "LPC17xx.h"

#include "usbapi.h"
#include "usbdebug.h"

#include "serial_fifo.h"

#include "descriptor.h"

void dbgled(int l);

// CodeRed
// Control how the character received by the board is echoed back to the host
// Set to 1 to increment character ('a' echoed as 'b'), else set to 0
#define INCREMENT_ECHO_BY 1
//#define INCREMENT_ECHO_BY 0


#define BAUD_RATE	115200

#define INT_IN_EP                0x81
#define BULK_OUT_EP              0x05
#define BULK_IN_EP               0x82

#define MAX_PACKET_SIZE          64

#define LE_WORD(x)               ((x)&0xFF),((x)>>8)

// CDC definitions
#define CS_INTERFACE             0x24
#define CS_ENDPOINT              0x25

#define	SET_LINE_CODING          0x20
#define	GET_LINE_CODING          0x21
#define	SET_CONTROL_LINE_STATE   0x22

// data structure for GET_LINE_CODING / SET_LINE_CODING class requests
typedef struct {
	U32		dwDTERate;
	U8		bCharFormat;
	U8		bParityType;
	U8		bDataBits;
} TLineCoding;

static TLineCoding LineCoding = {115200, 0, 0, 8};
static U8 abBulkBuf[128];
static U8 abClassReqData[8];

static U8 txdata[VCOM_FIFO_SIZE];
static U8 rxdata[VCOM_FIFO_SIZE];

static fifo_t txfifo;
static fifo_t rxfifo;

// forward declaration of interrupt handler
void USBIntHandler(void);

static const struct {
	usbdesc_device				device;
	usbdesc_configuration	config0;
	usbdesc_interface			if0;
	usbcdc_header					fd_header;
	usbcdc_union					fd_union;
	usbcdc_ether					fd_ether;
	usbdesc_endpoint			ep_notify;
	usbdesc_interface			if_nop;
	usbdesc_interface			if_data;
	usbdesc_endpoint			ep_bulkout;
	usbdesc_endpoint			ep_bulkin;
	usbdesc_language			st_language;
	usbdesc_string_l(6)		st_Manufacturer;
	usbdesc_string_l(9)		st_Product;
	usbdesc_string_l(8)		st_Serial;
	usbdesc_string_l(12)	st_MAC;
	U8										end;

} abDescriptors = {
	.device = {
		DL_DEVICE,
		DT_DEVICE,
		.bcdUSB							= USB_VERSION_1_1,
		.bDeviceClass				= UC_COMM,
		.bDeviceSubClass		= 0,
		.bDeviceProtocol		= 0,
		.bMaxPacketSize			= MAX_PACKET_SIZE0,
		.idVendor						= 0xFFFF,
		.idProduct					= 0x0005,
		.bcdDevice					= 0x0100,
		.iManufacturer			= 0x01,
		.iProduct						= 0x02,
		.iSerialNumber			= 0x03,
		.bNumConfigurations	= 1,
	},
	.config0 = {
		DL_CONFIGURATION,
		DT_CONFIGURATION,
		.wTotalLength				= sizeof(usbdesc_configuration)
												+ sizeof(usbdesc_interface)
												+ sizeof(usbcdc_header)
												+ sizeof(usbcdc_union)
												+ sizeof(usbcdc_ether)
												+ sizeof(usbdesc_endpoint)
												+ sizeof(usbdesc_interface)
												+ sizeof(usbdesc_interface)
												+ sizeof(usbdesc_endpoint)
												+ sizeof(usbdesc_endpoint)
												,
		.bNumInterfaces			= 2,
		.bConfigurationValue = 1,
		.iConfiguration			= 0,
		.bmAttributes				= CA_BUSPOWERED,
		.bMaxPower					= 100 mA,
	},
	.if0 = {
		DL_INTERFACE,
		DT_INTERFACE,
		.bInterfaceNumber		= 0,
		.bAlternateSetting	= 0,
		.bNumEndPoints			= 1,
		.bInterfaceClass		= UC_COMM,
		.bInterfaceSubClass	= USB_CDC_SUBCLASS_ETHERNET,
		.bInterfaceProtocol	= 0, // linux requires value of 1 for the cdc_acm module
		.iInterface					= 0,
	},
	.fd_header = {
		USB_CDC_LENGTH_HEADER,
		DT_CDC_DESCRIPTOR,
		USB_CDC_SUBTYPE_HEADER,
		.bcdCDC = 0x0110,
	},
	.fd_union = {
		USB_CDC_LENGTH_UNION,
		DT_CDC_DESCRIPTOR,
		USB_CDC_SUBTYPE_UNION,
		.bMasterInterface = 0,
		.bSlaveInterface0 = 1,
	},
	.fd_ether = {
		USB_CDC_LENGTH_ETHER,
		DT_CDC_DESCRIPTOR,
		USB_CDC_SUBTYPE_ETHERNET,
		.iMacAddress = 4,
		.bmEthernetStatistics = 0,
		.wMaxSegmentSize = 1514,
		.wNumberMCFilters = 0,
		.bNumberPowerFilters = 0,
	},
	.ep_notify = {
		DL_ENDPOINT,
		DT_ENDPOINT,
		.bEndpointAddress = INT_IN_EP,
		.bmAttributes = EA_INTERRUPT,
		.wMaxPacketSize = 8,
		.bInterval = 10,
	},
	.if_nop = {
		DL_INTERFACE,
		DT_INTERFACE,
		.bInterfaceNumber = 1,
		.bAlternateSetting = 0,
		.bNumEndPoints = 0,
		.bInterfaceClass = UC_CDC_DATA,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.if_data = {
		DL_INTERFACE,
		DT_INTERFACE,
		.bInterfaceNumber = 1,
		.bAlternateSetting = 1,
		.bNumEndPoints = 2,
		.bInterfaceClass = UC_CDC_DATA,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.ep_bulkout = {
		DL_ENDPOINT,
		DT_ENDPOINT,
		.bEndpointAddress = BULK_OUT_EP,
		.bmAttributes = EA_BULK,
		.wMaxPacketSize = MAX_PACKET_SIZE,
		.bInterval = 0,
	},
	.ep_bulkin = {
		DL_ENDPOINT,
		DT_ENDPOINT,
		.bEndpointAddress = BULK_IN_EP,
		.bmAttributes = EA_BULK,
		.wMaxPacketSize = MAX_PACKET_SIZE,
		.bInterval = 0,
	},
	.st_language = {
		.bLength = 4,
		DT_STRING,
		{ SL_USENGLISH, },
	},
	.st_Manufacturer = {
		14,
		DT_STRING,
		{ 'L','P','C','U','S','B', },
	},
	.st_Product = {
		20,
		DT_STRING,
		{ 'U','S','B','S','e','r','i','a','l', },
	},
	.st_Serial = {
		18,
		DT_STRING,
		{ 'D','E','A','D','B','E','E','F', },
	},
	.st_MAC = {
		26,
		DT_STRING,
		{ '0','6','2','D','2','8','3','A','9','D','3','B', },
	},
	0,
};

/**
	Local function to handle incoming bulk data

	@param [in] bEP
	@param [in] bEPStatus
 */
static void BulkOut(U8 bEP, U8 bEPStatus)
{
	int i, iLen;

	printf("OUT EP %d", bEP);

	if (fifo_free(&rxfifo) < MAX_PACKET_SIZE) {
		// may not fit into fifo
		printf(" full\n");
		return;
	}

	// get data from USB into intermediate buffer
	iLen = USBHwEPRead(bEP, abBulkBuf, sizeof(abBulkBuf));

	printf(" got %d\n", iLen);

	for (i = 0; i < iLen; i++) {
		// put into FIFO
		if (!fifo_put(&rxfifo, abBulkBuf[i])) {
			// overflow... :(
			ASSERT(FALSE);
			break;
		}
	}
}


/**
	Local function to handle outgoing bulk data

	@param [in] bEP
	@param [in] bEPStatus
 */
static void BulkIn(U8 bEP, U8 bEPStatus)
{
	int i, iLen;

	printf("IN EP %d", bEP);

	if (fifo_avail(&txfifo) == 0) {
		// no more data, disable further NAK interrupts until next USB frame
		USBHwNakIntEnable(0);
		printf(" empty\n");
		return;
	}

	// get bytes from transmit FIFO into intermediate buffer
	for (i = 0; i < MAX_PACKET_SIZE; i++) {
		if (!fifo_get(&txfifo, &abBulkBuf[i])) {
			break;
		}
	}
	iLen = i;

	printf(" sent %d\n", iLen);

	// send over USB
	if (iLen > 0) {
		USBHwEPWrite(bEP, abBulkBuf, iLen);
	}
}


/**
	Local function to handle the USB-CDC class requests

	@param [in] pSetup
	@param [out] piLen
	@param [out] ppbData
 */
static BOOL HandleClassRequest(TSetupPacket *pSetup, int *piLen, U8 **ppbData)
{
	printf("handle %x %x %d %d %d\n", pSetup->bmRequestType, pSetup->bRequest, pSetup->wValue, pSetup->wIndex, pSetup->wLength);
	switch (pSetup->bRequest) {

	// set line coding
	case SET_LINE_CODING:
		DBG("SET_LINE_CODING\n");
		memcpy((U8 *)&LineCoding, *ppbData, 7);
		*piLen = 7;
		DBG("dwDTERate=%u, bCharFormat=%u, bParityType=%u, bDataBits=%u\n",
		LineCoding.dwDTERate,
		LineCoding.bCharFormat,
		LineCoding.bParityType,
		LineCoding.bDataBits);
		break;

	// get line coding
	case GET_LINE_CODING:
		DBG("GET_LINE_CODING\n");
		*ppbData = (U8 *)&LineCoding;
		*piLen = 7;
		break;

	// set control line state
	case SET_CONTROL_LINE_STATE:
		// bit0 = DTR, bit = RTS
		DBG("SET_CONTROL_LINE_STATE %X\n", pSetup->wValue);
		break;

	default:
		printf("Unknown CLASS Request: %d\n", pSetup->bRequest);
		return FALSE;
	}
	return TRUE;
}


/**
	Initialises the VCOM port.
	Call this function before using VCOM_putchar or VCOM_getchar
 */
void VCOM_init(void)
{
	fifo_init(&txfifo, txdata);
	fifo_init(&rxfifo, rxdata);
}


/**
	Writes one character to VCOM port

	@param [in] c character to write
	@returns character written, or EOF if character could not be written
 */
int VCOM_putchar(int c)
{
	return fifo_put(&txfifo, c) ? c : EOF;
}


/**
	Reads one character from VCOM port

	@returns character read, or EOF if character could not be read
 */
int VCOM_getchar(void)
{
	U8 c;

	return fifo_get(&rxfifo, &c) ? c : EOF;
}


/**
	Interrupt handler

	Simply calls the USB ISR
 */
//void USBIntHandler(void)
void USB_IRQHandler(void)
{
	static int l;
	dbgled((++l) >> 8);
	USBHwISR();
//	dbgled(0);
}


static void USBFrameHandler(U16 wFrame)
{
	if (fifo_avail(&txfifo) > 0) {
		// data available, enable NAK interrupt on bulk in
		USBHwNakIntEnable(INACK_BI);
	}
}

//void enable_USB_interrupts(void);


/*************************************************************************
	main
	====
**************************************************************************/
int main(void)
{
	int c;

	dbgled(0);

	//NVIC_DeInit();
	//NVIC_SCBDeInit();
	//NVIC_SetVTOR((uint32_t) &__cs3_interrupt_vector_cortex_m);

	//extern void* __cs3_interrupt_vector_cortex_m;
	//printf("VTOR is %p = 0x%08lX\n", &__cs3_interrupt_vector_cortex_m, SCB->VTOR);

	dbgled(1);

// 	printf("abDescriptors  : ");
// 	for (int i = 0; i < sizeof(abDescriptors); i++) {
// 		printf("0x%02X ", ((uint8_t *) &abDescriptors)[i]);
// 	}
// 	printf("\nabDescriptors_s: ");
// 	for (int i = 0; i < sizeof(abDescriptors_s); i++) {
// 		printf("0x%02X ", abDescriptors_s[i]);
// 	}
	printf("Initialising USB stack\n");

	// initialise stack
	USBInit();

	// register descriptors
	USBRegisterDescriptors((uint8_t *) &abDescriptors);

	// register class request handler
	USBRegisterRequestHandler(REQTYPE_TYPE_CLASS, HandleClassRequest, abClassReqData);

	// register endpoint handlers
	USBHwRegisterEPIntHandler(INT_IN_EP, NULL);
	USBHwRegisterEPIntHandler(BULK_IN_EP, BulkIn);
	USBHwRegisterEPIntHandler(BULK_OUT_EP, BulkOut);

	// register frame handler
	USBHwRegisterFrameHandler(USBFrameHandler);

	// enable bulk-in interrupts on NAKs
	USBHwNakIntEnable(INACK_BI);

	dbgled(2);

	// initialise VCOM
	VCOM_init();
	printf("Starting USB communication\n");

/* CodeRed - comment out original interrupt setup code
	// set up USB interrupt
	VICIntSelect &= ~(1<<22);               // select IRQ for USB
	VICIntEnable |= (1<<22);

	(*(&VICVectCntl0+INT_VECT_NUM)) = 0x20 | 22; // choose highest priority ISR slot
	(*(&VICVectAddr0+INT_VECT_NUM)) = (int)USBIntHandler;

	enableIRQ();
*/

// CodeRed - add in interrupt setup code for RDB1768

	dbgled(3);

#ifndef POLLED_USBSERIAL
	//enable_USB_interrupts();
	NVIC_EnableIRQ(USB_IRQn);
	dbgled(4);
	LPC_SC->USBIntSt |= 0x80000000;
	dbgled(5);
#endif

	dbgled(6);
	// connect to bus

	printf("Connecting to USB bus\n");
	dbgled(7);
	USBHwConnect(TRUE);

	dbgled(8);

	volatile static int i = 0 ;

	// echo any character received (do USB stuff in interrupt)
	while (1) {

		i++ ;

		// CodeRed - add option to use polling rather than interrupt
#ifdef POLLED_USBSERIAL

		USBHwISR();

#endif

		c = VCOM_getchar();
		if (c != EOF) {
			// show on console
			if ((c == 9) || (c == 10) || (c == 13) || ((c >= 32) && (c <= 126))) {
				printf("%c", c);
			}
			else {
				printf(".");
			}

// CodeRed
// Echo character back as is, or incremented, as per #define.
			//VCOM_putchar(c + INCREMENT_ECHO_BY );
		}
	}

	return 0;
}

