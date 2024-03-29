#include "application.h"
#include "datalink.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <libgen.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX(A, B) (((A) > (B)) ? (A) : (B))
#define MIN(A, B) (((A) < (B)) ? (A) : (B))
#define GET_BYTE(X, N) (((X) & (0xFF << (N * 8))) >> (N * 8))

#define NUM_FILE_SEND_RECEIVE_RETRIES 3

typedef enum {
	PACKET_CTRL_TYPE_SIZE,
	PACKET_CTRL_TYPE_NAME
} packet_ctrl_type_t;

typedef enum {
	PACKET_CTRL_FIELD_DATA = 0,
	PACKET_CTRL_FIELD_START = 1,
	PACKET_CTRL_FIELD_END = 2
} packet_ctrl_field_t;

typedef struct {
	packet_ctrl_type_t type;
	unsigned char length;
	char *value;
} control_packet_param_t;

typedef struct {
	packet_ctrl_field_t ctrl_field;
	unsigned char sn;
	uint16_t length;
	char *data;
} data_packet_t;

typedef struct {
	packet_ctrl_field_t ctrl_field;
	unsigned num_params;
	control_packet_param_t *params;
} control_packet_t;

int send_data_packet(datalink_t *datalink, const data_packet_t *data_packet);
int send_control_packet(datalink_t *datalink, const control_packet_t *control_packet);
control_packet_param_t *get_param_by_type(const control_packet_t *control_packet, packet_ctrl_type_t type);
void show_progress_bar(float progress);
void print_usage(char *argv0);
int cli();
int baudrate = 0;
int max_packet_size = MAX_PACKET_SIZE;
int retransmission = DEFAULT_RETRANSMISSIONS;
int timeout = DEFAULT_TIMEOUT;

int main(int argc, char *argv[]) // ./file_transfer <port> <send|receive> <filename>
{
	srand(time(NULL));
	if (argc == 1) return cli();
	if (argc != 4 && argc != 3)
	{
		print_usage(argv[0]);
		return 1;
	}
	datalink_t datalink;
	if(strcmp(argv[2], "send") == 0) {
		if (argc != 4)
		{
			print_usage(argv[0]);
			return 1;
		}
		datalink_init(&datalink, SENDER);
		unsigned tries = 0;
		while (tries < NUM_FILE_SEND_RECEIVE_RETRIES)
		{
			if (send_file(argv[1], argv[3]))
				tries++;
			else
				return 0;
		}
		printf("Couldn't send file. Terminating program.\n");
	} else if(strcmp(argv[2], "receive") == 0) {
		if (argc != 3)
		{
			print_usage(argv[0]);
			return 1;
		}
		datalink_init(&datalink, RECEIVER);
		unsigned tries = 0;
		while (tries < NUM_FILE_SEND_RECEIVE_RETRIES)
		{
			if (receive_file(argv[1], argc == 4 ? argv[3] : ""))
				tries++;
			else
				return 0;
		}
		printf("Couldn't receive file. Terminating program.\n");
	}
	return 0;
}

void print_usage(char *argv0)
{
	printf("Usage:\n"
			"\t%s <port> send <filename>\n"
			"\t\tOR\n"
			"\t%s <port> receive\n", argv0, argv0);
}

int send_file(const char *port, const char *file_name)
{
	// Read file
	FILE *fp = fopen(file_name, "r");
	if (fp == NULL)
	{
		perror("Error opening file");
		return 1;
	}
	fseek(fp, 0, SEEK_END);
	unsigned long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	char data[size];
	fread(data, size, 1, fp);
	fclose(fp);

	// Establish connection
	datalink_t datalink;
	datalink_init(&datalink, SENDER);
	datalink->baudrate = baudrate;
	datalink->timeout = timeout;
	datalink->max_retransmissions = retransmission;
	if (llopen(port, &datalink)) return 1;

	// Send start packet
	control_packet_t control_packet;
	control_packet.ctrl_field = PACKET_CTRL_FIELD_START;
	control_packet.num_params = 2;

	control_packet_param_t param_size;
	param_size.type = PACKET_CTRL_TYPE_SIZE;
	char str[(size == 0) ? (2) : ((int)((ceil(log10(size)) + 1) * sizeof(char)))];
	if (sprintf(str, "%lu", size) < 0) return 1;
	param_size.length = strlen(str) + 1;
	param_size.value = str;
	printf("Size:::: %s\n", str);

	control_packet_param_t param_name;
	param_name.type = PACKET_CTRL_TYPE_NAME;
	char copy[strlen(file_name) + 1];
	strcpy(copy, file_name);
	char *base = basename(copy);
	char file[strlen(base) + 1];
	strcpy(file, base);
	param_name.length = strlen(file);
	param_name.value = file;

	control_packet_param_t params[] = {param_size, param_name};
	control_packet.params = params;
	if (send_control_packet(&datalink, &control_packet)) return 1;

	// Send data packet
	unsigned long i;
	unsigned sn = 0;
	for (i = 0; i < size; i += max_packet_size)
	{
		data_packet_t data_packet;
		data_packet.ctrl_field = PACKET_CTRL_FIELD_DATA;
		data_packet.sn = (char)(sn++ % (1 << 8));
		data_packet.length = MIN(max_packet_size, size - i);
		data_packet.data = &data[i];
		if (send_data_packet(&datalink, &data_packet)) return 1;
		show_progress_bar((float)i/size);
	}

	// Send end packet
	control_packet.ctrl_field = PACKET_CTRL_FIELD_END;
	if (send_control_packet(&datalink, &control_packet)) return 1;

	return llclose(&datalink);
}

