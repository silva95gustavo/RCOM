#include <stdio.h>
#include "frame_validator.h"

int invalid_frame(const frame_t *frame) {
	if(frame == NULL) return 1;

	switch(frame->type) {
	case DATA_FRAME:
		return invalid_data_frame(frame);
	case CMD_FRAME:
		return invalid_cmd_frame(frame);
	default:
		printf("ERROR (valid_frame): invalid frame type %d.", frame->type);
		return 1;
	}

	return 1;
}

int invalid_data_frame(const frame_t *frame) {
	// TODO
	return 0;
}

int invalid_cmd_frame(const frame_t *frame) {

	printf("testing valid cmd\n");

	if(frame->length != 1 || (frame->buffer[0] ^ frame->address_field) != frame->bcc1) {
		return 1;
	}

	return 0;
}
