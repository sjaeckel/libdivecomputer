/*
 * libdivecomputer
 *
 * Copyright (C) 2018 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free
#include <stdbool.h> // bool
#include <assert.h> // assert

#include "cressi_goa.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "array.h"
#include "platform.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &cressi_goa_device_vtable)

#define CMD_VERSION        0x00
#define CMD_SET_TIME       0x13
#define CMD_EXIT_PCLINK    0x1D
#define CMD_LOGBOOK        0x21
#define CMD_DIVE           0x22
#define CMD_LOGBOOK_V4     0x23

#define HEADER  0xAA
#define TRAILER 0x55
#define END     0x04
#define ACK     0x06

#define SZ_DATA   512
#define SZ_PACKET 12
#define SZ_HEADER 23

#define FP_OFFSET 0x11
#define FP_SIZE   6

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

typedef struct cressi_goa_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[FP_SIZE];
} cressi_goa_device_t;

static dc_status_t cressi_goa_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t cressi_goa_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t cressi_goa_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t cressi_goa_device_close (dc_device_t *abstract);
static const dc_device_vtable_t cressi_goa_device_vtable = {
	sizeof(cressi_goa_device_t),
	DC_FAMILY_CRESSI_GOA,
	cressi_goa_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	cressi_goa_device_foreach, /* foreach */
	cressi_goa_device_timesync, /* timesync */
	cressi_goa_device_close /* close */
};