int send_control_packet(datalink_t *datalink, const control_packet_t *control_packet)
{
	printf("Sending control packet...\n");
	unsigned size = 1 + 2 * control_packet->num_params;
	unsigned i;
	for (i = 0; i < control_packet->num_params; ++i)
	{
		size += control_packet->params[i].length;
	}
	unsigned char packet[size];
	packet[0] = control_packet->ctrl_field;
	unsigned j;
	for (i = 0, j = 1; i < control_packet->num_params; ++i)
	{
		packet[j++] = control_packet->params[i].type;
		packet[j++] = control_packet->params[i].length;
		memcpy(&packet[j], control_packet->params[i].value, control_packet->params[i].length);
		j += control_packet->params[i].length;
	}

	if (llwrite(datalink, packet, size))
	{
		printf("Error sending control packet.\n");
		return 1;
	}
	printf("Sent control packet with size %d.\n", size);
	return 0;
}

int send_data_packet(datalink_t *datalink, const data_packet_t *data_packet)
{
	printf("Sending data packet number %d ...\n", data_packet->sn);
	unsigned size = data_packet->length + 4;
	unsigned char packet[size];
	packet[0] = data_packet->ctrl_field;
	packet[1] = data_packet->sn;
	packet[2] = (uint8_t)((data_packet->length & 0xFF00) >> 8);
	packet[3] = (uint8_t)(data_packet->length & 0x00FF);
	memcpy(&packet[4], data_packet->data, size);
	if (llwrite(datalink, packet, size))
	{
		printf("Error data control packet.\n");
		return 1;
	}
	return 0;
}

int receive_file(const char *port, const char *destination_folder)
{
	// Establish connection
	datalink_t datalink;
	datalink_init(&datalink, RECEIVER);
	datalink->baudrate = baudrate;
	datalink->timeout = timeout;
	datalink->max_retransmissions = retransmission;
	if (llopen(port, &datalink)) return 1;
	char buf[max_packet_size];

	unsigned long size = llread(&datalink, buf);
	if (size < 0)
	{
		printf("Error: could not read data.\n");
		return 1;
	}

	// Read start packet
	control_packet_t control_packet;
	unsigned long i = 0;
	control_packet.ctrl_field = buf[i++];
	control_packet_param_t params[max_packet_size];
	unsigned long j;

	for (j = 0; i < size; ++j)
	{
		params[j].type = buf[i++];
		params[j].length = buf[i++];
		if ((params[j].value = malloc(params[j].length)) == NULL) return 1;
		memcpy(params[j].value, &buf[i], params[j].length);
		i += params[j].length;
	}
	control_packet.num_params = j;
	control_packet.params = params;

	control_packet_param_t *param_name = get_param_by_type(&control_packet, PACKET_CTRL_TYPE_NAME);
	control_packet_param_t *param_size = get_param_by_type(&control_packet, PACKET_CTRL_TYPE_SIZE);

	if (param_name == NULL || param_size == NULL)
	{
		printf("Error: could not read file header.\n");
		return 1;
	}

	char file_name[param_name->length + 1];
	memcpy(file_name, param_name->value, param_name->length);
	file_name[param_name->length] = '\0';
	char file_path[strlen(destination_folder) + strlen(file_name) + 1];
	strcpy(file_path, destination_folder);
	strcat(file_path, file_name);
	FILE *fp = fopen(file_path,"wb");
	if (fp == NULL)
	{
		perror("Error creating output file: ");
		return 1;
	}

	// Read data
	unsigned long bytes_read = 0;
	unsigned char sn = 0;
	unsigned long file_size = strtoul(param_size->value, NULL, 10);
	printf("File size: %lu bytes.\n", file_size);
	show_progress_bar(0);
	while (bytes_read < file_size)
	{
		data_packet_t data_packet;
		if (llread(&datalink, buf) < 0) return 1;
		data_packet.ctrl_field = buf[0];
		data_packet.sn = buf[1];
		data_packet.length = (((unsigned char)buf[2]) << 8) | ((unsigned char)buf[3]);
		data_packet.data = &buf[4];
		if (data_packet.ctrl_field != PACKET_CTRL_FIELD_DATA)
		{
			printf("Error receiving file. Received a control packet instead of a data packet.\n");
			return 1;
		}
		if (data_packet.sn != sn)
		{
			printf("Error receiving file. Expected packet number %d but received packet number %d instead.\n", sn, data_packet.sn);
			return 1;
		}
		sn = (unsigned char)(((unsigned)sn + 1) % (1 << 8));
		unsigned num_written = fwrite(data_packet.data, sizeof(char), data_packet.length, fp);
		if(num_written < data_packet.length)
		{
			printf("Error writting to output file. Could only write %d out of %d bytes.\n", num_written, data_packet.length);
			return 1;
		}
		bytes_read += data_packet.length;
		show_progress_bar((float)bytes_read / file_size);
	}

	// Read end packet
	size = llread(&datalink, buf);
	i = 0;
	control_packet.ctrl_field = buf[i++];
	if (control_packet.ctrl_field != PACKET_CTRL_FIELD_END)
	{
		printf("Error receiving file. Was expecting an end packet but received a different one instead.\n");
		return 1;
	}

	if (llclose(&datalink))
	{
		printf("Could not close connection properly.\n");
	}

	for (i = 0; i < control_packet.num_params; ++i)
	{
		free(params[i].value);
	}

	return 0;
}

