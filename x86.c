/*
 * Copyright (c) 2014, Aleksander Osman
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h> 
#include <fcntl.h>

#include "spi.h"
#include "user_io.h"
#include "file_io.h"
#include "fpga_io.h"

#define ALT_CPU_CPU_FREQ 90000000u

#define FLOPPY_BASE      0x8800
#define HDD_BASE         0x8840 
#define PC_BUS_BASE      0x88a0 
#define PIO_OUTPUT_BASE  0x8860 
#define SOUND_BASE       0x9000 
#define PIT_BASE         0x8880 
#define RTC_BASE         0x8c00 
#define SD_BASE          0x0A00 

#define CFG_VER          1

typedef struct
{
	uint32_t ver;
	char fdd_name[1024];
	char hdd_name[1024];
} x86_config;

static x86_config config;

static uint8_t dma_sdio(int status)
{
	uint8_t res;
	EnableFpga();
	spi8(UIO_DMA_SDIO);
	res = spi_w((uint16_t)status);
	DisableFpga();
	return res;
}

static uint32_t dma_get(uint32_t address)
{
	EnableFpga();
	spi8(UIO_DMA_READ);
	spi32w(address);
	uint32_t res = spi32w(0);
	DisableFpga();
	return res;
}

static void dma_set(uint32_t address, uint32_t data)
{
	EnableFpga();
	spi8(UIO_DMA_WRITE);
	spi32w(address);
	spi32w(data);
	DisableFpga();
}

static void dma_sendbuf(uint32_t address, uint32_t length, uint32_t *data)
{
	EnableFpga();
	spi8(UIO_DMA_WRITE);
	spi32w(address);
	while (length--) spi32w(*data++);
	DisableFpga();
}

static void dma_rcvbuf(uint32_t address, uint32_t length, uint32_t *data)
{
	EnableFpga();
	spi8(UIO_DMA_READ);
	spi32w(address);
	while (length--) *data++ = spi32w(0);
	DisableFpga();
}

static int load_bios(char* name, uint8_t index)
{
	fileTYPE f = { 0 };
	static uint32_t buf[128];

	if (!FileOpen(&f, name)) return 0;

	unsigned long bytes2send = f.size;
	printf("BIOS %s, %lu bytes.\n", name, bytes2send);

	EnableFpga();
	spi8(UIO_DMA_WRITE);
	spi32w( index ? 0x80C0000 : 0x80F0000 );

	while (bytes2send)
	{
		printf(".");

		uint16_t chunk = (bytes2send>512) ? 512 : bytes2send;
		bytes2send -= chunk;

		FileReadSec(&f, buf);

		chunk = (chunk + 3) >> 2;
		uint32_t* p = buf;
		while(chunk--) spi32w(*p++);
	}
	DisableFpga();
	FileClose(&f);

	printf("\n");
	return 1;
}

static void crc32(uint8_t *ptr, uint32_t *crc_output)
{
    static uint8_t crc[32];

    //do nothing
    if(ptr != NULL && crc_output != NULL) return;

    //initialize
    if(ptr == NULL && crc_output == NULL)
	{
    	for(int i=0; i<32; i++) crc[i] = 1;
    	return;
    }

    //output
    if(ptr == NULL && crc_output != NULL)
	{
    	*crc_output = 0;
		for(int i=0; i<32; i++)
		{
			(*crc_output) |= crc[i] << (31-i);
		}
		(*crc_output) = ~(*crc_output);
		return;
    }

    uint8_t in[8];
    for(int j=0; j<8; j++) in[j] = ((*ptr) >> j) & 1;

    uint8_t new_crc[32];

	new_crc[31] = in[2] ^ crc[23] ^ crc[29];
	new_crc[30] = in[0] ^ in[3] ^ crc[22] ^ crc[28] ^ crc[31];
	new_crc[29] = in[0] ^ in[1] ^ in[4] ^ crc[21] ^ crc[27] ^ crc[30] ^ crc[31];
	new_crc[28] = in[1] ^ in[2] ^ in[5] ^ crc[20] ^ crc[26] ^ crc[29] ^ crc[30];
	new_crc[27] = in[0] ^ in[2] ^ in[3] ^ in[6] ^ crc[19] ^ crc[25] ^ crc[28] ^ crc[29] ^ crc[31];
	new_crc[26] = in[1] ^ in[3] ^ in[4] ^ in[7] ^ crc[18] ^ crc[24] ^ crc[27] ^ crc[28] ^ crc[30];
	new_crc[25] = in[4] ^ in[5] ^ crc[17] ^ crc[26] ^ crc[27];
	new_crc[24] = in[0] ^ in[5] ^ in[6] ^ crc[16] ^ crc[25] ^ crc[26] ^ crc[31];
	new_crc[23] = in[1] ^ in[6] ^ in[7] ^ crc[15] ^ crc[24] ^ crc[25] ^ crc[30];
	new_crc[22] = in[7] ^ crc[14] ^ crc[24];
	new_crc[21] = in[2] ^ crc[13] ^ crc[29];
	new_crc[20] = in[3] ^ crc[12] ^ crc[28];
	new_crc[19] = in[0] ^ in[4] ^ crc[11] ^ crc[27] ^ crc[31];
	new_crc[18] = in[0] ^ in[1] ^ in[5] ^ crc[10] ^ crc[26] ^ crc[30] ^ crc[31];
	new_crc[17] = in[1] ^ in[2] ^ in[6] ^ crc[9] ^ crc[25] ^ crc[29] ^ crc[30];
	new_crc[16] = in[2] ^ in[3] ^ in[7] ^ crc[8] ^ crc[24] ^ crc[28] ^ crc[29];
	new_crc[15] = in[0] ^ in[2] ^ in[3] ^ in[4] ^ crc[7] ^ crc[27] ^ crc[28] ^ crc[29] ^ crc[31];
	new_crc[14] = in[0] ^ in[1] ^ in[3] ^ in[4] ^ in[5] ^ crc[6] ^ crc[26] ^ crc[27] ^ crc[28] ^ crc[30] ^ crc[31];
	new_crc[13] = in[0] ^ in[1] ^ in[2] ^ in[4] ^ in[5] ^ in[6] ^ crc[5] ^ crc[25] ^ crc[26] ^ crc[27] ^ crc[29] ^ crc[30] ^ crc[31];
	new_crc[12] = in[1] ^ in[2] ^ in[3] ^ in[5] ^ in[6] ^ in[7] ^ crc[4] ^ crc[24] ^ crc[25] ^ crc[26] ^ crc[28] ^ crc[29] ^ crc[30];
	new_crc[11] = in[3] ^ in[4] ^ in[6] ^ in[7] ^ crc[3] ^ crc[24] ^ crc[25] ^ crc[27] ^ crc[28];
	new_crc[10] = in[2] ^ in[4] ^ in[5] ^ in[7] ^ crc[2] ^ crc[24] ^ crc[26] ^ crc[27] ^ crc[29];
	new_crc[9] = in[2] ^ in[3] ^ in[5] ^ in[6] ^ crc[1] ^ crc[25] ^ crc[26] ^ crc[28] ^ crc[29];
	new_crc[8] = in[3] ^ in[4] ^ in[6] ^ in[7] ^ crc[0] ^ crc[24] ^ crc[25] ^ crc[27] ^ crc[28];
	new_crc[7] = in[0] ^ in[2] ^ in[4] ^ in[5] ^ in[7] ^ crc[24] ^ crc[26] ^ crc[27] ^ crc[29] ^ crc[31];
	new_crc[6] = in[0] ^ in[1] ^ in[2] ^ in[3] ^ in[5] ^ in[6] ^ crc[25] ^ crc[26] ^ crc[28] ^ crc[29] ^ crc[30] ^ crc[31];
	new_crc[5] = in[0] ^ in[1] ^ in[2] ^ in[3] ^ in[4] ^ in[6] ^ in[7] ^ crc[24] ^ crc[25] ^ crc[27] ^ crc[28] ^ crc[29] ^ crc[30] ^ crc[31];
	new_crc[4] = in[1] ^ in[3] ^ in[4] ^ in[5] ^ in[7] ^ crc[24] ^ crc[26] ^ crc[27] ^ crc[28] ^ crc[30];
	new_crc[3] = in[0] ^ in[4] ^ in[5] ^ in[6] ^ crc[25] ^ crc[26] ^ crc[27] ^ crc[31];
	new_crc[2] = in[0] ^ in[1] ^ in[5] ^ in[6] ^ in[7] ^ crc[24] ^ crc[25] ^ crc[26] ^ crc[30] ^ crc[31];
	new_crc[1] = in[0] ^ in[1] ^ in[6] ^ in[7] ^ crc[24] ^ crc[25] ^ crc[30] ^ crc[31];
	new_crc[0] = in[1] ^ in[7] ^ crc[24] ^ crc[30];

    memcpy(crc, new_crc, sizeof(crc));
}

static bool floppy_is_160k = false;
static bool floppy_is_180k = false;
static bool floppy_is_320k = false;
static bool floppy_is_360k = false;
static bool floppy_is_720k = false;
static bool floppy_is_1_2m = false;
static bool floppy_is_1_44m= false;
static bool floppy_is_1_68m= false;
static bool floppy_is_2_88m= false;

#define CMOS_FDD_TYPE ((floppy_is_2_88m) ? 0x50 : (floppy_is_1_44m || floppy_is_1_68m) ? 0x40 : (floppy_is_720k) ? 0x30 : (floppy_is_1_2m) ? 0x20 : 0x10)

static fileTYPE fdd_image = { 0 };
static fileTYPE hdd_image = { 0 };
static bool boot_from_floppy = 1;

#define IMG_TYPE_FDD 0x800
#define IMG_TYPE_HDD 0x000

static __inline fileTYPE *get_image(uint32_t type)
{
	return (type == IMG_TYPE_FDD) ? &fdd_image : &hdd_image;
}

static int img_mount(uint32_t type, char *name)
{
	FileClose(get_image(type));

	int writable = FileCanWrite(name);
	int ret = FileOpenEx(get_image(type), name, writable ? (O_RDWR | O_SYNC) : O_RDONLY);
	if (!ret)
	{
		get_image(type)->size = 0;
		printf("Failed to open file %s\n", name);
		return 0;
	}

	printf("Mount %s as %s\n", name, writable ? "read-write" : "read-only");
	return 1;
}

static int img_read(uint32_t type, uint32_t lba, void *buf, uint32_t len)
{
	if (!FileSeekLBA(get_image(type), lba)) return 0;
	return FileReadAdv(get_image(type), buf, len);
}

static int img_write(uint32_t type, uint32_t lba, void *buf, uint32_t len)
{
	if (!FileSeekLBA(get_image(type), lba)) return 0;
	return FileWriteAdv(get_image(type), buf, len);
}

#define IOWR(base, reg, value) dma_set(base+(reg<<2), value)

static uint32_t cmos[128];

void cmos_set(int addr, uint8_t val)
{
	if (addr >= sizeof(cmos)) return;

	cmos[addr] = val;
	return;

	uint16_t sum = 0;
	for (int i = 0x10; i <= 0x2D; i++) sum += cmos[i];

	cmos[0x2E] = sum >> 8;
	cmos[0x2F] = sum & 0xFF;

	IOWR(RTC_BASE, addr, cmos[addr]);

	IOWR(RTC_BASE, 0x2E, cmos[0x2E]);
	IOWR(RTC_BASE, 0x2F, cmos[0x2F]);
}

static int fdd_set(char* filename)
{
	floppy_is_160k = false;
	floppy_is_180k = false;
	floppy_is_320k = false;
	floppy_is_360k = false;
	floppy_is_720k = false;
	floppy_is_1_2m = false;
	floppy_is_1_44m = false;
	floppy_is_1_68m = false;
	floppy_is_2_88m = false;

	int floppy = img_mount(IMG_TYPE_FDD, filename);
	uint32_t size = get_image(IMG_TYPE_FDD)->size/512;
	if (floppy && size)
	{
		if (size >= 5760) floppy_is_2_88m = true;
		else if (size >= 3360) floppy_is_1_68m = true;
		else if (size >= 2880) floppy_is_1_44m = true;
		else if (size >= 2400) floppy_is_1_2m = true;
		else if (size >= 1440) floppy_is_720k = true;
		else if (size >= 720) floppy_is_360k = true;
		else if (size >= 640) floppy_is_320k = true;
		else if (size >= 360) floppy_is_180k = true;
		else floppy_is_160k = true;
	}
	else
	{
		floppy = 0;
		floppy_is_1_44m = true;
	}

	/*
	0x00.[0]:      media present
	0x01.[0]:      media writeprotect
	0x02.[7:0]:    media cylinders
	0x03.[7:0]:    media sectors per track
	0x04.[31:0]:   media total sector count
	0x05.[1:0]:    media heads
	0x06.[31:0]:   media sd base
	0x07.[15:0]:   media wait cycles: 200000 us / spt
	0x08.[15:0]:   media wait rate 0: 1000 us
	0x09.[15:0]:   media wait rate 1: 1666 us
	0x0A.[15:0]:   media wait rate 2: 2000 us
	0x0B.[15:0]:   media wait rate 3: 500 us
	0x0C.[7:0]:    media type: 8'h20 none; 8'h00 old; 8'hC0 720k; 8'h80 1_44M; 8'h40 2_88M
	*/

	int floppy_spt =
		(floppy_is_160k) ? 8 :
		(floppy_is_180k) ? 9 :
		(floppy_is_320k) ? 8 :
		(floppy_is_360k) ? 9 :
		(floppy_is_720k) ? 9 :
		(floppy_is_1_2m) ? 15 :
		(floppy_is_1_44m) ? 18 :
		(floppy_is_1_68m) ? 21 :
		(floppy_is_2_88m) ? 36 :
		0;

	int floppy_cylinders     = (floppy_is_2_88m || floppy_is_1_68m || floppy_is_1_44m || floppy_is_1_2m || floppy_is_720k) ? 80 : 40;
	int floppy_heads         = (floppy_is_160k || floppy_is_180k) ? 1 : 2;
	int floppy_total_sectors = floppy_spt * floppy_heads * floppy_cylinders;
	int floppy_wait_cycles   = 200000000 / floppy_spt;

	int floppy_media =
		(!floppy) ? 0x20 :
		(floppy_is_160k) ? 0x00 :
		(floppy_is_180k) ? 0x00 :
		(floppy_is_320k) ? 0x00 :
		(floppy_is_360k) ? 0x00 :
		(floppy_is_720k) ? 0xC0 :
		(floppy_is_1_2m) ? 0x00 :
		(floppy_is_1_44m) ? 0x80 :
		(floppy_is_2_88m) ? 0x40 :
		0x20;

	IOWR(FLOPPY_BASE, 0x0, floppy ? 1 : 0);
	IOWR(FLOPPY_BASE, 0x1, (floppy && (get_image(IMG_TYPE_FDD)->mode & O_RDWR)) ? 0 : 1);
	IOWR(FLOPPY_BASE, 0x2, floppy_cylinders);
	IOWR(FLOPPY_BASE, 0x3, floppy_spt);
	IOWR(FLOPPY_BASE, 0x4, floppy_total_sectors);
	IOWR(FLOPPY_BASE, 0x5, floppy_heads);
	IOWR(FLOPPY_BASE, 0x6, 0); // base LBA
	IOWR(FLOPPY_BASE, 0x7, (int)(floppy_wait_cycles / (1000000000.0 / ALT_CPU_CPU_FREQ)));
	IOWR(FLOPPY_BASE, 0x8, (int)(1000000.0 / (1000000000.0 / ALT_CPU_CPU_FREQ)));
	IOWR(FLOPPY_BASE, 0x9, (int)(1666666.0 / (1000000000.0 / ALT_CPU_CPU_FREQ)));
	IOWR(FLOPPY_BASE, 0xA, (int)(2000000.0 / (1000000000.0 / ALT_CPU_CPU_FREQ)));
	IOWR(FLOPPY_BASE, 0xB, (int)(500000.0 / (1000000000.0 / ALT_CPU_CPU_FREQ)));
	IOWR(FLOPPY_BASE, 0xC, floppy_media);

	//cmos_set(0x10, CMOS_FDD_TYPE);
	return floppy;
}

