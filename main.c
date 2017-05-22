#include <err.h>
#include <string.h>	/* memset */
#include <sys/types.h>	/* socket... */
#include <sys/socket.h>	/* socket... */
#include <netinet/in.h>	/* sockaddr_in */
#include <stdio.h>	/* printf */
#include <unistd.h>	/* close */
#include <stdlib.h>	/* calloc */
#include <netdb.h>	/* getnameinfo */
#include <ctype.h>	/* isalnum */

struct connection_list;
int main(int argc, char *argv[]);
void fd_set_all(struct connection_list*, fd_set*, fd_set*, int *maxfd);
int handle_read(struct connection_list *item);
int peek_char(struct connection_list *connection, char *c, int *pos);
char *accept_buffer(struct connection_list *connection, int nchars);	// must be freed
void skip_buffer(struct connection_list *connection, int nchars);
void add_output_string(struct connection_list *connection, const char *string);
void add_output(struct connection_list *connection, const char *buf, size_t len);

struct connection_list {
	int sockfd;
	char readbuf[1024];
	char writebuf[1024];
	int readpos, readend;
	int writepos, writeend;
	//char linepos;
	char *current_tag;
	enum {
		CMD_NONE = 0,
		CMD_UNKNOWN,
		CMD_CAPABILITY,
		CMD_LOGOUT
	} current_cmd;
	int quit_after_write;
	struct connection_list *next;
};

int main(int argc, char *argv[])
{
	struct sockaddr_in sin;
	fd_set fdset_in, fdset_out;
	int maxfd = 0;
	struct connection_list *connections = NULL;
	struct connection_list **pitem;
	int srvfd = socket(AF_INET, SOCK_STREAM, 0);

	if (srvfd < 0) {
		err(1, "server socket");
	}

	memset(&sin, 0, sizeof sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(143);
	sin.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(srvfd, (struct sockaddr*)&sin, sizeof sin) < 0) {
		err(1, "bind server socket");
	}

	if (listen(srvfd, 5) < 0) {
		err(1, "listen server socket");
	}

	while(1) {
		FD_ZERO(&fdset_out);
		FD_ZERO(&fdset_in);
		FD_SET(srvfd, &fdset_in);
		FD_SET(STDIN_FILENO, &fdset_in);
		maxfd = srvfd;

		fd_set_all(connections, &fdset_in, &fdset_out, &maxfd);

		printf("Select... (maxfd: %d)\n", maxfd);
		if (select(maxfd + 1, &fdset_in, &fdset_out, NULL, NULL) < 0) {
			err(1, "select");
		}
		printf("Selected.\n");

		if (FD_ISSET(srvfd, &fdset_in)) {
			int clientfd;
			struct sockaddr_in addr;
			socklen_t addr_len;
			clientfd = accept(srvfd, (struct sockaddr*)&addr, &addr_len);
			if (clientfd < 0) {
				warn("accept");
				continue;
			} else {
				char hostname[256];
				getnameinfo((struct sockaddr*)&addr, addr_len, hostname, sizeof hostname, NULL, 0, 0);
				printf("New connection from %s\n", hostname);

				struct connection_list *connection = calloc(1, sizeof(struct connection_list));
				if (!connection) {
					err(1, "calloc");
				}
				connection->sockfd = clientfd;
				connection->next = connections;
				connections = connection;

				add_output_string(connection, "* OK\r\n");
			}
		}

		if (FD_ISSET(STDIN_FILENO, &fdset_in)) {
			char c;
			read(STDIN_FILENO, &c, 1);
			if (c == 'q') {
				break;
			}
			printf("Input? %c\n", c);
		}

		struct connection_list *item;
		for (pitem = &connections; (item = *pitem); pitem = &item->next) {
			if (FD_ISSET(item->sockfd, &fdset_in)) {
				printf("Can read from %d\n", item->sockfd);
				if (handle_read(item) == -2) {
					*pitem = item->next;
					free(item);
					break;	//continue;
				}
			}

			if (FD_ISSET(item->sockfd, &fdset_out)) {
				if (item->quit_after_write && item->writeend == item->writepos) {
					close(item->sockfd);
					*pitem = item->next;
					free(item);
					break;	//continue;	// TODO use-after-free in for condition?!!
				}

				int writeend = item->writeend;
				if (writeend < item->writepos) {
					writeend = sizeof(item->writebuf);
				}
				int nwritten = write(item->sockfd, &item->writebuf[item->writepos], writeend - item->writepos);
				if (nwritten < 0) {
					warn("write");
					*pitem = item->next;
					free(item);
					break;	//continue;
				} else {
					printf("Written %d of %d to %d: %.*s\n", nwritten, writeend - item->writepos, item->sockfd, nwritten, &item->writebuf[item->writepos]);
					item->writepos += nwritten;
					item->writepos %= sizeof(item->writebuf);
				}
			}
		}
	}

	close(srvfd);

	return 0;
}

