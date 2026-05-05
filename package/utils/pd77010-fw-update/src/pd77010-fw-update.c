// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PD77010 PoE Controller Firmware Update Utility
 *
 * Implements the firmware download and replace procedure described in
 * Microchip DS50003836A (PD77020/PD77010 Firmware Download and Replace
 * Procedure) and DS50003874C (BT Serial Communication Protocol).
 *
 * Communication is over I2C using the Linux kernel i2c-dev interface.
 * All messages are 15 bytes: 13 bytes of payload + 2 bytes of checksum.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

/* Protocol constants */
#define MSG_LEN			15	/* total bytes per message */
#define MSG_PAYLOAD_LEN		13	/* bytes covered by checksum */
#define NVM_DATA_PER_CMD	8	/* firmware bytes per program command */
#define NVM_PAGE_SIZE		64	/* bytes per NVM page (8 cmds × 8 bytes) */
#define HEX_LINE_DATA_LEN	32	/* bytes per :20 HEX line */

/* Message KEY field values (byte [0]) */
#define KEY_COMMAND		0x00
#define KEY_PROGRAM		0x01
#define KEY_REQUEST		0x02
#define KEY_TELEMETRY		0x03
#define KEY_REPORT		0x52

/* Unused "N" field value - must be sent in all unused DATA positions */
#define FIELD_N			0x4E

/* CPU Status1 field values (response byte [2]) */
#define CPU_STATUS_APP		0x00	/* Application mode, PoE functional */
#define CPU_STATUS_BOOT		0x01	/* BOOT mode, PoE not functional */

/* Boot Err Code values (response byte [5]) */
#define ERR_NO_ERROR		0x00
#define ERR_BOOT_DURING_ERASE	0x05	/* NVM erase in progress */
#define ERR_BOOT_DURING_DL	0x06	/* Erase done, ready for download */
#define ERR_NVM_ERASE_ERROR	0x07
#define ERR_NVM_PROG_ERROR	0x08

/* Timing (milliseconds) per DS50003874C Table 1-2 */
#define T_READ_BACK_MS		12	/* min wait before reading response */
#define T_BETWEEN_CMDS_MS	6	/* min wait between commands */
#define T_ERASE_WAIT_MS		4000	/* wait after issuing NVM erase */
#define T_PAGE_WAIT_MS		2	/* wait after every 8 program commands */
#define T_RESET_WAIT_MS		50	/* wait after device reset */
#define T_FINAL_WAIT_MS		30	/* wait after final Download Restart */

#define ERASE_POLL_RETRIES	20
#define MODE_CHECK_RETRIES	10

static int	i2c_fd = -1;
static uint8_t	i2c_addr;
static uint8_t	echo = 0x01;

static void msleep(unsigned int ms)
{
	struct timespec ts = {
		.tv_sec  = ms / 1000,
		.tv_nsec = (ms % 1000) * 1000000L,
	};
	nanosleep(&ts, NULL);
}

/* ECHO must not repeat on consecutive messages (0x00-0xFE range) */
static uint8_t next_echo(void)
{
	uint8_t e = echo;

	echo = (echo >= 0xFE) ? 0x00 : echo + 1;
	return e;
}

/* 16-bit arithmetic sum of first 13 bytes, split into bytes [13] (MSB) and [14] (LSB) */
static void fill_checksum(uint8_t *msg)
{
	uint16_t sum = 0;

	for (int i = 0; i < MSG_PAYLOAD_LEN; i++)
		sum += msg[i];

	msg[13] = (sum >> 8) & 0xFF;
	msg[14] = sum & 0xFF;
}

static int i2c_write_msg(const uint8_t *buf)
{
	if (write(i2c_fd, buf, MSG_LEN) != MSG_LEN) {
		perror("I2C write");
		return -1;
	}
	return 0;
}

static int i2c_read_msg(uint8_t *buf)
{
	/* Minimum read-back time after a write: 12 ms */
	msleep(T_READ_BACK_MS);

	if (read(i2c_fd, buf, MSG_LEN) != MSG_LEN) {
		perror("I2C read");
		return -1;
	}
	return 0;
}

/*
 * Send a fully built message (sets checksum then writes).
 * Caller fills [0]..[12]; this function fills [13]..[14] and sends.
 */
static int send_msg(uint8_t *msg)
{
	fill_checksum(msg);
	return i2c_write_msg(msg);
}

