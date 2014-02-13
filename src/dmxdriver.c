#define _POSIX_C_SOURCE 199309L


#include <pthread.h>
#include "dmxdriver.h"
#include "api.h"
#include <string.h>
#include <time.h>
#include <assert.h>

extern int watchdog_dmx_pong;

void
purge_buffers(struct ftdi_context *ftdic) {
	int ret;
	ret = ftdi_usb_purge_buffers(ftdic);
	if (ret == -1) {
		fprintf(stderr, "Read buffer purge failed");
		exit(1);
	} else if (ret == -2) {
		fprintf(stderr, "Write buffer purge failed");
		exit(2);
	} else if (ret == -3) {
		fprintf(stderr, "USB device unavailable");
		exit(3);
	}
}


int
purge_receive_buffer(struct ftdi_context *ftdic) {
	int ret;
	ret = ftdi_usb_purge_rx_buffer(ftdic);
	if (ret == -1) {
		fprintf(stderr, "Read buffer purge failed");
	} else if (ret == -2) {
		fprintf(stderr, "USB device unavailable");
	}
	return ret;
}


void
send_msg_error(int ret) {
	if (ret == -666) {
		fprintf(stderr, "USB Device Unavailable for ftdi_write_data\n");
	} else if (ret < 0) {
		fprintf(stderr, "ftdi_write_data returned error %d from usb_bulk_write\n", ret);
	} else {
		fprintf(stderr, "Unexpected number of bytes written: %d\n", ret);
	}
}


int
send_msg(struct ftdi_context *ftdic, int label, unsigned char *data, int length) {
	unsigned char end_code = MSG_END_CODE;
	int ret = 0;

	// Form Packet Header
	unsigned char header[MSG_HEADER_LENGTH];
	header[0] = MSG_START_CODE;
	header[1] = label;
	header[2] = length & OFFSET;
	header[3] = length >> BYTE_LENGTH;

	// Write The Header
	ret = ftdi_write_data(ftdic, header, MSG_HEADER_LENGTH);
	if (ret != MSG_HEADER_LENGTH) {
		send_msg_error(ret);
		return -1;
	}

	// Write The Data
	ret = ftdi_write_data(ftdic, data, length);
	if (ret != length) {
		send_msg_error(ret);
		return -1;
	}

	// Write End Code
	ret = ftdi_write_data(ftdic, &end_code, 1);
	if (ret != 1) {
		send_msg_error(ret);
		return -1;
	}

	return 0;
}


void
send_dmx(struct mk2_pro_context *mk2c, unsigned char *dmxbytes) {
	unsigned char my_dmx[513];
	int ret;
	
	memcpy(my_dmx + 1, dmxbytes, 512);

	// First byte has to be 0
	my_dmx[0] = 0;

	// send the array here
	ret = send_msg(mk2c->ftdic, SEND_DMX_2, my_dmx, 513);
	if (ret < 0)
	{
		fprintf(stderr, "FAILED to send DMX ... exiting. FIXME THIS MESSAGE TO BE USEFUL!\n");
		// TODO fix "nice" shutdown of FTDI.
	}
}


void
receive_msg_error(int ret) {
	if (ret == -666) {
		fprintf(stderr, "USB Device Unavailable for ftdi_read_data\n");
	} else if (ret < 0) {
		fprintf(stderr, "ftdi_read_data returned error %d from libusb_bulk_transfer\n", ret);
	} else {
		fprintf(stderr, "Unexpected number of bytes read: %d\n", ret);
	}
}


/*
 * Function that reads exactly length bytes from the ftdi device.
 * Buffer must be at least length bytes in length.
 *
 */
int
read_data(struct mk2_pro_context *mk2c, unsigned char *buffer, int length) {
	int ret;
	int bytes_read = 0;

	while (bytes_read < length && mk2c->running) {
		//fprintf(stderr, "Reading data, still %d to read.", length - bytes_read);
		ret = ftdi_read_data(mk2c->ftdic, buffer + bytes_read, length - bytes_read);
		if (ret < 0) {
			receive_msg_error(ret);
			return -1;
		}
		bytes_read += ret;
	}
	if (!mk2c->running) {
		pthread_exit(NULL);
	}

	assert(bytes_read == length);
	return bytes_read;
}


