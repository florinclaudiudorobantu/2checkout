/* Dorobantu Florin-Claudiu */

#ifndef SOCK_UTIL_H_
#define SOCK_UTIL_H_

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>

// Default backlog for listen call
#define DEFAULT_LISTEN_BACKLOG 5

// "Shortcut" for struct sockaddr structure
#define SSA struct sockaddr

int tcp_connect_to_server(const char *name, unsigned short port);
int tcp_close_connection(int s);
int tcp_create_listener(unsigned short port, int backlog);
int get_peer_address(int sockfd, char *buf, size_t len);

#endif /* SOCK_UTIL_H_ */
