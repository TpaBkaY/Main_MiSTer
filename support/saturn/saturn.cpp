#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../../spi.h"
#include "../../hardware.h"
#include "../../menu.h"
#include "../../cheats.h"
#include "saturn.h"

static int need_reset = 0;
uint32_t frame_cnt = 0;
uint8_t time_mode;

static uint32_t CalcTimerOffset(uint8_t speed) {
	static uint8_t adj0 = 0x1, adj1 = 0x1, adj2 = 0x1;

	uint32_t offs;
	if (speed == 2) {
		offs = 6 + ((adj2 & 0x03) != 0x00 ? 1 : 0);	//6.6
		adj2 <<= 1;
		if (adj2 >= 0x08) adj2 = 0x01;
		adj0 = adj1 = 0x1;
	}
	else if (speed == 1) {
		offs = 13 + ((adj1 & 0x01) != 0x00 ? 1 : 0);	//13.3
		adj1 <<= 1;
		if (adj1 >= 0x08) adj1 = 0x01;
		adj0 = adj2 = 0x1;
	}
	else {
		offs = 16 + ((adj0 & 0x03) != 0x00 ? 1 : 0);	//16.7
		adj0 <<= 1;
		if (adj0 >= 0x08) adj0 = 0x01;
		adj1 = adj2 = 0x1;
	}
	return offs;
}

void saturn_poll()
{
	static unsigned long poll_timer = 0;
	static uint8_t last_req = 255;

	if (!poll_timer || CheckTimer(poll_timer))
	{
		poll_timer = GetTimer(0);

		uint16_t data_in[6];
		uint8_t req = spi_uio_cmd_cont(UIO_CD_GET);
		if (req != last_req)
		{
			last_req = req;

			for (int i = 0; i < 6; i++) data_in[i] = spi_w(0);
			DisableIO();

			satcdd.SetCommand((uint8_t*)data_in);
			satcdd.CommandExec();
		}
		else
			DisableIO();

		satcdd.Process(&time_mode);
		poll_timer += CalcTimerOffset(time_mode);

		uint16_t* s = (uint16_t*)satcdd.GetStatus();
		spi_uio_cmd_cont(UIO_CD_SET);
		for (int i = 0; i < 6; i++) spi_w(s[i]);
		DisableIO();

		satcdd.Update();
		frame_cnt++;

#ifdef SATURN_DEBUG
		unsigned long curr_timer = GetTimer(0);
		if (curr_timer >= poll_timer) {
			printf("\x1b[32mSaturn: ");
			printf("Time over: next = %lu, curr = %lu", poll_timer, curr_timer);
			printf("\n\x1b[0m");
		}
#endif // SATURN_DEBUG
	}
}

static char buf[1024];

static void saturn_get_save_without_disk(char *buf)
{
	char *p1, *p2;

	if ((p1 = strstr(buf, "disc")) != 0 || (p1 = strstr(buf, "Disc")) != 0 || (p1 = strstr(buf, "DISC")) != 0)
	{
		p2 = p1 + 4;

		if (p1 > buf && *(--p1) == '(') p1--;
		if (p1 > buf && *(--p1) == ' ') p1--;
		if (*p2 == ' ') p2++;
		if ((*p2 >= '0' && *p2 <= '9') || (*p2 >= 'a' && *p2 <= 'f') || (*p2 >= 'A' && *p2 <= 'F')) {
			p2++;
			if (*p2 == ')') p2++;
			p1++;
			strcpy(p1, p2);
		}
	}
}

static void saturn_mount_save(const char *filename)
{
	user_io_set_index(SAVE_IO_INDEX);
	user_io_set_download(1);
	if (strlen(filename))
	{
		FileGenerateSavePath(filename, buf);
		saturn_get_save_without_disk(buf);
#ifdef SATURN_DEBUG
		printf("Saturn save filename = %s\n", buf);
#endif // SATURN_DEBUG
		user_io_file_mount(buf, 0, 1);
	}
	else
	{
		user_io_file_mount("");
	}
	user_io_set_download(0);
}

