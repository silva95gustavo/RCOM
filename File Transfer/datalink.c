#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include "datalink.h"
#include "serial.h"
#include "frame_validator.h"

int send_cmd_frame(int fd, const frame_t *frame);
int send_data_frame(int fd, const frame_t *frame);
int byte_stuffing(const unsigned char *src, unsigned length, unsigned char **dst, unsigned *new_length);
int byte_destuffing(const unsigned char *src, unsigned length, unsigned char **dst, unsigned *new_length);
int get_frame(int fd, frame_t *frame);
int byte_stuffing(const unsigned char *src, unsigned length, unsigned char **dst, unsigned *new_length);
int write_timed_frame();
void alarm_handler();
int send_frame(int fd, const frame_t *frame);
int llopen_transmitter(int fd);
int llopen_receiver(int fd);
int llclose_transmitter(int fd);
int llclose_receiver(int fd);
int llread_first(datalink_t *datalink, char * buffer);
int llread_middle(datalink_t *datalink, char * buffer);
int llread_last(datalink_t *datalink, char * buffer);
unsigned acknowledge_frame(datalink_t *datalink);
unsigned get_data_frame(datalink_t *datalink, frame_t *frame);
void inc_sequence_number(int *seq_num);

alarm_info_t alrm_info;
void alarm_handler() {
	if(alrm_info.stop) {
		alrm_info.tries_left = 0;
		return;
	}

	if(alrm_info.tries_left > 0) {
		alrm_info.tries_left--;
		if(send_frame(alrm_info.fd, alrm_info.frame)) {
			printf("ERROR (alarm_handler): unable to send requested frame\n");
			return;
		}
		alarm(alrm_info.time_dif);
	} else if(alrm_info.stop) {
		alrm_info.stop = 2;
		if(kill(getpid(), SIGINT) != 0) {	// to return from blocking read in get_frame
			printf("ERROR (alarm_handler): unable to send SIGINT signal.\n");
			return;
		}
	}
}

int write_timed_frame() {

	if(alrm_info.frame == NULL || alrm_info.tries_left == 0 || alrm_info.time_dif == 0) {
		printf("ERROR (write_timed_frame): invalid alarm info parameters.\n");
		return 1;
	}

	signal(SIGALRM, alarm_handler);
	if(send_frame(alrm_info.fd, alrm_info.frame))
		return 1;
	alarm(alrm_info.time_dif);

	return 0;
}

int read_byte(int fd, unsigned char *c)
{
	int res = read(fd,c,1);
	/*if (res != 1)
	{
		printf("Error reading from the serial port.\n");
		return 1;
	}*/
	printf("Read %d bytes\n", res);
	if(res >= 1)
		printf("Read 0x%X\n", + *c);
	return res;
}

void datalink_init(datalink_t *datalink, unsigned int mode) {
	datalink->mode = mode;
	datalink->curr_seq_number = 0;
	datalink->repeat = 0;
	datalink->frame_order = FIRST;
	datalink->fd = -1;
}

int llopen(char *filename, datalink_t *datalink) {
	int vtime = 0;
	int vmin = 1;
	int serial_fd = serial_initialize(filename, vmin, vtime);
	if (serial_fd < 0) return 1;
	datalink->fd = serial_fd;

	switch(datalink->mode) {
	case SENDER:
		if(llopen_transmitter(serial_fd))
			return 1;
		break;
	case RECEIVER:
		if(llopen_receiver(serial_fd))
			return 1;
		break;
	default:
		printf("ERROR (llopen): invalid serial port opening mode.\n");
		return 1;
	}

	return 0;
}

int llclose(datalink_t *datalink) {
	switch(datalink->mode) {
	case SENDER:
		if(llclose_transmitter(datalink->fd))
			return 1;
		break;
	case RECEIVER:
		if(llclose_receiver(datalink->fd))
			return 1;
		break;
	default:
		printf("ERROR (llclose): invalid serial port opening mode.");
		return 1;
	}

	return serial_terminate(datalink->fd);
}

int llopen_transmitter(int fd) {
	frame_t frame;
	frame.sequence_number = 0;
	frame.control_field = C_SET;
	frame.type = CMD_FRAME;
	frame.address_field = A_TRANSMITTER;

	alrm_info.fd = fd;
	alrm_info.tries_left = INIT_CONNECTION_TRIES;
	alrm_info.time_dif = INIT_CONNECTION_RESEND_TIME;
	alrm_info.frame = &frame;
	alrm_info.stop = 0;

	write_timed_frame();

	frame_t answer;
	if(get_frame(fd, &answer)) {
		return 1;
	}
	if(alrm_info.stop == 2) {
		return 1;
	}
	if(invalid_frame(&answer) || answer.control_field != C_UA) {
		printf("ERROR (llopen_transmitter): received invalid frame. Expected valid UA command frame\n");
		return 1;
	}
	alrm_info.stop = 1;

	return 0;
}

