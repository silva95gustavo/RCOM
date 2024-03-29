#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <libgen.h>

#define FTP_DEFAULT_PORT 21
#define FTP_CODE_NUM_DIGITS 3
#define BUFFER_SIZE 9000

typedef struct {
	int sockfd;
	int pasvsockfd;
	const char *user;
	const char *pass;
	const char *host;
	const char *path;
} Downloader;

bool validateURL(const char *url);
int parseURL(const char *url, char **user, char **pass, char **host, char **path);
int socket_connect(struct in_addr *server_address, unsigned server_port);
int host_to_address(const char *host, struct in_addr *address);
int download(const char* user, const char *pass, const char *host, const char *path);
int socket_send(const Downloader *downloader, const char *cmd, const char *arg);
int socket_receive(const Downloader *downloader, char *buf, unsigned length);
int ftp_send_username(Downloader *downloader);
int ftp_send_password(Downloader *downloader);
int ftp_passive_mode(Downloader *downloader);
int ftp_retrieve(Downloader *downloader);
int ftp_download(Downloader *downloader);
int ftp_quit(Downloader *downloader);
int ftp_get_code(const char *str);

int main(int argc, char *argv[])
{
	if (argc != 2)
	{
		printf("Usage: %s <url>\n", argv[0]);
		return 1;
	}
	if (!validateURL(argv[1])) return 1;

	char *user;
	char *pass;
	char *host;
	char *path;
	parseURL(argv[1], &user, &pass, &host, &path);
	bool error = false;
	if (download(user, pass, host, path)) {
		error = true;
	}

	free(user);
	free(pass);
	free(host);
	free(path);

	return error ? 1 : 0;
}

int download(const char* user, const char *pass, const char *host, const char *path) {
	struct in_addr address;
	if (host_to_address(host, &address)) return 1;
	int sockfd = socket_connect(&address, FTP_DEFAULT_PORT);
	if (sockfd < 0) return 1;

	Downloader downloader;
	downloader.sockfd = sockfd;
	downloader.user = user;
	downloader.pass = pass;
	downloader.host = host;
	downloader.path = path;

	char buf[100];
	socket_receive(&downloader, buf, 100);

	if (ftp_send_username(&downloader)) return 1;

	close(sockfd);
	return 0;
}

int ftp_send_username(Downloader *downloader) {
	char buf[BUFFER_SIZE];
	if (socket_send(downloader, "USER", (downloader->user == NULL) ? "anonymous" : downloader->user)) return 1;
	if (socket_receive(downloader, buf, BUFFER_SIZE) < 0) return 1;
	int code = ftp_get_code(buf);
	if (code < 0) return 1;
	else if (code == 331) return ftp_send_password(downloader);
	else if (code == 230) return ftp_passive_mode(downloader);
	else {
		printf("Unexpected response code %d.\n", code);
		return 1;
	}
}

int ftp_send_password(Downloader *downloader) {
	char buf[BUFFER_SIZE];
	if (socket_send(downloader, "PASS", downloader->pass)) return 1;
	if (socket_receive(downloader, buf, BUFFER_SIZE) < 0) return 1;
	int code = ftp_get_code(buf);
	if (code < 0) return 1;
	else if (code == 230) return ftp_passive_mode(downloader);
	else if (code == 530) {
		printf("Wrong username/password.\n");
		return 1;
	}
	else return 1;
}