void fd_set_all(struct connection_list *connections, fd_set *fdset_in, fd_set *fdset_out, int *maxfd)
{
	struct connection_list *item;
	for (item = connections; item; item = item->next) {
		FD_SET(item->sockfd, fdset_in);
		if (item->writepos != item->writeend || item->quit_after_write) {
			printf("Adding socket %d for writing\n", item->sockfd);
			FD_SET(item->sockfd, fdset_out);
		} else {
			printf("NOT Adding socket %d for writing\n", item->sockfd);
		}
		if (item->sockfd > *maxfd) {
			*maxfd = item->sockfd;
		}
	}
}

int handle_read(struct connection_list *item)
{
	int bufend = sizeof(item->readbuf);
	if (item->readend < item->readpos) {
		bufend = item->readpos;
	}
	int nread = read(item->sockfd, &item->readbuf[item->readend], bufend - item->readend);

	if (nread == 0) {
		printf("%d disconnected.\n", item->sockfd);
		close(item->sockfd);
		return -2;
	}

	if (nread < 0) {
		warn("read");
		close(item->sockfd);
		return -2;
	}

	printf("RECEIVED %.*s\n", nread, &item->readbuf[item->readend]);

	item->readend += nread;
	item->readend %= sizeof(item->readbuf);

	int pos = 0;
	char c;
	while(1) {
		if (!peek_char(item, &c, &pos)) {
			// No data yet
			return 0;
		}

		if (c == '\r') {
			skip_buffer(item, 1);
			pos = 0;
			printf("Command complete! (tag %s)\n", item->current_tag);
			if (!item->current_tag) {
				add_output_string(item, "* ERR MISSING TAG\r\n");
				continue;
			}
			switch(item->current_cmd) {
				case CMD_NONE:
					//errx(1, "Internal error\n");
					add_output_string(item, item->current_tag);
					add_output_string(item, " ERR MISSING COMMAND\r\n");
					break;
				case CMD_UNKNOWN:
					printf("TODO unknown command. send reply\n");
					add_output_string(item, item->current_tag);
					add_output_string(item, " ERR COMMAND UNKNOWN\r\n");
					break;
				case CMD_CAPABILITY:
					add_output_string(item, item->current_tag);
					add_output_string(item, " OK\r\n");
					break;
				case CMD_LOGOUT:
					add_output_string(item, item->current_tag);
					add_output_string(item, " OK BYE\r\n");
					item->quit_after_write = 1;
					break;
			}
			if (item->current_tag) {
				free(item->current_tag);
				item->current_tag = NULL;
			}
			item->current_cmd = CMD_NONE;
			continue;
		}

		if (isspace(c)) {
			printf("SKIP SPACE\n");
			// skip
			skip_buffer(item, pos);
			pos = 0;
			continue;
		}

		if (!item->current_tag) {
			printf("READ TAG...\n");
			// this is a tag

			if (isalnum(c)) {
				// at least one character
				while (peek_char(item, &c, &pos)) {
					if (isalnum(c)) {
						// OK
					} else {
						pos--;
						// Should be a space char?
						//printf("TAG LEN: %d\n", pos);
						item->current_tag = accept_buffer(item, pos);
						pos = 0;
						//printf("TAG: '%s'\n", item->current_tag);
						break;
					}
				}
			} else {
				printf("Line started without alphanumeric: '%c'\n", c);
				err(1, "?");
			}
			continue;
		} else if(item->current_cmd == CMD_NONE) {
			printf("READ COMMAND\n");
			// read command
			if (isalpha(c)) {
				// At least one letter
				while (peek_char(item, &c, &pos)) {
					if (isalpha(c)) {
						// OK
					} else {
						pos--;
						char *keyword = accept_buffer(item, pos);
						pos = 0;
						printf("KEYWORD: '%s'\n", keyword);

						if (strcmp(keyword, "CAPABILITY") == 0) {
							item->current_cmd = CMD_CAPABILITY;
						} else if (strcmp(keyword, "LOGOUT") == 0) {
							item->current_cmd = CMD_LOGOUT;
						} else {
							item->current_cmd = CMD_UNKNOWN;
						}

						free(keyword);
						break;
					}
				}	
			}
			continue;
		}

		printf("Should not be here! - Char is '%c'\n", c);
	}
	return 0;
}