/*
 * Get BT System Status (DS50003874C §3.1.6 / §4.1.4).
 * Identical request format in Application and Boot mode.
 *
 * Response bytes of interest:
 *   [2] CPU_Status1: 0x00 = Application, 0x01 = Boot
 *   [5] Err_Code (Application mode constant 0x00; Boot: see §4.1.4 table)
 *
 * In Boot mode the device returns the Boot telemetry layout where
 * Err_Code is at [5] in the automatic reset telemetry (ECHO=0xFF, §4.1.5.1)
 * and the explicit Boot Get BT System Status response (§4.1.4).
 */
static int cmd_get_system_status(uint8_t *resp)
{
	uint8_t msg[MSG_LEN] = {
		KEY_REQUEST, next_echo(), 0x07, 0xD0, FIELD_N,
		FIELD_N, FIELD_N, FIELD_N, FIELD_N,
		FIELD_N, FIELD_N, FIELD_N, FIELD_N,
		0x00, 0x00
	};

	if (send_msg(msg) < 0)
		return -1;

	return i2c_read_msg(resp);
}

/*
 * Download Restart / Download Abort (DS50003874C §4.1.1).
 * Used to: (a) switch from Application to BOOT mode before download,
 *           (b) finalize download and return to Application mode.
 * The device resets after this command; caller must read 15 bytes to
 * drain the I2C TX buffer before sending any further commands.
 */
static int cmd_download_restart(void)
{
	uint8_t msg[MSG_LEN] = {
		KEY_PROGRAM, next_echo(), 0xFF, 0x99, 0x15,
		0x16, 0x16, 0x99, FIELD_N,
		FIELD_N, FIELD_N, FIELD_N, FIELD_N,
		0x00, 0x00
	};

	return send_msg(msg);
}

/*
 * NVM Download Erase (DS50003874C §4.1.2).
 * Erases the entire application area. Takes up to 4 seconds.
 */
static int cmd_nvm_erase(void)
{
	uint8_t msg[MSG_LEN] = {
		KEY_PROGRAM, next_echo(), 0xFF, 0x90, 0x02,
		FIELD_N, FIELD_N, FIELD_N, FIELD_N,
		FIELD_N, FIELD_N, FIELD_N, FIELD_N,
		0x00, 0x00
	};

	return send_msg(msg);
}

/*
 * NVM Download and Program Page DATA (DS50003874C §4.1.3).
 * Sends exactly 8 bytes of firmware data. The internal buffer accumulates
 * 64 bytes (8 calls) then programs one NVM page automatically.
 */
static int cmd_nvm_program(const uint8_t *data8)
{
	uint8_t msg[MSG_LEN] = {
		KEY_PROGRAM, next_echo(), 0xFF, 0x92, 0x92,
		data8[0], data8[1], data8[2], data8[3],
		data8[4], data8[5], data8[6], data8[7],
		0x00, 0x00
	};

	return send_msg(msg);
}

/* Drain the device's I2C TX buffer (required after reset commands) */
static void drain_i2c_buffer(void)
{
	uint8_t buf[MSG_LEN];

	i2c_read_msg(buf);
}

static int check_response_key(const uint8_t *resp, const char *context)
{
	if (resp[0] != KEY_TELEMETRY && resp[0] != KEY_REPORT) {
		fprintf(stderr, "%s: unexpected KEY byte 0x%02X\n",
			context, resp[0]);
		return -1;
	}
	return 0;
}

/*
 * Ensure the device is in BOOT mode.
 * If in Application mode, send Download Restart to trigger the switch.
 */
static int ensure_boot_mode(void)
{
	uint8_t resp[MSG_LEN];
	uint8_t cpu_status;

	if (cmd_get_system_status(resp) < 0)
		return -1;

	if (check_response_key(resp, "get_system_status") < 0)
		return -1;

	cpu_status = resp[2];

	if (cpu_status == CPU_STATUS_BOOT) {
		printf("Device already in BOOT mode\n");
		return 0;
	}

	printf("Device in Application mode, switching to BOOT mode...\n");

	if (cmd_download_restart() < 0)
		return -1;

	/* Device resets; drain the telemetry placed in the I2C TX buffer */
	msleep(T_RESET_WAIT_MS);
	drain_i2c_buffer();

	/* Verify BOOT mode */
	for (int i = 0; i < MODE_CHECK_RETRIES; i++) {
		msleep(T_RESET_WAIT_MS);

		if (cmd_get_system_status(resp) < 0)
			continue;

		if (resp[0] != KEY_TELEMETRY)
			continue;

		cpu_status = resp[2];

		if (cpu_status == CPU_STATUS_BOOT) {
			printf("Switched to BOOT mode\n");
			return 0;
		}
	}

	fprintf(stderr, "Failed to enter BOOT mode (CPU Status1=0x%02X)\n",
		cpu_status);
	return -1;
}