int llopen_receiver(int fd) {

	int attempts = INIT_CONNECTION_TRIES;

	while (attempts > 0) {
		frame_t frame;
		if(get_frame(fd, &frame)) {
			return 1;
		}

		if(invalid_frame(&frame) || frame.control_field != C_SET) {
			printf("ERROR (llopen_receiver): received invalid frame. Expected valid SET command frame\n");
			//return 1;
		} else {
			break;
		}
		--attempts;
	}
	if(attempts <= 0)
		return 1;

	frame_t answer;
	answer.sequence_number = 0;
	answer.control_field = C_UA;
	answer.type = CMD_FRAME;
	answer.address_field = A_TRANSMITTER;

	if(send_frame(fd, &answer)) {
		printf("ERROR (llopen_receiver): unable to answer sender's SET.\n");
		return 1;
	}

	return 0;
}

int llclose_transmitter(int fd) {
	frame_t frame;
	frame.sequence_number = 0;
	frame.control_field = C_DISC;
	frame.type = CMD_FRAME;
	frame.address_field = A_TRANSMITTER;

	alrm_info.fd = fd;
	alrm_info.tries_left = FINAL_DISCONNECTION_TRIES;
	alrm_info.time_dif = FINAL_DISCONNECTION_RESEND_TIME;
	alrm_info.frame = &frame;
	alrm_info.stop = 0;

	write_timed_frame();

	frame_t answer;
	if(get_frame(fd, &answer)) {
		return 1;
	}
	if(alrm_info.stop == 2) {
		return 1;
	}
	if(invalid_frame(&answer) || answer.control_field != C_DISC) {
		printf("ERROR (llclose_transmitter): received invalid frame. Expected valid DISC command frame.\n");
		return 1;
	}
	alrm_info.stop = 1;

	frame_t final_ua;
	final_ua.sequence_number = 0;
	final_ua.control_field = C_UA;
	final_ua.type = CMD_FRAME;
	final_ua.address_field = A_TRANSMITTER;

	if(send_frame(fd, &final_ua)) {
		printf("ERROR (llclose_transmitter): unable to answer receiver's DISC.\n");
		return 1;
	}

	return 0;
}

int llclose_receiver(int fd) {

	int attempts = FINAL_DISCONNECTION_TRIES;

	while (attempts > 0) {
		frame_t frame;
		if(get_frame(fd, &frame)) {
			return 1;
		}

		if(invalid_frame(&frame) || frame.control_field != C_DISC) {
			printf("ERROR (llclose_receiver): received invalid frame. Expected valid DISC command frame.\n");
			//return 1;
		} else {
			break;
		}
		--attempts;
	}
	if(attempts <= 0)
		return 1;

	frame_t answer;
	answer.sequence_number = 0;
	answer.control_field = C_DISC;
	answer.type = CMD_FRAME;
	answer.address_field = A_TRANSMITTER;

	if(send_frame(fd, &answer)) {
		printf("ERROR (llclose_receiver): unable to answer sender's SET.\n");
		return 1;
	}

	return 0;
}

int llwrite(datalink_t *datalink, const unsigned char *buffer, int length) {

	frame_t frame;
	frame.sequence_number = 0;
	if ((frame.buffer = malloc(length)) == NULL) return 1;
	memcpy(frame.buffer, buffer, length);
	if (send_data_frame(datalink->fd, &frame)) return 1;
	return 0;
}

int llread(datalink_t *datalink, char * buffer) {

	switch(datalink->frame_order) {
	case FIRST:
		return llread_first(datalink, buffer);
		break;
	case MIDDLE:
		return llread_middle(datalink, buffer);
		break;
	case LAST:
		return llread_last(datalink, buffer);
		break;
	}

	return -1;
}

int llread_first(datalink_t *datalink, char * buffer) {
	frame_t frame;
	if(get_data_frame(datalink, &frame)) {
		return -1;
	}
	memcpy(buffer, frame.buffer, frame.length);
	return frame.length;
}