int peek_char(struct connection_list *connection, char *c, int *pos)
{
	int readpos = (connection->readpos + *pos) % sizeof(connection->readbuf);
	if (readpos == connection->readend) {
		printf("Not peeked anything\n");
		return 0;
	}

	*c = connection->readbuf[readpos];
	//printf("PEEKED '%c'\n", *c);
	//sleep(1);
	(*pos)++;
	return 1;
}

char *accept_buffer(struct connection_list *connection, int nchars)
{
	char *result = malloc(nchars + 1);
	// TODO add check if nchars > readend-readpos
	if (connection->readend >= connection->readpos) {
		printf("Accepting %d chars from buffer [%.*s]\n", nchars, (int)sizeof(connection->readbuf), connection->readbuf);
		memcpy(result, &connection->readbuf[connection->readpos], nchars);
	} else {
		int part1 = sizeof(connection->readbuf) - connection->readpos;
		memcpy(result, &connection->readbuf[connection->readpos], sizeof(connection->readbuf) - connection->readpos);
		memcpy(result + part1, connection->readbuf, nchars - part1);
	}
	result[nchars] = '\0';
	connection->readpos += nchars;
	connection->readpos %= sizeof(connection->readbuf);
	//connection->linepos += nchars;	// TODO wrap?
	return result;
}

void skip_buffer(struct connection_list *connection, int nchars)
{
	printf("Skipping %d chars\n", nchars);
	connection->readpos += nchars;
	connection->readpos %= sizeof(connection->readbuf);
	//connection->linepos += nchars;	// TODO wrap?
}

// TODO what if ring buffer is full?
void add_output_string(struct connection_list *connection, const char *string)
{
	if (!string) {
		warnx("Internal error: add null string?!");
		return;
	}
	add_output(connection, string, strlen(string));
}

void add_output(struct connection_list *connection, const char *buf, size_t len)
{
	if (connection->writepos == connection->writeend) {
		//connection->writepos = 0;
		//connection->writeend = 0;
	}

	if (len > sizeof(connection->writebuf)) {
		err(1, "Can not write buffers larger than %lu\n", sizeof(connection->writebuf));
	}

	// TODO wrap ring buffer!!!!
	if (connection->writeend + len > sizeof(connection->writebuf)) {
		err(1, "Ring buffer wrapping not yet implemented!!!\n");
	}
	memcpy(&connection->writebuf[connection->writeend], buf, len);
	connection->writeend += len;
}