void x86_init()
{
	user_io_8bit_set_status(UIO_STATUS_RESET, UIO_STATUS_RESET);

	load_bios("ao486/boot0.rom", 0);
	load_bios("ao486/boot1.rom", 1);

	IOWR(PC_BUS_BASE, 0, 0x00FFF0EA);
	IOWR(PC_BUS_BASE, 1, 0x000000F0);

	//-------------------------------------------------------------------------- sound
	/*
	0-255.[15:0]: cycles in period
	256.[12:0]:  cycles in 80us
	257.[9:0]:   cycles in 1 sample: 96000 Hz
	*/

	double cycle_in_ns = (1000000000.0 / ALT_CPU_CPU_FREQ); //33.333333;
    for(int i=0; i<256; i++)
	{
        double f = 1000000.0 / (256.0-i);

        double cycles_in_period = 1000000000.0 / (f * cycle_in_ns);
        IOWR(SOUND_BASE, i, (int)cycles_in_period);
    }

	IOWR(SOUND_BASE, 256, (int)(80000.0 / (1000000000.0 / ALT_CPU_CPU_FREQ)));
	IOWR(SOUND_BASE, 257, (int)((1000000000.0/96000.0) / (1000000000.0 / ALT_CPU_CPU_FREQ)));

	//-------------------------------------------------------------------------- pit
	/*
	0.[7:0]: cycles in sysclock 1193181 Hz
	*/

	IOWR(PIT_BASE, 0, (int)((1000000000.0/1193181.0) / (1000000000.0 / ALT_CPU_CPU_FREQ)));

	//-------------------------------------------------------------------------- floppy

	fdd_set(config.fdd_name);
	
	//-------------------------------------------------------------------------- hdd

	unsigned int hd_cylinders = 0;
	unsigned int hd_heads = 0;
	unsigned int hd_spt = 0;
	unsigned int hd_total_sectors = 0;
	unsigned int hdd_sd_base = 0;

	int hdd = img_mount(IMG_TYPE_HDD, config.hdd_name);
	if (hdd)
	{
		hd_cylinders = 1024;
		hd_heads = 16;
		hd_spt = 63;

		hd_total_sectors = get_image(IMG_TYPE_HDD)->size / 512;
	}

	/*
	0x00.[31:0]:    identify write
	0x01.[16:0]:    media cylinders
	0x02.[4:0]:     media heads
	0x03.[8:0]:     media spt
	0x04.[13:0]:    media sectors per cylinder = spt * heads
	0x05.[31:0]:    media sectors total
	0x06.[31:0]:    media sd base
	*/

	uint32_t identify[256] = 
	{
		0x0040, 										//word 0
		(hd_cylinders > 16383)? 16383 : hd_cylinders, 	//word 1
		0x0000,											//word 2 reserved
		hd_heads,										//word 3
		(uint16_t)(512 * hd_spt),					    //word 4
		512,											//word 5
		hd_spt,											//word 6
		0x0000,											//word 7 vendor specific
		0x0000,											//word 8 vendor specific
		0x0000,											//word 9 vendor specific
		('A' << 8) | 'O',								//word 10
		('H' << 8) | 'D',								//word 11
		('0' << 8) | '0',								//word 12
		('0' << 8) | '0',								//word 13
		('0' << 8) | ' ',								//word 14
		(' ' << 8) | ' ',								//word 15
		(' ' << 8) | ' ',								//word 16
		(' ' << 8) | ' ',								//word 17
		(' ' << 8) | ' ',								//word 18
		(' ' << 8) | ' ',								//word 19
		3,   											//word 20 buffer type
		512,											//word 21 cache size
		4,												//word 22 number of ecc bytes
		0,0,0,0,										//words 23..26 firmware revision
		('A' << 8) | 'O',								//words 27..46 model number
		(' ' << 8) | 'H',
		('a' << 8) | 'r',
		('d' << 8) | 'd',
		('r' << 8) | 'i',
		('v' << 8) | 'e',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		(' ' << 8) | ' ',
		16,												//word 47 max multiple sectors
		1,												//word 48 dword io
		1<<9,											//word 49 lba supported
		0x0000,											//word 50 reserved
		0x0200,											//word 51 pio timing
		0x0200,											//word 52 pio timing
		0x0007,											//word 53 valid fields
		(hd_cylinders > 16383)? 16383 : hd_cylinders, 	//word 54
		hd_heads,										//word 55
		hd_spt,											//word 56
		hd_total_sectors & 0xFFFF,						//word 57
		hd_total_sectors >> 16,							//word 58
		0x0000,											//word 59 multiple sectors
		hd_total_sectors & 0xFFFF,						//word 60
		hd_total_sectors >> 16,							//word 61
		0x0000,											//word 62 single word dma modes
		0x0000,											//word 63 multiple word dma modes
		0x0000,											//word 64 pio modes
		120,120,120,120,								//word 65..68
		0,0,0,0,0,0,0,0,0,0,0,							//word 69..79
		0x007E,											//word 80 ata modes
		0x0000,											//word 81 minor version number
		1<<14,  										//word 82 supported commands
		(1<<14) | (1<<13) | (1<<12) | (1<<10),			//word 83
		1<<14,	    									//word 84
		1<<14,	 	    								//word 85
		(1<<14) | (1<<13) | (1<<12) | (1<<10),			//word 86
		1<<14,	    									//word 87
		0x0000,											//word 88
		0,0,0,0,										//word 89..92
		1 | (1<<14) | 0x2000,							//word 93
		0,0,0,0,0,0,									//word 94..99
		hd_total_sectors & 0xFFFF,						//word 100
		hd_total_sectors >> 16,							//word 101
		0,												//word 102
		0,												//word 103

		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,//word 104..127

		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,				//word 128..255
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
	};

	for(int i=0; i<128; i++) IOWR(HDD_BASE, 0, hdd ? ((unsigned int)identify[2*i+1] << 16) | (unsigned int)identify[2*i+0] : 0);

	IOWR(HDD_BASE, 1, hd_cylinders);
	IOWR(HDD_BASE, 2, hd_heads);
	IOWR(HDD_BASE, 3, hd_spt);
	IOWR(HDD_BASE, 4, hd_spt * hd_heads);
	IOWR(HDD_BASE, 5, hd_spt * hd_heads * hd_cylinders);
	IOWR(HDD_BASE, 6, 0); // base LBA

	printf("HDD:\n  hd_cylinders %d\n  hd_heads %d\n  hd_spt %d\n  hd_total_sectors %d\n\n", hd_cylinders, hd_heads, hd_spt, hd_total_sectors);

	//-------------------------------------------------------------------------- rtc

	/*
    128.[26:0]: cycles in second
    129.[12:0]: cycles in 122.07031 us
    */

	IOWR(RTC_BASE, 128, (int)(1000000000.0 / (1000000000.0 / ALT_CPU_CPU_FREQ)));
	IOWR(RTC_BASE, 129, (int)(122070.0 / (1000000000.0 / ALT_CPU_CPU_FREQ)));

	bool translate_none = hd_cylinders <= 1024 && hd_heads <= 16 && hd_spt <= 63;
	bool translate_large= !translate_none && (hd_cylinders * hd_heads) <= 131072;
	bool translate_lba  = !translate_none && !translate_large;

	unsigned char translate_byte = 1; //(translate_large) ? 1 : (translate_lba) ? 2 : 0;

	//rtc contents 0-127
	uint32_t tmp[128] = {
		0x00, //0x00: SEC BCD
		0x00, //0x01: ALARM SEC BCD
		0x00, //0x02: MIN BCD
		0x00, //0x03: ALARM MIN BCD
		0x12, //0x04: HOUR BCD 24h
		0x12, //0x05: ALARM HOUR BCD 24h
		0x01, //0x06: DAY OF WEEK Sunday=1
		0x03, //0x07: DAY OF MONTH BCD from 1
		0x11, //0x08: MONTH BCD from 1
		0x13, //0x09: YEAR BCD
		0x26, //0x0A: REG A
		0x02, //0x0B: REG B
		0x00, //0x0C: REG C
		0x80, //0x0D: REG D
		0x00, //0x0E: REG E - POST status
		0x00, //0x0F: REG F - shutdown status

		CMOS_FDD_TYPE, //0x10: floppy drive type; 0-none, 1-360K, 2-1.2M, 3-720K, 4-1.44M, 5-2.88M
		0x00, //0x11: configuration bits; not used
		0xF0, //0x12: hard disk types; 0-none, 1:E-type, F-type 16+
		0x00, //0x13: advanced configuration bits; not used
		0x0D, //0x14: equipment bits
		0x80, //0x15: base memory in 1k LSB
		0x02, //0x16: base memory in 1k MSB
		0x00, //0x17: memory size above 1m in 1k LSB
		0xFC, //0x18: memory size above 1m in 1k MSB
		0x2F, //0x19: extended hd types 1/2; type 47d
		0x00, //0x1A: extended hd types 2/2

		hdd ? hd_cylinders & 0xFF : 0, 		//0x1B: hd 0 configuration 1/9; cylinders low
		hdd ? (hd_cylinders >> 8) & 0xFF : 0, //0x1C: hd 0 configuration 2/9; cylinders high
		hdd ? hd_heads : 0, 					//0x1D: hd 0 configuration 3/9; heads
		hdd ? 0xFF : 0, 						//0x1E: hd 0 configuration 4/9; write pre-comp low
		hdd ? 0xFF : 0, 						//0x1F: hd 0 configuration 5/9; write pre-comp high
		hdd ? 0xC8 : 0, 						//0x20: hd 0 configuration 6/9; retries/bad map/heads>8
		hdd ? hd_cylinders & 0xFF : 0, 		//0x21: hd 0 configuration 7/9; landing zone low
		hdd ? (hd_cylinders >> 8) & 0xFF : 0, //0x22: hd 0 configuration 8/9; landing zone high
		hdd ? hd_spt : 0, 					//0x23: hd 0 configuration 9/9; sectors/track

		0x00, //0x24: hd 1 configuration 1/9
		0x00, //0x25: hd 1 configuration 2/9
		0x00, //0x26: hd 1 configuration 3/9
		0x00, //0x27: hd 1 configuration 4/9
		0x00, //0x28: hd 1 configuration 5/9
		0x00, //0x29: hd 1 configuration 6/9
		0x00, //0x2A: hd 1 configuration 7/9
		0x00, //0x2B: hd 1 configuration 8/9
		0x00, //0x2C: hd 1 configuration 9/9

		(boot_from_floppy)? 0x20u : 0x00u, //0x2D: boot sequence

		0x00, //0x2E: checksum MSB
		0x00, //0x2F: checksum LSB

		0x00, //0x30: memory size above 1m in 1k LSB
		0xFC, //0x31: memory size above 1m in 1k MSB

		0x20, //0x32: IBM century
		0x00, //0x33: ?

		0x00, //0x34: memory size above 16m in 64k LSB
		0x07, //0x35: memory size above 16m in 64k MSB; 128 MB

		0x00, //0x36: ?
		0x20, //0x37: IBM PS/2 century

		0x00, 			//0x38: eltorito boot sequence; not used
		translate_byte, //0x39: ata translation policy 1/2
		0x00, 			//0x3A: ata translation policy 2/2

		0x00, //0x3B: ?
		0x00, //0x3C: ?

		0x00, //0x3D: eltorito boot sequence; not used

		0x00, //0x3E: ?
		0x00, //0x3F: ?

		0, 0, 0, 0, 0, 0, 0, 0,	0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0,	0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0,	0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0,	0, 0, 0, 0, 0, 0, 0, 0
	};

	memcpy(cmos, tmp, sizeof(cmos));

	//count checksum
	unsigned short sum = 0;
	for(int i=0x10; i<=0x2D; i++) sum += cmos[i];

	cmos[0x2E] = sum >> 8;
	cmos[0x2F] = sum & 0xFF;

	for(unsigned int i=0; i<sizeof(cmos)/sizeof(unsigned int); i++) IOWR(RTC_BASE, i, cmos[i]);

	user_io_8bit_set_status(0, UIO_STATUS_RESET);
}