int llread_middle(datalink_t *datalink, char * buffer) {
	if(acknowledge_frame(datalink))
		return -1;
	frame_t frame;
	if(get_data_frame(datalink, &frame)) {
		return -1;

	memcpy(buffer, frame.buffer, frame.length);
	return frame.length;
}

int llread_last(datalink_t *datalink, char * buffer) {
	if(acknowledge_frame(datalink))
		return -1;
	return 0;
}

unsigned acknowledge_frame(datalink_t *datalink) {
	// TODO acknowledge current frame if not to repeat
	return 0;
}

unsigned get_data_frame(datalink_t *datalink, frame_t *frame) {
	// TODO get a frame with some tries and attention to global stop for alarms
	return 0;
}

int send_frame(int fd, const frame_t *frame) {
	switch(frame->type) {
	case CMD_FRAME:
		return send_cmd_frame(fd, frame);
	case DATA_FRAME:
		return send_data_frame(fd, frame);
	}

	printf("ERROR (send_frame): invalid frame type received.\n");
	return 1;
}

int send_cmd_frame(int fd, const frame_t *frame)
{
	unsigned char msg[] = {FLAG,
			frame->address_field,
			frame->control_field,
			frame->address_field ^ frame->control_field,
			FLAG};
	if (write(fd, msg, sizeof(msg)) != sizeof(msg)) return 1;

	return 0;
}

int send_data_frame(int fd, const frame_t *frame) // TODO UNTESTED
{
	unsigned char ctrl = frame->sequence_number << 5;
	unsigned char fh[] = {FLAG,
			A_TRANSMITTER,
			ctrl,
			A_TRANSMITTER ^ ctrl
	};

	unsigned char *data;
	unsigned length;
	if (byte_stuffing(frame->buffer, frame->length, &data, &length)) return 1;

	int i;
	unsigned char bcc2;
	if (length > 0)
	{
		bcc2 = data[0];
		for (i = 1; i < length; ++i)
		{
			bcc2 ^= data[i];
		}
	}
	else bcc2 = 0;
	unsigned char *bcc2_stuffed;
	unsigned length2;
	if (byte_stuffing(&bcc2, sizeof(bcc2), &bcc2_stuffed, &length2)) return 1;

	unsigned char ft[] = {*bcc2_stuffed,
			FLAG
	};

	if (write(fd, fh, sizeof(fh)) != sizeof(fh)) return 1;
	if (write(fd, data, length) != length) return 1;
	if (write(fd, bcc2_stuffed, length2) != length2) return 1;
	if (write(fd, ft, sizeof(ft)) != sizeof(ft)) return 1;

	free(data);
	free(bcc2_stuffed);
	return 0;
}

void inc_sequence_number(int *seq_num) {
	*seq_num = (*seq_num + 1)%2;
}

int byte_stuffing(const unsigned char *src, unsigned length, unsigned char **dst, unsigned *new_length)
{
	unsigned char stuffed[2 * length]; // 2 * length is the size in the worst case
	unsigned i;
	unsigned j;
	for (i = 0, j = 0; i < length; ++i, ++j)
	{
		if (src[i] == FLAG)
		{
			stuffed[j] = ESC;
			stuffed[++j] = FLAG ^ ESC_XOR;
		}
		else if (src[i] == ESC)
		{
			stuffed[j] = ESC;
			stuffed[++j] = ESC ^ ESC_XOR;
		}
		else
			stuffed[j] = src[i];
	}
	if ((*dst = malloc(j - 1)) == NULL) return 1;
	memcpy(*dst, stuffed, j - 1);
	return 0;
}

int byte_destuffing(const unsigned char *src, unsigned length, unsigned char **dst, unsigned *new_length)
{
	unsigned char destuffed[length];
	unsigned i;
	unsigned j;
	for (i = 0, j = 0; i < length; ++i, ++j)
	{
		if(src[i] != ESC)
			destuffed[j] = src[i];
	}
	if ((*dst = malloc(j - 1)) == NULL) return 1;
	memcpy(*dst, destuffed, j - 1);
	return 0;
}

int get_frame(int fd, frame_t *frame) {
	state_t state = START;
	unsigned char byte;
	if ((frame->buffer = malloc(sizeof(char) * 50000)) == NULL) return 1;

	while(state != STOP) {
		int ret = read_byte(fd, &byte);
		if(ret == 0) {
			return 1;
		}

		switch(state) {
		case START:
			if(byte == FLAG) {
				state = FLAG_RCV;
			}
			break;
		case FLAG_RCV:
			if(byte == A_TRANSMITTER || byte == A_RECEIVER) {	// TODO check
				state = A_RCV;
				frame->address_field = byte;
			} else if(byte != FLAG) {
				state = START;
			}
			break;
		case A_RCV:
			if(byte == FLAG) {
				state = FLAG_RCV;
			} else if(byte == C_SET || byte == C_UA || byte == C_DISC || byte == C_DATA(0) || byte == C_DATA(1)) {
				frame->type = CMD_FRAME;
				frame->control_field = byte;
				state = C_RCV;
			} else {
				state = START;
			}
			break;
		case C_RCV:
		{
			unsigned char bcc1 = frame->address_field ^ frame->control_field;
			if(byte == bcc1) {
				if(frame->type == CMD_FRAME)
					state = BCC1_RCV;
				else
					state = DATA_ESC_RCV;

				frame->bcc1 = bcc1;
			} else if (byte == FLAG){
				state = FLAG_RCV;
			} else {
				state = START;
			}
			break;
		}
		case BCC1_RCV:
			if(byte == FLAG) {
				state = STOP;
			} else {
				state = START;
			}
			break;
		case DATA_ESC_RCV:
			if(byte == ESC) {
				state = DATA_RCV;
			} else if(byte == FLAG) {
				//if(bcc2_checks(&frame)) {
				state = STOP;
				//}
			} else {
				frame->buffer[frame->length++] = byte;
			}
			break;
		case DATA_RCV:
			frame->buffer[frame->length++] = byte;
			state = DATA_ESC_RCV;
			break;
		case STOP:
			break;
		}
	}

	return 0;
}

/*
int read_byte(int fd, unsigned char *c)
{
	int res = read(fd,c,1);
	if (res != 1)
	{
		printf("Error reading from the serial port.\n");
		return 1;
	}
	printf("Read 0x%X\n", + *c);
	return 0;
}
 */