int
receive_msg(struct mk2_pro_context *mk2c, struct application_message *appmsg) {
	int ret;
	int label;
	int length = 0;
	unsigned char header[MSG_HEADER_LENGTH];
	unsigned char buffer[600];

	/* Read header */
	//fprintf(stderr, "Reading new packet header.\n");
	ret = read_data(mk2c, header, MSG_HEADER_LENGTH);
	if (ret == -1) {
		return -1;
	}

	/* Do sanity check on header contents */
	/* TODO filter on valid labels */
	/* Label 5 should have a length of 514 */
	while (1) {
		label = header[1];
		length = header[2];
		length += ((int) header[3]) << BYTE_LENGTH;

		if ((header[0] != MSG_START_CODE) || (length > 600)) {
			fprintf(stderr, "Received:\nStart code: 0x%X\nLabel:      %u\nLength:     %u\n", header[0], label, length);
			fprintf(stderr, "Header invalid. Cycling...\n");
			
			/* Move the header one byte, discarding the "wrong" MSG_START_CODE, then read another byte and repeat */
			memmove(header, header + 1, MSG_HEADER_LENGTH - 1);
			ret = read_data(mk2c, header + 3, 1);
			if (ret != 1) {
				return -1;
			}
		} else {
			//fprintf(stderr, "Header is sane. Reading %u bytes.\n", length);
			break;
		}
	}
	
	ret = read_data(mk2c, buffer, length);
	if (ret == -1) {
		return -1;
	}

	/* Check end code */
	ret = read_data(mk2c, header, 1);
	if (ret == -1) {
		return -1;
	}

	if (header[0] != MSG_END_CODE) {
		fprintf(stderr, "End code not at expected location.\n");
		return -1;
	}

	// Copy The Data read to the buffer passed
	appmsg->label = label;
	appmsg->length = length;
	memcpy(appmsg->data, buffer, length);
	return label;
}

void
read_dmx_usb_mk2_pro(struct mk2_pro_context *mk2c) {
	int ret;
	unsigned char dmx_state[512];
	struct application_message appmsg;

	while (mk2c->running) {
		watchdog_dmx_pong = 1;
		ret = receive_msg(mk2c, &appmsg);

		if (ret < 0) {
			fprintf(stderr, "Error occurred during receive. Purging receive buffer and retrying.\n");
			ret = purge_receive_buffer(mk2c->ftdic);
			continue;
		}

		/* TODO handle everything that is not DMX message. */
		if (appmsg.label != 5) {
			fprintf(stderr, "Unable to handle message with label %d.\n", appmsg.label);
			continue;
		}

		if ((appmsg.data[0] & 0x02) != 0) {
			fprintf(stderr, "Widget receive overrun occurred. DMX Data invalid.\n");
			continue;
		} else if ((appmsg.data[0] & 0x01) != 0) {
			fprintf(stderr, "Widget receive queue overflowed. DMX Data invalid.\n");
			continue;
		}

		if (appmsg.data[1] != 0) {
			fprintf(stderr, "Received DMX start code not equal to 0x00: 0x%X\n", appmsg.data[1]);
		}

		for (int i = 0; i < 512; ++i) {
			if (appmsg.data[i + 2] != dmx_state[i]) {
				fprintf(stderr, "Channel %d changed to value %d\n", i + 1, appmsg.data[i + 2]);
				/* todo callback */
				mk2c->update_callback(i, dmx_state[i], appmsg.data[i+2]);
				dmx_state[i] = appmsg.data[i+2];
			}
		}
		mk2c->commit_callback();
	}
}

static void *
read_dmx_usb_mk2_pro_runner(void *mk2c) {
	read_dmx_usb_mk2_pro(mk2c);
	return NULL;
}


