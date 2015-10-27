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
unsigned acknowledge_frame(datalink_t *datalink);
unsigned get_data_frame(datalink_t *datalink, frame_t *frame);
void inc_sequence_number(unsigned int *seq_num);
int check_frame_order(datalink_t *datalink, frame_t *frame);

alarm_info_t alrm_info;
void alarm_handler() {
	printf("Alarm handler started.\n");
	if(alrm_info.stop) {
		alrm_info.tries_left = 0;
		return;
	}

	if(alrm_info.frame == NULL) {
		if(alrm_info.tries_left > 0)
			--alrm_info.tries_left;
		else {
			if(kill(getpid(), SIGINT) != 0) {	// to return from blocking read in get_frame
				printf("ERROR (alarm_handler): unable to send SIGINT signal.\n");
				return;
			}
		}
		return;
	}

	if(alrm_info.tries_left > 0) {
		alrm_info.tries_left--;
		if(send_frame(alrm_info.fd, alrm_info.frame)) {
			printf("ERROR (alarm_handler): unable to send requested frame\n");
			return;
		}
		alarm(alrm_info.time_dif);
	} else {
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
	if(send_frame(alrm_info.fd, alrm_info.frame)) {
		printf("ERROR (write_timed_frame): send_frame failed.\n");
		return 1;
	}
	alarm(alrm_info.time_dif);

	return 0;
}

int read_byte(int fd, unsigned char *c)
{
	int res = read(fd,c,1);
	if(res >= 1) {
		printf("Read 0x%X", + *c);
	}
	return res;
}

void datalink_init(datalink_t *datalink, unsigned int mode) {
	datalink->mode = mode;
	datalink->curr_seq_number = 0;
	datalink->repeat = 0;
	datalink->frame_order = FIRST;
	datalink->fd = -1;
}

int llopen(const char *filename, datalink_t *datalink) {
	int vtime = 0;
	int vmin = 1;
	int serial_fd = serial_initialize(filename, vmin, vtime);
	if (serial_fd < 0) {
		printf("ERROR (llopen): serial_initialize failed.\n");
		return 1;
	}
	datalink->fd = serial_fd;

	switch(datalink->mode) {
	case SENDER:
		if(llopen_transmitter(serial_fd)) {
			printf("ERROR (llopen): llopen_transmitter failed\n");
			return 1;
		}
		break;
	case RECEIVER:
		if(llopen_receiver(serial_fd)) {
			printf("ERROR (llopen): llopen_receiver failed\n");
			return 1;
		}
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
		if(llclose_transmitter(datalink->fd)) {
			printf("ERROR (llclose): llclose_transmitter failed\n");
			//return 1;
		}
		break;
	case RECEIVER:
		if(llclose_receiver(datalink->fd)) {
			printf("ERROR (llclose): llclose_receiver failed\n");
			//return 1;
		}
		break;
	default:
		printf("ERROR (llclose): invalid serial port opening mode.");
		//return 1;
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
		printf("ERROR (llopen_transmitter): get_frame failed\n");
		return 1;
	}
	if(alrm_info.stop == 2) {
		printf("ERROR (llopen_transmitter): transmission failed (number of attempts to get UA exceeded)\n");
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

	alrm_info.frame = NULL;
	alrm_info.tries_left = 0;
	alrm_info.stop = 0;
	signal(SIGALRM, alarm_handler);
	/*struct sigaction act;
	memset (&act, '\0', sizeof(act));
	//act.sa_sigaction = &alarm_handler;
	//act.sa_flags = SA_SIGINFO | (SA_RESTART ^ (unsigned)0xFFFFFFFFFF);
	if (sigaction(SIGTERM, &act, NULL) < 0) {
		perror ("sigaction");
		return 1;
	}*/
	alarm(15);

	int attempts = INIT_CONNECTION_TRIES;

	while (attempts > 0) {
		frame_t frame;
		if(get_frame(fd, &frame)) {
			printf("ERROR (llopen_receiver): get_frame failed\n");
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
	if(attempts <= 0) {
		printf("ERROR (llopen_receiver): transmission failed (number of attempts to receiver SET exceeded\n");
		return 1;
	}

	frame_t answer;
	answer.sequence_number = 0;
	answer.control_field = C_UA;
	answer.type = CMD_FRAME;
	answer.address_field = A_TRANSMITTER;

	if(send_frame(fd, &answer)) {
		printf("ERROR (llopen_receiver): unable to answer sender's SET.\n");
		return 1;
	}

	alrm_info.stop = 1;
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
		printf("ERROR (llclose_transmitter): get_frame failed\n");
		return 1;
	}
	if(alrm_info.stop == 2) {
		printf("ERROR (llclose_transmitter)\n");
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
			printf("ERROR (llclose_receiver): get_frame failed\n");
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
	if(attempts <= 0) {
		printf("ERROR (llclose_receiver): transmission failed (attemts to get DISC exceeded\n");
		return 1;
	}

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
	unsigned attempts = LLWRITE_ANSWER_TRIES;
	frame_t frame;
	frame.sequence_number = datalink->curr_seq_number;
	if ((frame.buffer = malloc(length)) == NULL) {
		printf("ERROR (llwrite): unable to allocate %d bytes of memory\n", length);
		return 1;
	}
	memcpy(frame.buffer, buffer, length);
	frame.length = length;
	frame.control_field = C_DATA(frame.sequence_number);
	frame.type = DATA_FRAME;
	frame.address_field = A_TRANSMITTER;

	alrm_info.fd = datalink->fd;
	alrm_info.tries_left = INIT_CONNECTION_TRIES;
	alrm_info.time_dif = INIT_CONNECTION_RESEND_TIME;
	alrm_info.frame = &frame;
	alrm_info.stop = 0;

	if(write_timed_frame()) {
		printf("ERROR (llwrite): unable to start alarms\n");
		return 1;
	}

	while(attempts-- > 0) {
		frame_t answer;
		if(get_frame(datalink->fd, &answer)) {
			printf("ERROR (llwrite): get_frame failed\n");
			continue;
		}
		if(alrm_info.stop == 2) {
			printf("ERROR (llwrite): transmission failed (number of attempts to get RR exceeded)\n");
			continue;
		}

		if(answer.type != CMD_FRAME) {
			printf("Invalid RR or REJ received\n");
			continue;
		}

		if(check_bcc1(&answer)) {
			printf("Invalid bcc1 in frame received\n");
			continue;
		}

		if(answer.control_field != C_RR((datalink->curr_seq_number + 1)%2)) {
			printf("Inavlid RR value received");
			continue;
		}

		/*if(invalid_frame(&answer) || frame.control_field != C_RR(datalink->curr_seq_number)) {
			printf("ERROR (llwrite): received invalid frame. Expected valid RR command frame\n");
			continue;
		}*/

		alrm_info.stop = 1;
		break;
	}

	if(attempts == 0) {
		printf("ERROR (llwrite): communication failed. Attempts to send frame exceeded limit(%d)", LLWRITE_ANSWER_TRIES);
		return 1;
	}
	inc_sequence_number(&datalink->curr_seq_number);
	return 0;
}

int send_REJ(datalink_t *datalink) {
	frame_t frame;
	frame.sequence_number = datalink->curr_seq_number;
	frame.control_field = C_REJ(datalink->curr_seq_number);
	frame.type = CMD_FRAME;
	frame.address_field = A_TRANSMITTER;

	return send_frame(datalink->fd, &frame);
}

int send_RR(datalink_t *datalink) {
	frame_t frame;
	frame.sequence_number = datalink->curr_seq_number;
	frame.control_field = C_RR(frame.sequence_number);
	printf("=============>%d\n", frame.sequence_number);
	frame.type = CMD_FRAME;
	frame.address_field = A_TRANSMITTER;

	return send_frame(datalink->fd, &frame);
}

int llread(datalink_t *datalink, char * buffer) {
	printf("\n\n\n\t%d\n\n\n\n", datalink->curr_seq_number);
	alrm_info.frame = NULL;
	alrm_info.tries_left = 0;
	alrm_info.stop = 0;
	signal(SIGALRM, alarm_handler);
	alarm(15);

	frame_t frame;
	int tries = LLREAD_VALIDMSG_TRIES;
	while(tries-- > 0) {
		if(get_frame(datalink->fd, &frame)) {
			printf("ERROR (llread): unable to get frame\n");
			return -1;
		}

		if(check_bcc1(&frame)) {
			continue;
		}

		if(check_bcc2(&frame)) {
			if(ORDER_BIT(datalink->curr_seq_number) != frame.control_field) {
				printf("REJ\n");
				send_REJ(datalink);
				continue;
			} else {
				printf("BCC2 failed.\n");
				printf("RR%d\n", datalink->curr_seq_number);
				send_RR(datalink);
				continue;
			}
		}

		if(ORDER_BIT(datalink->curr_seq_number) != frame.control_field) {
			printf("BCC2 failed.\n");
			printf("RR%d\n", datalink->curr_seq_number);
			send_RR(datalink);
			continue;
		}

		alrm_info.stop = 1;
		inc_sequence_number(&datalink->curr_seq_number);
		send_RR(datalink);
		memcpy(buffer, frame.buffer, frame.length);
		return frame.length;
	}

	alrm_info.stop = 1;
	printf("ERROR (llread): attempts exceeded\n");
	return -1;
}

unsigned acknowledge_frame(datalink_t *datalink) {
	if(!datalink->repeat) {
		inc_sequence_number(&datalink->curr_seq_number);
	}

	frame_t frame;
	frame.sequence_number = datalink->curr_seq_number;
	frame.control_field = C_RR(frame.sequence_number);
	frame.type = CMD_FRAME;
	frame.address_field = A_TRANSMITTER;

	alrm_info.fd = datalink->fd;
	alrm_info.tries_left = LLREAD_ANSWER_TRIES;
	alrm_info.time_dif = LLREAD_ANSWER_RESEND_TIME;
	alrm_info.frame = &frame;
	alrm_info.stop = 0;

	return write_timed_frame();
}

unsigned get_data_frame(datalink_t *datalink, frame_t *frame) {
	int tries = LLREAD_VALIDMSG_TRIES;
	while(tries-- > 0) {
		if(get_frame(datalink->fd, frame)) {
			printf("ERROR (get_data_frame): unable to get a frame\n");
			return 1;
		}
		if(alrm_info.stop == 2) {
			printf("ERROR (get_data_frame): unable to get frame, ammount of attempts exceeded\n");
			return 1;
		}

		if(invalid_frame(frame) || (frame->control_field != ORDER_BIT(datalink->curr_seq_number))) {
			printf("ERROR (get_data_frame): received invalid frame. Expected valid DATA frame\n");
			return 1;
		}

		alrm_info.stop = 1;

		return 0;
	}

	return 1;
}

int check_frame_order(datalink_t *datalink, frame_t *frame) {
	if((ORDER_BIT(datalink->curr_seq_number) & ORDER_BIT(1)) ^ frame->control_field) {
		return 0;
	} else {
		return 1;
	}
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
	if (write(fd, msg, sizeof(msg)) != sizeof(msg)) {
		printf("ERROR (send_cmd_frame): write failed\n");
		return 1;
	}

	return 0;
}

int send_data_frame(int fd, const frame_t *frame)
{
	printf("Sending data frame...\n");
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
	if (frame->length > 0)
	{
		bcc2 = frame->buffer[0];
		for (i = 1; i < frame->length; ++i)
		{
			bcc2 ^= frame->buffer[i];
		}
	}
	else bcc2 = 0;
	unsigned char *bcc2_stuffed;
	unsigned length2;
	if (byte_stuffing(&bcc2, sizeof(bcc2), &bcc2_stuffed, &length2)) {
		printf("ERROR (send_data_frame): byte_stuffing failed\n");
		return 1;
	}
	printf("BCC2 size: %d.\n", length2);
	int pp;
	for(pp = 0; pp < length2; pp++) {
		printf("# 0x%X\n", bcc2_stuffed[pp]);
	}

	unsigned char ft[] = {FLAG};
	if (write(fd, fh, sizeof(fh)) != sizeof(fh)) return 1;
	if (write(fd, data, length) != length) return 1;
	if (write(fd, bcc2_stuffed, length2) != length2) return 1;
	if (write(fd, ft, sizeof(ft)) != sizeof(ft)) return 1;
	free(data);
	free(bcc2_stuffed);
	return 0;
}

void inc_sequence_number(unsigned int *seq_num) {
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
	*new_length = j;
	if ((*dst = malloc(*new_length)) == NULL) {
		printf("ERROR (byte_stuffing): unable to allocate %d bytes of memory\n", *new_length);
		return 1;
	}
	memcpy(*dst, stuffed, *new_length);
	return 0;
}

int byte_destuffing(const unsigned char *src, unsigned length, unsigned char **dst, unsigned *new_length)
{
	unsigned char destuffed[length];
	unsigned i;
	unsigned j;
	for (i = 0, j = 0; i < length; ++i, ++j)
	{
		if(src[i] == ESC)
		{
			destuffed[j] = src[++i] ^ ESC_XOR;
		}
		else
			destuffed[j] = src[i];
	}

	*new_length = j;
	if ((*dst = malloc(*new_length)) == NULL) {
		printf("ERROR (byte_destuffing): unable to allocate %d bytes of memory\n", *new_length);
		return 1;
	}
	memcpy(*dst, destuffed, *new_length);
	return 0;
}

int get_frame(int fd, frame_t *frame) {
	state_t state = START;
	unsigned char byte;
	if ((frame->buffer = malloc(sizeof(char) * 50000)) == NULL) {
		printf("ERROR (get_frame): unable to allocate %d bytes of memory\n", 50000);
		return 1;
	}
	unsigned char *buf = malloc(sizeof(char) * 50000);
	if(buf == NULL) {
		printf("ERROR (get_frame): unable to allocate %d bytes of memory\n", 50000);
		return 1;
	}
	unsigned buf_length = 0;
	frame->type = DATA_FRAME;
	frame->bcc2 = 0;
	char *test[] = {
			"START",
			"FLAG_RCV",
			"A_RCV",
			"C_RCV",
			"BCC1_RCV",
			"STOP"
	};

	while(state != STOP) {
		printf("PREV_STATE: %s\t", test[(int)state]);
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
			if(byte == A_TRANSMITTER || byte == A_RECEIVER) {
				state = A_RCV;
				frame->address_field = byte;
			} else if(byte != FLAG) {
				state = START;
			}
			break;
		case A_RCV:
			if(byte == FLAG) {
				state = FLAG_RCV;
			} else if(byte == C_SET || byte == C_UA || byte == C_DISC || byte == C_REJ(0) || byte == C_REJ(1) || byte == C_RR(0) || byte == C_RR(1)) {
				frame->type = CMD_FRAME;
				frame->control_field = byte;
				state = C_RCV;
			} else if(byte == C_DATA(0) || byte == C_DATA(1)) {
				frame->type = DATA_FRAME;
				frame->control_field = byte;
				state = C_RCV;
			} else {
				state = START;
			}
			break;
		case C_RCV:
		{
			state = BCC1_RCV;
			frame->bcc1 = byte;
			break;
		}
		case BCC1_RCV:
			if(byte == FLAG) {
				state = STOP;
			} else {
				buf[buf_length++] = byte;
			}
			break;
		case STOP:
			break;
		}
		printf("\tNEXT_STATE: %s\t\n", test[(int)state]);
	}

	printf("\n\tLEAVING State Machine\n\n");

	unsigned char *temp; unsigned int temp_len;

	if(frame->type == DATA_FRAME) {
		if(byte_destuffing(buf, buf_length, &temp, &temp_len)) {
			printf("ERROR (get_frame): failed to perform destuffing on DATA frame received\n");
			return 1;
		}

		frame->length = temp_len - 1;
		memcpy(frame->buffer, temp, temp_len - 1);
		frame->bcc2 = temp[temp_len-1];
		printf("bcc2 received: 0x%X\n", frame->bcc2);
	}

	printf("\n\tLEFT State Machine\n\n");

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