static int erase_nvm(void)
{
	uint8_t resp[MSG_LEN];
	uint8_t err_code;

	printf("Erasing NVM application area...\n");

	if (cmd_nvm_erase() < 0)
		return -1;

	printf("Waiting %d ms for erase to complete...\n", T_ERASE_WAIT_MS);
	msleep(T_ERASE_WAIT_MS);

	/*
	 * Poll Boot Get BT System Status until Err Code transitions from
	 * 0x05 (Boot During Erase) to 0x06 (Boot During Download).
	 * Err Code is at byte [5] in the Boot mode response.
	 */
	for (int i = 0; i < ERASE_POLL_RETRIES; i++) {
		if (cmd_get_system_status(resp) < 0) {
			msleep(500);
			continue;
		}

		if (resp[0] != KEY_TELEMETRY) {
			msleep(500);
			continue;
		}

		err_code = resp[5];

		if (err_code == ERR_BOOT_DURING_DL) {
			printf("NVM erase complete\n");
			return 0;
		}

		if (err_code == ERR_BOOT_DURING_ERASE) {
			printf("  Erase in progress (poll %d/%d)...\n",
			       i + 1, ERASE_POLL_RETRIES);
			msleep(500);
			continue;
		}

		if (err_code == ERR_NVM_ERASE_ERROR) {
			fprintf(stderr, "NVM erase error reported by device\n");
			return -1;
		}

		fprintf(stderr, "Unexpected Err Code during erase: 0x%02X\n",
			err_code);
		msleep(500);
	}

	fprintf(stderr, "NVM erase timed out\n");
	return -1;
}

/*
 * Parse one Intel HEX record.
 *
 * Returns:
 *   1  - data record with 32 bytes (`:20`, type 00), data written to buf
 *   0  - EOF record (`:00000001FF`)
 *  -1  - skip (extended address, start, wrong byte count, or parse error)
 */
static int parse_hex_line(const char *line, uint8_t *buf)
{
	unsigned int byte_count, addr, rec_type;
	uint8_t calc = 0;

	if (line[0] != ':')
		return -1;

	if (sscanf(line + 1, "%02x%04x%02x", &byte_count, &addr, &rec_type) != 3)
		return -1;

	if (rec_type == 0x01)
		return 0;	/* EOF record */

	/* Only process 32-byte data records */
	if (rec_type != 0x00 || byte_count != HEX_LINE_DATA_LEN)
		return -1;

	for (unsigned int i = 0; i < byte_count; i++) {
		unsigned int b;

		if (sscanf(line + 9 + i * 2, "%02x", &b) != 1)
			return -1;
		buf[i] = (uint8_t)b;
	}

	/* Verify Intel HEX record checksum */
	calc += (uint8_t)byte_count;
	calc += (uint8_t)(addr >> 8);
	calc += (uint8_t)(addr & 0xFF);
	calc += (uint8_t)rec_type;
	for (unsigned int i = 0; i < byte_count; i++)
		calc += buf[i];
	calc = (~calc + 1) & 0xFF;

	unsigned int file_cs;

	if (sscanf(line + 9 + byte_count * 2, "%02x", &file_cs) != 1)
		return -1;

	if (calc != (uint8_t)file_cs) {
		fprintf(stderr, "HEX checksum error at address 0x%04X\n", addr);
		return -1;
	}

	return 1;
}