int
connect_dmx_usb_mk2_pro(struct ftdi_context *ftdic) {
	int ret;

	/* Search for the FTDI device powering the Enttec DMX USB Mk2 Pro: vendor 0x0403, product 0x6001,
	   description DMX USB PRO Mk2, serial ENVWI3AT (but unnecessary so NULL). */
	ret = ftdi_usb_open_desc(ftdic, 0x0403, 0x6001, "DMX USB PRO Mk2", NULL);
	switch (ret) {
		case -3:
			fprintf(stderr, "USB device not found\n");
			break;
		case -4:
			fprintf(stderr, "Unable to open device\n");
			break;
		case -5:
			fprintf(stderr, "Unable to claim device\n");
			break;
		case -6:
			fprintf(stderr, "Device reset failed\n");
			break;
		case -7:
			fprintf(stderr, "Setting baud rate failed\n");
			break;
		case -8:
			fprintf(stderr, "Getting product description failed\n");
			break;
		case -9:
			fprintf(stderr, "Getting serial number failed\n");
			break;
		case -12:
			fprintf(stderr, "libusb_get_device_list() failed\n");
			break;
		case -13:
			fprintf(stderr, "libusb_get_device_descriptor() failed\n");
			break;
	}
	return ret;
}


int
enable_second_universe(struct ftdi_context *ftdic) {
	unsigned char port_set[] = { 1, 1};
	int ret = 0;

	ret = send_msg(ftdic, SET_API_KEY, APIKey, 4);
	if (ret != 0) {
		fprintf(stderr, "Setting API key failed.");
		return ret;
	}

	ret = send_msg(ftdic, SET_PORT_ASSIGNMENT, port_set, 2);
	if (ret != 0) {
		fprintf(stderr, "Setting port assignment failed.");
		return ret;
	}

	return 0;
}


/**
 * on_change: 0 if we want to receive every DMX message, 1 if we want to receive only changes.
 */
int
set_dmx_recv_mode(struct ftdi_context *ftdic, unsigned char on_change) {
	int ret;

	ret = send_msg(ftdic, RECEIVE_DMX_ON_CHANGE_1, &on_change, 1);
	if (ret != 0) {
		fprintf(stderr, "Setting receive mode failed.");
	}

	return ret;
}


struct mk2_pro_context *
init_dmx_usb_mk2_pro(dmx_update_callback_t update_callback, dmx_commit_callback_t commit_callback, dmx_error_callback_t error_callback) {
	int ret;
	struct mk2_pro_context *mk2c;

	mk2c = malloc(sizeof(struct mk2_pro_context));
	if (mk2c == NULL) {
		fprintf(stderr, "Error allocating memory for mk2_pro_context.");
		exit(1);
	}

	mk2c->ftdic = ftdi_new();
	if (mk2c->ftdic == NULL) {
		free(mk2c);
		fprintf(stderr, "ftdi_init failed\n");
		return NULL;
	}

	ret = connect_dmx_usb_mk2_pro(mk2c->ftdic);
	if (ret != 0) {
		ftdi_free(mk2c->ftdic);
		free(mk2c);
		return NULL;
	}

	purge_buffers(mk2c->ftdic);

	ret = enable_second_universe(mk2c->ftdic);
	ret = set_dmx_recv_mode(mk2c->ftdic, 0);

	mk2c->update_callback = update_callback;
	mk2c->commit_callback = commit_callback;
	mk2c->error_callback = error_callback;
	mk2c->running = 1;
	pthread_create(&mk2c->readid, NULL, read_dmx_usb_mk2_pro_runner, mk2c);

	return mk2c;
}


void
teardown_dmx_usb_mk2_pro(struct mk2_pro_context *mk2c) {
	mk2c->running = 0;
	pthread_join(mk2c->readid, NULL);
	purge_buffers(mk2c->ftdic);
	ftdi_usb_close(mk2c->ftdic);
	ftdi_free(mk2c->ftdic);
	free(mk2c);
}