int ftp_passive_mode(Downloader *downloader) {
	char buf[BUFFER_SIZE];
	if (socket_send(downloader, "PASV", NULL)) return 1;
	if (socket_receive(downloader, buf, BUFFER_SIZE) < 0) return 1;
	int code = ftp_get_code(buf);
	if (code < 0) return 1;
	else if (code != 227) {
		printf("Unexpected response code %d.\n", code);
		return 1;
	}

	int ip[4];
	unsigned port[2];
	if (sscanf(buf, "227 Entering Passive Mode (%d, %d, %d, %d, %d, %d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) < 6)
	{
		printf("Error reading passive mode info.\n");
		return 1;
	}

	char final_ip[3 * 4 + 3];
	unsigned final_port;
	if ((sprintf(final_ip, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3])) < 0) return 1;
	final_port = port[0] * 256 + port[1];

	printf("Passive IP: %s\n", final_ip);
	printf("Passive Port: %d\n", final_port);

	struct in_addr addr;
	addr.s_addr = inet_addr(final_ip);
	downloader->pasvsockfd = socket_connect(&addr, final_port);
	if (downloader->pasvsockfd < 0) {
		printf("Error connecting to passive FTP.\n");
		return 1;
	}
	return ftp_retrieve(downloader);
}

int ftp_retrieve(Downloader *downloader) {
	char buf[BUFFER_SIZE];
	if (socket_send(downloader, "RETR", downloader->path)) return 1;
	if (socket_receive(downloader, buf, BUFFER_SIZE) < 0) return 1;
	int code = ftp_get_code(buf);
	if (code < 0) return 1;
	else if (code == 150) return ftp_download(downloader);
	else {
		printf("Unexpected response code %d.\n", code);
		return 1;
	}
}

int ftp_download(Downloader *downloader) {
	FILE *fp;
	unsigned length = strlen(downloader->path);
	char path2[length];
	char filename[length];
	strcpy(path2, downloader->path);
	strcpy(filename, basename(path2));
	if ((fp = fopen(filename, "wb")) == NULL)
	{
		printf("Could not create output file.\n");
		return 1;
	}

	char buf[BUFFER_SIZE];
	int n;
	while (true)
	{
		n = recv(downloader->pasvsockfd, buf, BUFFER_SIZE, 0);
		if (n < 0) return 1;
		if (fwrite(buf, 1, n, fp) != n) {
			printf("Error writing data to file.\n");
			return 1;
		}
		if (n < BUFFER_SIZE) break;
	}
	if (fclose(fp) != 0) return 1;
	if (close(downloader->pasvsockfd) == -1) return 1;
	if (socket_receive(downloader, buf, BUFFER_SIZE) < 0) return 1;
	int code = ftp_get_code(buf);
	if (code < 0) return 1;
	else if (code == 226) return ftp_quit(downloader);
	else {
		printf("Unexpected response code %d.\n", code);
		return 1;
	}
}

int ftp_quit(Downloader *downloader) {
	char buf[BUFFER_SIZE];
	if (socket_send(downloader, "QUIT", NULL)) return 1;
	if (socket_receive(downloader, buf, BUFFER_SIZE) < 0) return 1;
	bool error = false;
	int code = ftp_get_code(buf);
	if (code < 0) return 1;
	else if (code != 221)
	{
		printf("Unexpected response code %d.\n", code);
		error = true;
	}
	if (close(downloader->sockfd) == -1) error = true;
	return error;
}

int ftp_get_code(const char *str) {
	if (strlen(str) < FTP_CODE_NUM_DIGITS) {
		printf("Invalid message code.\n");
		return -1;
	}
	char buf[FTP_CODE_NUM_DIGITS];
	memcpy(buf, str, FTP_CODE_NUM_DIGITS);
	int code = atoi(str);
	if (code == 0) {
		printf("Invalid message code.\n");
		return -1;
	}
	return code;
}

int socket_send(const Downloader *downloader, const char *cmd, const char *arg) {
	unsigned length = strlen(cmd) + ((arg == NULL) ? 0 : (1 + strlen(arg))) + 2;
	char buf[length + 1];
	if (arg == NULL)
		sprintf(buf, "%s\r\n", cmd);
	else
		sprintf(buf, "%s %s\r\n", cmd, arg);
	printf("> %s", buf);
	fflush(stdout);
	if (send(downloader->sockfd, buf, length, 0) != length) {
		printf("Error sending \"%s\".\n", buf);
		return 1;
	}
	return 0;
}

int socket_receive(const Downloader *downloader, char *buf, unsigned length) {
	size_t i;
	for (i = 0; i < length - 1; ++i)
	{
		unsigned r = recv(downloader->sockfd, &buf[i], 1, 0);
		if (r != 1) {
			if (r == 0) printf("Connection closed by the host.\n");
			else printf("Error reading server message.\n");
			return -1;
		}
		if (buf[i] == '\n') {
			buf[++i] = '\0';
			printf("< %s", buf);
			fflush(stdout);
			return i;
		}
	}
	return length;
}

bool validateURL(const char *url)
{
	char regexString[] = "^ftp://"	// ftp://
			"("
			"("
			"("
			"[^:@/]*"	// <user>
			")"
			"("
			":"			// :
			"("
			"[^:@/]*"	// <pass>
			")"
			")?"
			")?"
			"@"			// @
			")?"
			"("
			"[^:@/]+"	// <host>
			")"
			"/"			// /
			"(.+)"		// <path>
			"$";

	regex_t regex;

	int res;
	if ((res = regcomp(&regex, regexString, REG_EXTENDED)))
	{
		printf("Error #%d compiling regex.\n", res);
		return false;
	}
	else
		printf("Regex compiled successfully.\n");

	if (regexec(&regex, url, 0, NULL, 0) == 0)
	{
		printf("Regex validated.\n");
		regfree(&regex);
		return true;
	}
	else
	{
		printf("Regex validation failed.\n");
		regfree(&regex);
		return false;
	}
}

int parseURL(const char *url, char **user, char **pass, char **host, char **path)
{
	const char *temp = url;
	if ((temp = strchr(temp, '/')) == NULL) return 1;
	if ((temp = strchr(++temp, '/')) == NULL) return 1;

	unsigned length;
	char *atSign = strchr(++temp, '@');
	if (atSign == NULL)
	{
		*user = NULL;
		*pass = NULL;
	}
	else
	{
		char *colon = strchr(temp, ':');
		length = (colon == NULL ? atSign : colon) - temp;
		if ((*user = malloc((length + 1) * sizeof(char))) == NULL) return 1;
		memcpy(*user, temp, length);
		*(*user + length) = '\0';

		if ((colon != NULL && colon < atSign)) // Has password
		{
			temp = colon + 1;
			length = atSign - temp;
			if ((*pass = malloc((length + 1) * sizeof(char))) == NULL) return 1;
			memcpy(*pass, temp, length);
			*(*pass + length) = '\0';
		}
		else *pass = NULL;
	}

	temp = (atSign == NULL ? temp : atSign + 1);
	char *path_slash = strchr(temp, '/');
	length = path_slash - temp;
	if ((*host = malloc((length + 1) * sizeof(char))) == NULL) return 1;
	memcpy(*host, temp, length);
	*(*host + length) = '\0';

	temp = path_slash + 1;
	length = strlen(temp);
	if ((*path = malloc((length + 1) * sizeof(char))) == NULL) return 1;
	memcpy(*path, temp, length);
	*(*path + length) = '\0';

	if (*user != NULL) printf("User: %s\n", *user);
	if (*pass != NULL) printf("Pass: %s\n", *pass);
	if (*host != NULL) printf("Host: %s\n", *host);
	if (*path != NULL) printf("Path: %s\n", *path);

	return 0;
}

int socket_connect(struct in_addr *server_address, unsigned server_port) {
	int	sockfd;
	struct	sockaddr_in server_addr;

	/*server address handling*/
	bzero((char*)&server_addr,sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = server_address->s_addr;	/*32 bit Internet address network byte ordered*/
	server_addr.sin_port = htons(server_port);		/*server TCP port must be network byte ordered */
	printf("Connecting to %s:%d...\n", inet_ntoa(*server_address), server_port);
	/*open an TCP socket*/
	if ((sockfd = socket(AF_INET,SOCK_STREAM,0)) < 0) {
		perror("socket()");
		return -1;
	}
	/*connect to the server*/
	if(connect(sockfd,
			(struct sockaddr *)&server_addr,
			sizeof(server_addr)) < 0){
		perror("connect()");
		return -1;
	}
	printf("Successfully connected to %s:%d.\n", inet_ntoa(*server_address), server_port);
	return sockfd;
}

int host_to_address(const char *host, struct in_addr *address) {
	struct hostent *h;
	if ((h=gethostbyname(host)) == NULL) {
		herror("gethostbyname");
		return 1;
	}
	*address = *(struct in_addr *)h->h_addr;
	return 0;
}