static dc_status_t
cressi_goa_device_send (cressi_goa_device_t *device, unsigned char cmd, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (size > SZ_PACKET) {
		ERROR (abstract->context, "Unexpected payload size (%u).", size);
		return DC_STATUS_INVALIDARGS;
	}

	// Setup the data packet.
	unsigned short crc = 0;
	unsigned char packet[SZ_PACKET + 8] = {
		HEADER, HEADER, HEADER,
		size,
		cmd
	};
	if (size) {
		memcpy (packet + 5, data, size);
	}
	crc = checksum_crc16_ccitt (packet + 3, size + 2, 0x000, 0x0000);
	packet[5 + size + 0] = (crc     ) & 0xFF; // Low
	packet[5 + size + 1] = (crc >> 8) & 0xFF; // High
	packet[5 + size + 2] = TRAILER;

	// Wait a small amount of time before sending the command. Without
	// this delay, the transfer will fail most of the time.
	dc_iostream_sleep (device->iostream, 100);

	// Send the command to the device.
	status = dc_iostream_write (device->iostream, packet, size + 8, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return status;
}

static dc_status_t
cressi_goa_device_receive (cressi_goa_device_t *device, dc_buffer_t *output)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	unsigned char packet[SZ_PACKET + 8];

	// Clear the output buffer.
	dc_buffer_clear (output);

	// Read the header of the data packet.
	status = dc_iostream_read (device->iostream, packet, 4, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the header of the packet.
	if (packet[0] != HEADER || packet[1] != HEADER || packet[2] != HEADER) {
		ERROR (abstract->context, "Unexpected answer header byte.");
		return DC_STATUS_PROTOCOL;
	}

	// Get the payload length.
	unsigned int length = packet[3];
	if (length > SZ_PACKET) {
		ERROR (abstract->context, "Unexpected payload size (%u).", length);
		return DC_STATUS_PROTOCOL;
	}

	// Read the remainder of the data packet.
	status = dc_iostream_read (device->iostream, packet + 4, length + 4, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the trailer of the packet.
	if (packet[length + 7] != TRAILER) {
		ERROR (abstract->context, "Unexpected answer trailer byte.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the checksum of the packet.
	unsigned short crc = array_uint16_le (packet + length + 5);
	unsigned short ccrc = checksum_crc16_ccitt (packet + 3, length + 2, 0x0000, 0x0000);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	if (length && output) {
		if (!dc_buffer_append (output, packet + 5, length)) {
			ERROR (abstract->context, "Could not append received data.");
			return DC_STATUS_NOMEMORY;
		}
	}

	return status;
}

static dc_status_t
cressi_goa_device_download (cressi_goa_device_t *device, dc_buffer_t *buffer, dc_event_progress_t *progress)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	const unsigned char ack[] = {ACK};
	const unsigned int initial = progress ? progress->current : 0;

	// Erase the contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned int skip = 2;
	unsigned int size = 2;
	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Read the data packet.
		unsigned char packet[3 + SZ_DATA + 2];
		status = dc_iostream_read (device->iostream, packet, sizeof(packet), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		// Verify the checksum of the packet.
		unsigned short crc = array_uint16_le (packet + sizeof(packet) - 2);
		unsigned short ccrc = checksum_crc16_ccitt (packet + 3, sizeof(packet) - 5, 0x0000, 0x0000);
		if (crc != ccrc) {
			ERROR (abstract->context, "Unexpected answer checksum.");
			return DC_STATUS_PROTOCOL;
		}

		// Send the ack byte to the device.
		status = dc_iostream_write (device->iostream, ack, sizeof(ack), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the ack byte.");
			return status;
		}

		// Get the total size from the first data packet.
		if (nbytes == 0) {
			size += array_uint16_le (packet + 3);
		}

		// Calculate the payload size of the packet.
		unsigned int length = size - nbytes;
		if (length > SZ_DATA) {
			length = SZ_DATA;
		}

		// Append the payload to the output buffer.
		if (!dc_buffer_append (buffer, packet + 3 + skip, length - skip)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}

		nbytes += length;
		skip = 0;

		// Update and emit a progress event.
		if (progress) {
			progress->current = initial + STEP(nbytes, size);
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}
	}

	// Read the end byte.
	unsigned char end = 0;
	status = dc_iostream_read (device->iostream, &end, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the end byte.");
		return status;
	}

	// Verify the end byte.
	if (end != END) {
		ERROR (abstract->context, "Unexpected end byte (%02x).", end);
		return DC_STATUS_PROTOCOL;
	}

	// Send the ack byte to the device.
	status = dc_iostream_write (device->iostream, ack, sizeof(ack), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the ack byte.");
		return status;
	}

	return status;
}

static dc_status_t
cressi_goa_device_transfer (cressi_goa_device_t *device,
                            unsigned char cmd,
                            const unsigned char input[], unsigned int isize,
                            dc_buffer_t *output,
                            dc_buffer_t *buffer,
                            dc_event_progress_t *progress)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	// Send the command to the dive computer.
	status = cressi_goa_device_send (device, cmd, input, isize);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Receive the answer from the dive computer.
	status = cressi_goa_device_receive (device, output);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Download the optional and variable sized payload.
	if (buffer) {
		status = cressi_goa_device_download (device, buffer, progress);
		if (status != DC_STATUS_SUCCESS) {
			return status;
		}
	}

	return status;
}


dc_status_t
cressi_goa_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cressi_goa_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (cressi_goa_device_t *) dc_device_allocate (context, &cressi_goa_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (3000 ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Clear the RTS line.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the RTS line.");
		goto error_free;
	}

	// Clear the DTR line.
	status = dc_iostream_set_dtr (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the DTR line.");
		goto error_free;
	}

	dc_iostream_sleep (device->iostream, 100);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
cressi_goa_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	cressi_goa_device_t *device = (cressi_goa_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

struct cressi_goa_irda_api_conf {
	unsigned char version;
	unsigned int idlen;
	unsigned char logbook_entry_len;
	unsigned int logbook_fp_offset;
	unsigned char cmd_logbook;
};

static int
cressi_goa_irda_api_version(dc_device_t *abstract, dc_event_devinfo_t *devinfo)
{
	if (devinfo->model > 11) {
		ERROR (abstract->context, "Unknown model %d.", devinfo->model);
		return -1;
	}
	int version;
	if (devinfo->firmware >= 161 && devinfo->firmware <= 165) {
		version = 0;
	} else if (devinfo->firmware >= 166 && devinfo->firmware <= 169) {
		version = 1;
	} else if (devinfo->firmware >= 170 && devinfo->firmware <= 179) {
		version = 2;
	} else if ((devinfo->firmware >= 100 && devinfo->firmware <= 110)
			|| devinfo->firmware == 900) {
		version = 3;
	} else if (devinfo->firmware >= 200 && devinfo->firmware <= 205) {
		version = 4;
	} else {
		ERROR (abstract->context, "Unknown firmware version %d.", devinfo->firmware);
		return -1;
	}
	const bool version_support_on_model[5][11] = {
		/*  1      2      3      4      5      6      7      8      9      10     11  */
		{  true,  true, false, false, false, false, false, false, false, false, false }, /* API v0 */
		{  true,  true, false, false, false, false, false, false, false, false, false }, /* API v1 */
		{  true,  true, false, false, false, false, false, false,  true, false, false }, /* API v2 */
		{  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true }, /* API v3 */
		{  true,  true, false,  true,  true, false, false, false,  true,  true, false }, /* API v4 */
	};
	if (!version_support_on_model[version][devinfo->model - 1]) {
		ERROR (abstract->context, "Firmware version %d of Model %d not known to have support for API v%d.",
				devinfo->firmware, devinfo->model, version);
		return -1;
	}
	return version;
}

static dc_status_t
cressi_goa_read_id(dc_device_t *abstract, bool emit_events, struct cressi_goa_irda_api_conf *conf)
{
	static const struct cressi_goa_irda_api_conf version_conf[] = {
		{ 0,  9, SZ_HEADER, FP_OFFSET, CMD_LOGBOOK },
		/*    4 is the new version
		 *   11 is the new response length to the `CMD_VERSION` command
		 *   15 is the length of an entry in the Logbook
		 *    3 is the offset to the START date in the Logbook header
		 * 0x23 is the new command to request the Logbook
		 */
		{ 4, 11,        15,         3, CMD_LOGBOOK_V4 },
	};
	const struct cressi_goa_irda_api_conf *conf_ = NULL;
	cressi_goa_device_t *device = (cressi_goa_device_t *) abstract;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_buffer_t *id = dc_buffer_new(11);
	if (id == NULL) {
		ERROR (abstract->context, "Failed to allocate memory for the ID.");
		return DC_STATUS_NOMEMORY;
	}
	status = cressi_goa_device_transfer (device, CMD_VERSION, NULL, 0, id, NULL, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the version information.");
		goto error_exit;
	}
	for (size_t version = 0; version < C_ARRAY_SIZE(version_conf); version++) {
		if (dc_buffer_get_size(id) != version_conf[version].idlen)
			continue;
		conf_ = &version_conf[version];
		break;
	}
	if (conf_ == NULL) {
		ERROR (abstract->context, "Could not find a configuration candidate.");
		goto error_exit;
	}
	// Emit a vendor event.
	dc_event_vendor_t vendor;
	unsigned char *id_data = dc_buffer_get_data(id);
	vendor.data = id_data;
	vendor.size = dc_buffer_get_size(id);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = id_data[4];
	devinfo.firmware = array_uint16_le (id_data + 5);
	devinfo.serial = array_uint32_le (id_data + 0);

	int version = cressi_goa_irda_api_version(abstract, &devinfo);
	if (version == -1) {
		status = DC_STATUS_UNSUPPORTED;
		goto error_exit;
	}
	*conf = *conf_;
	conf->version = version;
	if (emit_events) {
		device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);
		device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);
	}
error_exit:
	dc_buffer_free(id);
	return status;
}

static dc_status_t
cressi_goa_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cressi_goa_device_t *device = (cressi_goa_device_t *) abstract;
	dc_buffer_t *logbook = NULL;
	dc_buffer_t *dive = NULL;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Read the version information.
	struct cressi_goa_irda_api_conf conf;
	status = cressi_goa_read_id(abstract, false, &conf);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Allocate memory for the logbook data.
	logbook = dc_buffer_new(4096);
	if (logbook == NULL) {
		ERROR (abstract->context, "Failed to allocate memory for logbook.");
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Read the logbook data.
	status = cressi_goa_device_transfer (device, conf.cmd_logbook, NULL, 0, NULL, logbook, &progress);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the logbook data.");
		goto error_free_logbook;
	}

	const unsigned char *logbook_data = dc_buffer_get_data(logbook);
	size_t logbook_size = dc_buffer_get_size(logbook);

	// Count the number of dives.
	unsigned int count = 0;
	unsigned int offset = logbook_size;
	while (offset > conf.logbook_entry_len) {
		// Move to the start of the logbook entry.
		offset -= conf.logbook_entry_len;

		// Get the dive number.
		unsigned int number= array_uint16_le (logbook_data + offset);
		if (number == 0)
			break;

		// Compare the fingerprint to identify previously downloaded entries.
		if (memcmp (logbook_data + offset + conf.logbook_fp_offset, device->fingerprint, sizeof(device->fingerprint)) == 0) {
			break;
		}

		count++;
	}

	// Update and emit a progress event.
	progress.maximum = (count + 1) * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate memory for the dive data.
	dive = dc_buffer_new(4096);
	if (dive == NULL) {
		ERROR (abstract->context, "Failed to allocate memory for dive data.");
		status = DC_STATUS_NOMEMORY;
		goto error_free_logbook;
	}

	// Download the dives.
	offset = logbook_size;
	for (unsigned int i = 0; i < count; ++i) {
		// Move to the start of the logbook entry.
		offset -= conf.logbook_entry_len;

		// Read the dive data.
		status = cressi_goa_device_transfer (device, CMD_DIVE, logbook_data + offset, 2, NULL, dive, &progress);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive data.");
			goto error_free_dive;
		}

		const unsigned char *dive_data = dc_buffer_get_data (dive);
		size_t dive_size = dc_buffer_get_size (dive);

		if (conf.version != 4) {
			// Verify the header in the logbook and dive data are identical.
			// After the 2 byte dive number, the logbook header has 5 bytes
			// extra, which are not present in the dive header.
			if (dive_size < SZ_HEADER - 5 ||
				memcmp (dive_data + 0, logbook_data + offset + 0, 2) != 0 ||
				memcmp (dive_data + 2, logbook_data + offset + 7, SZ_HEADER - 7) != 0) {
				ERROR (abstract->context, "Unexpected dive header.");
				status = DC_STATUS_DATAFORMAT;
				goto error_free_dive;
			}
		} else {
			/* The header format is as follows:
			 *
			 *  ID  | SAMPLES | START | RATE | TARA | SESSION | DIPS | unused | ...
			 *  u16 |   u16   |  DATE |  u8  |  u8  |   u16   |  u8  |   u8   | ...
			 *
			 *  ... MAXDEPTH | MINTEMP | SURFTIME | DIVETIME | BESTDIP | CRC
			 *  ...    u16   |   u16   |    u16   |    u16   |   u16   | u16
			 *
			 *  START = A Date in the "default" byte-wise form u16u8u8u8u8 for YMDhm
			 *  RATE = SampleRate - 1=0.5sec 2=1sec 3=2sec
			 *  TARA = Taravana Factor
			 *  SESSION = Session Time
			 *  DIPS = Number of Dips/Dives in this session
			 */
			unsigned short log_entry = array_uint16_le(logbook_data + offset);
			unsigned short log_entry_ = array_uint16_le(dive_data);
			if (log_entry_ != log_entry) {
				ERROR (abstract->context, "Unexpected log entry %d != %d.", log_entry_, log_entry);
				status = DC_STATUS_DATAFORMAT;
				goto error_free_dive;
			}
			const unsigned char *start_date = logbook_data + offset + 3;
			if (memcmp(dive_data + 4, start_date, 6) != 0) {
				unsigned long long is = array_uint64_be(dive_data + 4);
				unsigned long long should = array_uint64_be(start_date);
				is >>= 16;
				should >>= 16;
				ERROR (abstract->context, "Unexpected start date 0x%0llx != 0x%0llx.", is, should);
				status = DC_STATUS_DATAFORMAT;
				goto error_free_dive;
			}
			unsigned short num_dips = dive_data[14];
			DEBUG (abstract->context, "Received %zu bytes of data for dip %d with %d samples.",
					dc_buffer_get_size (dive),
					array_uint16_le(dc_buffer_get_data (dive)),
					num_dips);
		}

		/* Inject a header
		 * [0] = divecomputer  -  0xdc
		 * [1] = divecomputer  -  0xdc
		 * [2] = version       -  0x**
		 * [3] = length        -  0x**
		 */
		unsigned char header[4] = {0xdc, 0xdc, conf.version, conf.logbook_entry_len};
		if (!dc_buffer_insert (dive, 0, header, 4)) {
			ERROR (abstract->context, "Out of memory.");
			status = DC_STATUS_NOMEMORY;
			goto error_free_dive;
		}
		/* Inject the logbook entry, which contains the dive mode */
		if (!dc_buffer_insert (dive, 4, logbook_data + offset, conf.logbook_entry_len)) {
			ERROR (abstract->context, "Out of memory.");
			status = DC_STATUS_NOMEMORY;
			goto error_free_dive;
		}

		dive_data = dc_buffer_get_data (dive);
		dive_size = dc_buffer_get_size (dive);

		if (callback && !callback(dive_data, dive_size, dive_data + 4 + conf.logbook_fp_offset, sizeof(device->fingerprint), userdata))
			break;
	}

error_free_dive:
	dc_buffer_free(dive);
error_free_logbook:
	dc_buffer_free(logbook);
error_exit:
	return status;
}

static dc_status_t cressi_goa_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cressi_goa_device_t *device = (cressi_goa_device_t *) abstract;

	unsigned char new_time[7];
	array_uint16_le_set(new_time, datetime->year);
	new_time[2] = datetime->month;
	new_time[3] = datetime->day;
	new_time[4] = datetime->hour;
	new_time[5] = datetime->minute;
	new_time[6] = datetime->second;
	status = cressi_goa_device_transfer (device, CMD_SET_TIME, new_time, 7, NULL, NULL, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the new time.");
	}
	return status;
}

static dc_status_t cressi_goa_device_close (dc_device_t *abstract)
{
	cressi_goa_device_t *device = (cressi_goa_device_t *) abstract;
	dc_status_t status = cressi_goa_device_transfer (device, CMD_EXIT_PCLINK, NULL, 0, NULL, NULL, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to exit PC Link.");
	}
	return status;
}