struct sd_param_t
{
	uint32_t addr;
	uint32_t lba;
	uint32_t bl_cnt;
};

static struct sd_param_t sd_params = { 0 };

void x86_poll()
{
	int res = 0;
	static uint32_t secbuf[128 * 4];

	char sd_req = dma_sdio(0);
	if (sd_req == 1)
	{
		dma_rcvbuf(SD_BASE + (4 << 2), sizeof(sd_params) >> 2, (uint32_t*)&sd_params);
		//printf("Read: 0x%08x, 0x%08x, %d\n", sd_params.addr, sd_params.lba, sd_params.bl_cnt);

		if (get_image(sd_params.addr)->size)
		{
			if (sd_params.bl_cnt>0 && sd_params.bl_cnt<=4)
			{
				if (img_read(sd_params.addr, sd_params.lba, secbuf, sd_params.bl_cnt * 512))
				{
					dma_sendbuf(sd_params.addr, sd_params.bl_cnt * 128, secbuf);
					res = 1;
				}
			}
			else
			{
				printf("Error: Block count %d is out of range 1..4.\n", sd_params.bl_cnt);
			}
		}
		else
		{
			printf("Error: image is not ready.\n");
		}

		dma_sdio(res ? 1 : 2);
	}
	else if (sd_req == 2)
	{
		dma_rcvbuf(SD_BASE + (4 << 2), sizeof(sd_params) >> 2, (uint32_t*)&sd_params);
		//printf("Write: 0x%08x, 0x%08x, %d\n", sd_params.addr, sd_params.lba, sd_params.bl_cnt);

		if (get_image(sd_params.addr)->size)
		{
			if (sd_params.bl_cnt>0 && sd_params.bl_cnt <= 4)
			{
				if (get_image(sd_params.addr)->mode & O_RDWR)
				{
					dma_rcvbuf(sd_params.addr, sd_params.bl_cnt * 128, secbuf);
					if (img_write(sd_params.addr, sd_params.lba, secbuf, sd_params.bl_cnt * 512))
					{
						res = 1;
					}
				}
				else
				{
					printf("Error: image is read-only.\n");
				}
			}
			else
			{
				printf("Error: Block count %d is out of range 1..4.\n", sd_params.bl_cnt);
			}
		}
		else
		{
			printf("Error: image is not ready.\n");
		}

		dma_sdio(res ? 1 : 2);
	}
}

void x86_set_image(int num, char *filename)
{
	if (num == 2)
	{
		strcpy(config.hdd_name, filename);
	}

	if (num == 0)
	{
		strcpy(config.fdd_name, filename);
		fdd_set(filename);
	}
}

void x86_config_save()
{
	config.ver = CFG_VER;
	FileSaveConfig("ao486sys.cfg", &config, sizeof(config));
}

void x86_config_load()
{
	static x86_config tmp;
	memset(&config, 0, sizeof(config));
	if (FileLoadConfig("ao486sys.cfg", &tmp, sizeof(tmp)) && (tmp.ver == CFG_VER))
	{
		memcpy(&config, &tmp, sizeof(config));
	}
}

void x86_set_fdd_boot(uint32_t boot)
{
	boot_from_floppy = (boot != 0);
}