static int program_firmware(const char *hex_path)
{
	FILE *f;
	char line[512];
	uint8_t data[HEX_LINE_DATA_LEN];
	int cmd_count = 0;
	int total_bytes = 0;
	int rc;
	bool eof_found = false;

	f = fopen(hex_path, "r");
	if (!f) {
		fprintf(stderr, "Cannot open %s: %s\n", hex_path, strerror(errno));
		return -1;
	}

	printf("Programming firmware...\n");

	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);

		/* Strip CR/LF */
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		rc = parse_hex_line(line, data);
		if (rc == 0) {
			eof_found = true;
			break;
		}
		if (rc < 0)
			continue;

		/*
		 * Each :20 line provides 32 bytes, sent as 4 × 8-byte program
		 * commands. After every 8 commands (64 bytes = 1 NVM page),
		 * wait 2 ms for the page programming to complete.
		 */
		for (int offset = 0; offset < HEX_LINE_DATA_LEN;
		     offset += NVM_DATA_PER_CMD) {
			if (cmd_nvm_program(data + offset) < 0) {
				fclose(f);
				fprintf(stderr, "NVM program command failed\n");
				return -1;
			}

			cmd_count++;
			total_bytes += NVM_DATA_PER_CMD;

			if (cmd_count % 8 == 0) {
				msleep(T_PAGE_WAIT_MS);

				if ((cmd_count % 160) == 0)
					printf("  %d bytes written...\n",
					       total_bytes);
			}
		}
	}

	fclose(f);

	if (!eof_found) {
		fprintf(stderr, "EOF record not found in HEX file\n");
		return -1;
	}

	printf("Firmware data sent: %d bytes (%d NVM pages)\n",
	       total_bytes, cmd_count / 8);
	return 0;
}

static int finalize_and_verify(void)
{
	uint8_t resp[MSG_LEN];
	uint8_t cpu_status;

	printf("Finalizing download (sending Download Restart)...\n");

	if (cmd_download_restart() < 0)
		return -1;

	/*
	 * Device resets; wait 30 ms then drain the boot/application telemetry
	 * from the I2C TX buffer before issuing the verification request.
	 */
	msleep(T_FINAL_WAIT_MS);
	drain_i2c_buffer();

	/* Verify the device came up in Application mode */
	printf("Verifying Application mode...\n");

	for (int i = 0; i < MODE_CHECK_RETRIES; i++) {
		msleep(T_RESET_WAIT_MS);

		if (cmd_get_system_status(resp) < 0)
			continue;

		if (resp[0] != KEY_TELEMETRY)
			continue;

		cpu_status = resp[2];

		if (cpu_status == CPU_STATUS_APP) {
			printf("Firmware update successful - device in Application mode\n");
			return 0;
		}

		if (cpu_status == CPU_STATUS_BOOT) {
			fprintf(stderr,
				"Device still in BOOT mode after update "
				"(Err Code=0x%02X, Err Info1=0x%02X, "
				"Err Info2=0x%02X, DL Type=0x%02X)\n",
				resp[5], resp[6], resp[7], resp[8]);
			return -1;
		}
	}

	fprintf(stderr, "Timeout waiting for Application mode\n");
	return -1;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s -b <bus> -a <addr> -f <firmware.hex>\n"
		"\n"
		"  -b <bus>   I2C bus number (e.g. 0 for /dev/i2c-0)\n"
		"  -a <addr>  I2C device address in hex (e.g. 0x20)\n"
		"  -f <file>  Microchip firmware HEX file\n"
		"\n"
		"Example: %s -b 0 -a 0x20 -f PD77010_fw.hex\n",
		prog, prog);
}

int main(int argc, char *argv[])
{
	int bus = -1;
	const char *hex_path = NULL;
	char dev_path[32];
	int opt;
	int ret = EXIT_FAILURE;

	i2c_addr = 0;

	while ((opt = getopt(argc, argv, "b:a:f:")) != -1) {
		switch (opt) {
		case 'b':
			bus = atoi(optarg);
			break;
		case 'a':
			i2c_addr = (uint8_t)strtoul(optarg, NULL, 0);
			break;
		case 'f':
			hex_path = optarg;
			break;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (bus < 0 || i2c_addr == 0 || !hex_path) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	snprintf(dev_path, sizeof(dev_path), "/dev/i2c-%d", bus);

	i2c_fd = open(dev_path, O_RDWR);
	if (i2c_fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", dev_path,
			strerror(errno));
		return EXIT_FAILURE;
	}

	if (ioctl(i2c_fd, I2C_SLAVE, i2c_addr) < 0) {
		fprintf(stderr, "Cannot set I2C slave address 0x%02X: %s\n",
			i2c_addr, strerror(errno));
		goto out;
	}

	printf("PD77010 Firmware Updater\n");
	printf("Device:   %s @ 0x%02X\n", dev_path, i2c_addr);
	printf("Firmware: %s\n\n", hex_path);

	if (ensure_boot_mode() < 0)
		goto out;

	if (erase_nvm() < 0)
		goto out;

	if (program_firmware(hex_path) < 0)
		goto out;

	if (finalize_and_verify() < 0)
		goto out;

	ret = EXIT_SUCCESS;

out:
	close(i2c_fd);
	return ret;
}