static int saturn_load_rom(const char *basename, const char *name, int sub_index)
{
	strcpy(buf, basename);
	char *p = strrchr(buf, '/');
	if (p)
	{
		p++;
		strcpy(p, name);
		if (user_io_file_tx(buf, sub_index << 6)) return 1;
	}

	return 0;
}

void saturn_set_image(int num, const char *filename)
{
	static char last_dir[1024] = {};

	(void)num;

	satcdd.Unload();
	satcdd.Reset();

	int same_game = *filename && *last_dir && !strncmp(last_dir, filename, strlen(last_dir));
	strcpy(last_dir, filename);
	char *p = strrchr(last_dir, '/');
	if (p) *p = 0;

	if (!same_game)
	{
		saturn_mount_save("");

		user_io_status_set("[0]", 1);
		saturn_reset();

		// load CD BIOS
		if (!saturn_load_rom(filename, "cd_bios.rom", 0)) // from disk folder.
		{
			if (!saturn_load_rom(last_dir, "cd_bios.rom", 0)) // from parent folder.
			{
				sprintf(buf, "%s/boot.rom", HomeDir()); // from home folder.
				if (!user_io_file_tx(buf))
				{
					Info("CD BIOS not found!", 4000);
				}
			}
		}
	}

	if (strlen(filename))
	{
		if (satcdd.Load(filename) > 0)
		{
			satcdd.SendData = saturn_send_data;

			if (!same_game)
			{
				//saturn_load_rom(filename, "cart.rom", 1);
				saturn_mount_save(filename);
				//cheats_init(filename, 0);
			}

			if (satcdd.GetBootHeader((uint8_t*)buf) > 0)
			{
				saturn_send_data((uint8_t*)buf, 256, BOOT_IO_INDEX);
			}
		}
	}

	user_io_status_set("[0]", 0);
}

void saturn_reset() {
	need_reset = 1;
}

int saturn_send_data(uint8_t* buf, int len, uint8_t index) {
	// set index byte
	user_io_set_index(index);

	user_io_set_download(1);
	user_io_file_tx_data(buf, len);
	user_io_set_download(0);
	return 1;
}

static char save_blank[] = {
	0x00, 0x42, 0x00, 0x61, 0x00, 0x63, 0x00, 0x6B, 0x00, 0x55, 0x00, 0x70, 0x00, 0x52, 0x00, 0x61,
	0x00, 0x6D, 0x00, 0x20, 0x00, 0x46, 0x00, 0x6F, 0x00, 0x72, 0x00, 0x6D, 0x00, 0x61, 0x00, 0x74,
	0x00, 0x42, 0x00, 0x61, 0x00, 0x63, 0x00, 0x6B, 0x00, 0x55, 0x00, 0x70, 0x00, 0x52, 0x00, 0x61,
	0x00, 0x6D, 0x00, 0x20, 0x00, 0x46, 0x00, 0x6F, 0x00, 0x72, 0x00, 0x6D, 0x00, 0x61, 0x00, 0x74,
	0x00, 0x42, 0x00, 0x61, 0x00, 0x63, 0x00, 0x6B, 0x00, 0x55, 0x00, 0x70, 0x00, 0x52, 0x00, 0x61,
	0x00, 0x6D, 0x00, 0x20, 0x00, 0x46, 0x00, 0x6F, 0x00, 0x72, 0x00, 0x6D, 0x00, 0x61, 0x00, 0x74,
	0x00, 0x42, 0x00, 0x61, 0x00, 0x63, 0x00, 0x6B, 0x00, 0x55, 0x00, 0x70, 0x00, 0x52, 0x00, 0x61,
	0x00, 0x6D, 0x00, 0x20, 0x00, 0x46, 0x00, 0x6F, 0x00, 0x72, 0x00, 0x6D, 0x00, 0x61, 0x00, 0x74,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

void saturn_fill_blanksave(uint8_t *buffer, uint32_t lba)
{
	if (lba == 0 || lba == 128)
	{
		memcpy(buffer, save_blank, 512);
	}
	else
	{
		memset(buffer, 0, 512);
	}
}