void show_progress_bar(float progress)
{
	unsigned width = 30;
	unsigned i;
	printf("[");
	for (i = 0; i < width; ++i)
	{
		if (i < progress * width)
			printf("=");
		else
			printf(" ");
	}
	printf("] %.2f%%\n", progress * 100);
}

control_packet_param_t *get_param_by_type(const control_packet_t *control_packet, packet_ctrl_type_t type)
{
	unsigned i;
	for (i = 0; i < control_packet->num_params; ++i)
	{
		if (control_packet->params[i].type == type) return &control_packet->params[i];
	}
	return NULL;
}

int cli(){

	char *mode,*fileName, *port;
	int tries=3, valid=0;

	mode=malloc(100);
	fileName=malloc(100);
	port=malloc(100);

	while(tries-- > 0){

		printf("Mode (send/receive)? ");
		scanf("%s", mode);

		//verify mode entry
		if (strcmp(mode,"send") == 0 || strcmp(mode,"receive") == 0){
			valid=1;

			break;
		}else{
			printf("Invalid mode.\n");
			continue;
		}


	}

	if (!valid){
		printf("Invalid mode.\n");
		return -1;
	}

	tries=3;
	valid=0;

	if (strcmp(mode, "send") == 0)
	{
		while(tries-- > 0){

			printf("File name? ");
			scanf("%s", fileName);

			if(access(fileName,F_OK) == -1)
				perror("File does not exist.\n");
			else{
				valid=1;
				break;
			}
		}
		if (!valid){
			printf("Invalid input.\n");
			return -1;
		}
	}
	else
	{
		printf("Destination folder? ");
		scanf("%s", fileName);
	}
	printf("Port? ");
	scanf("%s",port);

	printf("Baudrate (0 to select default value)? ");
	scanf("%d", &baudrate);

	tries = 3;
	while(tries-- > 0) {
		max_packet_size = -1;
		printf("Max data packet size (0 to select default value, other values between 10 and 200)? ");
		scanf("%d", &max_packet_size);

		if(max_packet_size == 0) {
			max_packet_size = MAX_PACKET_SIZE;
			break;
		}

		if(max_packet_size < 10 || max_packet_size > 200) {
			printf("Invalid input!\n");
			max_packet_size = MAX_PACKET_SIZE;
			continue;
		}
	}

	printf("Timeout (0 to select default value)? ");
	scanf("%d", &timeout);
	if(timeout == 0)
		timeout = DEFAULT_TIMEOUT;

	printf("Max retransmissions (0 to select default value)? ");
	scanf("%d", &retransmission);
	if(retransmission == 0)
		retransmission = DEFAULT_RETRANSMISSIONS;

	if (strcmp(mode, "send") == 0)
		return send_file(port, fileName);
	else
		return receive_file(port, fileName);
}
