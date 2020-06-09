/* Dorobantu Florin-Claudiu */

#include <stdio.h>
#include <libaio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sqlite3.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>

#include "util.h"
#include "server.h"
#include "sock_util.h"
#include "w_epoll.h"

#define SUCCESS 0
#define FAIL -1

#define NUM_OPS 1
#define NUM_USERS 3

#define RATE_LIMITING 3
#define REFRESH_USER_RATE_LIMITING 60

#define GET 0
#define POST 1
#define PUT 2
#define DELETE 3

// Server socket file descriptor
static int listenfd;

// Epoll file descriptor
static int epollfd;

// Connection state
enum connection_state {
	STATE_DATA_RECEIVED,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED
};

// Structure acting as a connection handler
struct connection {
	// Socket
	int sockfd;

	// Buffers for receiving messages and sending messages
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
	size_t send_len;

	// Connection state
	enum connection_state state;

	// Auxiliary file properties
	int fd;
	long file_size;
	char file_path[BUFSIZ];

	// HTTP header
	int http_header_sent;
	size_t http_header_bytes_sent;

	// Eventfd and Async IO
	int efd;
	int num_blocks;
	char agent_buffer[BUFSIZ];

	struct iocb *iocb_file_RD;
	struct iocb **piocb_file_RD;
	struct iocb *iocb_sock_WR;
	struct iocb **piocb_sock_WR;

	// HTTP request
	int http_method_type;
	char http_content[BUFSIZ];
	int http_error;
};

// Authorized users
char user_base64_token[NUM_USERS][BUFSIZ];

// User connections time (refreshed) used for users rate limiting
struct timeval user_connection_time[NUM_USERS];

// Number of user requests (refreshed) used for user rate limiting
int user_num_requests[NUM_USERS];

// Product catalog database
sqlite3* DB;

// Get current time
static char* get_time()
{
	time_t t;
	struct tm tm;
	char *time_buffer;

	t = time(NULL);
	tm = *localtime(&t);
	time_buffer = calloc(1, 20 * sizeof(char));
	DIE(time_buffer == NULL, "calloc");

	// Time format
	sprintf(time_buffer, "%02d-%02d-%d %02d:%02d:%02d", tm.tm_mday,
		tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);

	return time_buffer;
}

// Initialize connection structure on given socket
static struct connection *connection_create(int sockfd)
{
	struct connection *conn;

	conn = calloc(1, sizeof(*conn));
	DIE(conn == NULL, "calloc");

	conn->sockfd = sockfd;

	memset(conn->recv_buffer, 0, BUFSIZ);
	conn->recv_len = 0;

	memset(conn->send_buffer, 0, BUFSIZ);
	conn->send_len = 0;

	conn->fd = -1;
	conn->file_size = 0;
	memset(conn->file_path, 0, BUFSIZ);

	conn->http_header_sent = 0;
	conn->http_header_bytes_sent = 0;

	conn->efd = -1;
	conn->num_blocks = 0;
	memset(conn->agent_buffer, 0, BUFSIZ);

	conn->http_method_type = -1;
	memset(conn->http_content, 0, BUFSIZ);
	conn->http_error = 0;

	return conn;
}

// Remove connection handler
static void connection_remove(struct connection *conn)
{
	int ret;

	if (conn->fd != -1) {
		close(conn->fd);
		ret = remove(conn->file_path);
		DIE(ret < 0, "remove");
	}

	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;

	if (conn->efd != -1)
		close(conn->efd);

	free(conn);
}

// Handle a new connection request on the server socket
static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen;
	struct sockaddr_in addr;
	struct connection *conn;
	int rc, flags;

	addrlen = sizeof(struct sockaddr_in);

	// Accept new connection
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	// Mark nonblocking socket
	flags = fcntl(sockfd, F_GETFL, 0);
	fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

	// Instantiate new connection handler
	conn = connection_create(sockfd);

	// Add socket to epoll
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_in");
}

// Receive message on socket
// Store message in recv_buffer in struct connection
static enum connection_state receive_message(struct connection *conn)
{
	ssize_t bytes_recv;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	DIE(rc < 0, "get_peer_address");

	bytes_recv = recv(conn->sockfd,
			conn->recv_buffer + conn->recv_len,
			BUFSIZ, 0);

	// Error in communication
	if (bytes_recv < 0)
		goto remove_connection;

	// Connection closed
	if (bytes_recv == 0)
		goto remove_connection;

	conn->recv_len += bytes_recv;

	if (strstr(conn->recv_buffer, "\r\n\r\n") != NULL) {
		rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_update_ptr_out");
	}

	conn->state = STATE_DATA_RECEIVED;

	return STATE_DATA_RECEIVED;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	// Remove current connection
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

// Wait for asynchronous I/O operations
static int wait_aio(io_context_t ctx, int nops)
{
	struct io_event *events;
	int rc;

	// Alloc structure
	events = calloc(1, nops * sizeof(struct io_event));
	DIE(events == NULL, "calloc");

	// Wait for async operations to finish
	rc = io_getevents(ctx, nops, nops, events, NULL);
	DIE(rc < 0, "io_getevents");

	free(events);

	return rc;
}

// Write data asynchronously
void do_aio_async(struct connection *conn)
{
	int rc, n_aio_ops, block_size, i;
	io_context_t ctx = 0;

	// Set up eventfd notification
	conn->efd = eventfd(0, 0);
	DIE(conn->efd < 0, "eventfd");

	// Number of IO control blocks
	conn->num_blocks = conn->file_size / BUFSIZ;
	if (conn->file_size % BUFSIZ != 0)
		conn->num_blocks++;

	// Allocate iocb and piocb for operations with file and socket
	conn->iocb_file_RD = calloc(1, conn->num_blocks * sizeof(struct iocb));
	DIE(conn->iocb_file_RD == NULL, "calloc");

	conn->piocb_file_RD = calloc(1, conn->num_blocks *
					sizeof(struct iocb *));
	DIE(conn->piocb_file_RD == NULL, "calloc");

	conn->iocb_sock_WR = calloc(1, conn->num_blocks * sizeof(struct iocb));
	DIE(conn->iocb_sock_WR == NULL, "calloc");

	conn->piocb_sock_WR = calloc(1, conn->num_blocks *
					sizeof(struct iocb *));
	DIE(conn->piocb_sock_WR == NULL, "calloc");

	// Setup aio context
	rc = io_setup(NUM_OPS, &ctx);
	DIE(rc < 0, "io_submit");

	block_size = conn->file_size < BUFSIZ ? conn->file_size : BUFSIZ;

	for (i = 0; i < conn->num_blocks; i++) {
		// Read from file to buffer
		io_prep_pread(&(conn->iocb_file_RD[i]), conn->fd,
			conn->agent_buffer, block_size, i * block_size);
		conn->piocb_file_RD[i] = &(conn->iocb_file_RD[i]);

		// Configure aio with eventfd
		io_set_eventfd(&(conn->iocb_file_RD[i]), conn->efd);

		// Submit aio
		n_aio_ops = io_submit(ctx, NUM_OPS, &(conn->piocb_file_RD[i]));
		DIE(n_aio_ops < 0, "io_submit");

		// Wait for completion
		rc = 0;
		while (rc < NUM_OPS)
			rc = wait_aio(ctx, NUM_OPS);

		// Write from buffer to socket
		io_prep_pwrite(&(conn->iocb_sock_WR[i]), conn->sockfd,
			conn->agent_buffer, block_size, 0);
		conn->piocb_sock_WR[i] = &(conn->iocb_sock_WR[i]);

		// Configure aio with eventfd
		io_set_eventfd(&(conn->iocb_sock_WR[i]), conn->efd);

		// Submit aio
		n_aio_ops = io_submit(ctx, NUM_OPS, &(conn->piocb_sock_WR[i]));
		DIE(n_aio_ops < 0, "io_submit");

		// Wait for completion
		rc = 0;
		while (rc < NUM_OPS)
			rc = wait_aio(ctx, NUM_OPS);
	}

	// Destroy aio context
	io_destroy(ctx);

	free(conn->iocb_file_RD);
	free(conn->piocb_file_RD);
	free(conn->iocb_sock_WR);
	free(conn->piocb_sock_WR);
}

// Send message on socket
// Store message in send_buffer in struct connection
static enum connection_state send_message(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	DIE(rc < 0, "get_peer_address");

	if (conn->http_header_sent == 0) {
		bytes_sent = send(conn->sockfd,
			conn->send_buffer + conn->http_header_bytes_sent,
			conn->send_len - conn->http_header_bytes_sent, 0);

		// Error in communication
		if (bytes_sent < 0)
			goto remove_connection;

		// Connection closed
		if (bytes_sent == 0)
			goto remove_connection;

		conn->http_header_bytes_sent += bytes_sent;

		// Sending header is complete
		if (conn->http_header_bytes_sent == conn->send_len)
			conn->http_header_sent = 1;

		conn->state = STATE_DATA_SENT;

		return STATE_DATA_SENT;
	}

	// HTTP error
	if (conn->http_error == FAIL)
		goto remove_connection;

	// Send response
	do_aio_async(conn);

	conn->state = STATE_DATA_SENT;

	goto remove_connection;

	return STATE_DATA_SENT;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	// Remove current connection
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

// Check bad request
static int check_bad_request(struct connection *conn)
{
	char *token, *pch;
	char buffer[BUFSIZ];

	strcpy(buffer, conn->recv_buffer);

	// HTTTP header
	token = strtok(buffer, "\n");
	if (strstr(token, "HTTP/1.") == NULL)
		goto bad_request;
	if (strstr(token, "GET") == NULL && strstr(token, "POST") == NULL &&
			strstr(token, "PUT") == NULL && strstr(token, "DELETE") == NULL)
		goto bad_request;

	// Save method type
	if (strncmp(token, "GET", 3) == 0)
		conn->http_method_type = GET;
	else if (strncmp(token, "POST", 4) == 0)
		conn->http_method_type = POST;
	else if (strncmp(token, "PUT", 3) == 0)
		conn->http_method_type = PUT;
	else if (strncmp(token, "DELETE", 6) == 0)
		conn->http_method_type = DELETE;

	token = strtok(NULL, "\n");
	if (strstr(token, "Host") == NULL)
		goto bad_request;

	token = strtok(NULL, "\n");
	if (strstr(token, "User-Agent") == NULL)
		goto bad_request;

	token = strtok(NULL, "\n");
	if (strstr(token, "Accept") == NULL)
		goto bad_request;

	token = strtok(NULL, "\n");
	if (strstr(token, "Authorization") == NULL)
		goto bad_request;

	token = strtok(NULL, "\n");
	if (strstr(token, "Connection") == NULL)
		goto bad_request;

	// Newline
	token = strtok(NULL, "\n");
	if (strcmp(token, "\r") != 0)
		goto bad_request;

	// HTTP content
	token = strtok(NULL, "\n");
	if (strcmp(token, "\r") == 0)
		goto bad_request;

	// Save content
	strcpy(conn->http_content, token);

	// End of request
	token = strtok(NULL, "\n");
	if (strstr(token, "end") == NULL)
		goto bad_request;

	// Newline
	token = strtok(NULL, "\n");
	if (strcmp(token, "\r") != 0)
		goto bad_request;

	return SUCCESS;

bad_request:
	pch = strstr(conn->recv_buffer, "HTTP");

	if (pch == NULL)
		sprintf(conn->send_buffer, "HTTP/1.1 400 Bad Request\r\n\r\n");
	else
		sprintf(conn->send_buffer, "HTTP/1.%c 400 Bad Request\r\n\r\n",
				*(pch + 7));
	conn->send_len = strlen(conn->send_buffer);

	conn->http_error = FAIL;

	return FAIL;
}

// Check unauthorized
static int check_unauthorized(int *user_index, struct connection *conn)
{
	char *p_base64_token, *pch;
	char base64_token[BUFSIZ];
	int i;

	p_base64_token = strstr(conn->recv_buffer, "Authorization: Basic ");

	// Check user token
	for (i = 0; i < NUM_USERS; i++) {
		strcpy(base64_token, user_base64_token[i]);
		strcat(base64_token, "\r\n");
		if (strncmp(p_base64_token + 21, base64_token,
				strlen(base64_token)) == 0) {
			*user_index = i;
			// Set number of requests and connection time
			user_num_requests[i]++;
			if (user_connection_time[i].tv_sec == 0)
				gettimeofday(&user_connection_time[i], NULL);

			return SUCCESS;
		}
	}

	pch = strstr(conn->recv_buffer, "HTTP");

	sprintf(conn->send_buffer, "HTTP/1.%c 401 Unauthorized\r\n\r\n",
			*(pch + 7));
	conn->send_len = strlen(conn->send_buffer);

	conn->http_error = FAIL;

	return FAIL;
}

// Check too many requests
static int check_too_many_requests(int user_index, struct connection *conn)
{
	char *pch;
	struct timeval current_time;

	gettimeofday(&current_time, NULL);

	// Check rate limiting
	if (current_time.tv_usec - user_connection_time[user_index].tv_usec <= 10
			|| (double)(current_time.tv_sec -
			user_connection_time[user_index].tv_sec)
			/ user_num_requests[user_index] >= RATE_LIMITING) {
		if (current_time.tv_sec - user_connection_time[user_index].tv_sec
				> REFRESH_USER_RATE_LIMITING) {
			// Refresh user rate limiting
			user_num_requests[user_index] = 1;
			gettimeofday(&user_connection_time[user_index], NULL);
		}
		return SUCCESS;
	}

	pch = strstr(conn->recv_buffer, "HTTP");

	sprintf(conn->send_buffer, "HTTP/1.%c 429 Too Many Requests\r\n\r\n",
			*(pch + 7));
	conn->send_len = strlen(conn->send_buffer);

	conn->http_error = FAIL;

	return FAIL;
}

// Check not found
static int check_not_found(struct connection *conn)
{
	char *token, *pch;
	char buffer[BUFSIZ], pathname[BUFSIZ];

	strcpy(buffer, conn->recv_buffer);

	token = strtok(buffer, " ");
	token = strtok(NULL, " ");

	sprintf(pathname, "%s%s", SERVER_DOCUMENT_ROOT, token);

	// Check file path
	if (access(pathname, F_OK) != -1)
		return SUCCESS;

	pch = strstr(conn->recv_buffer, "HTTP");

	sprintf(conn->send_buffer, "HTTP/1.%c 404 Not Found\r\n\r\n",
			*(pch + 7));
	conn->send_len = strlen(conn->send_buffer);

	conn->http_error = FAIL;

	return FAIL;
}

// Sqlite3 callback function (write results from database)
static int callback(void *data, int argc, char **argv, char **azColName)
{
	int fd, i;
	char buffer[BUFSIZ];

	fd = *(int *)data;

	for (i = 0; i < argc; i++) {
		sprintf(buffer, "%s = %s\n", azColName[i],
			argv[i] ? argv[i] : "NULL");
		write(fd, buffer, strlen(buffer));
	}

	write(fd, "\n", 1);

	return 0;
}

// GET operation
static void get_operation(struct connection *conn)
{
	char *id;
	char sql[BUFSIZ], response[BUFSIZ];
	char *messagge_error = NULL;

	id = strtok(conn->http_content, "\r\n");

	if (strcmp(id, "all") == 0)
		sprintf(sql, "SELECT * FROM product_catalog;");
	else
		sprintf(sql, "SELECT * FROM product_catalog "
					"WHERE Id = %s;", id);

	sqlite3_exec(DB, sql, callback, (void *)&conn->fd, &messagge_error);

	if (messagge_error == NULL)
		sprintf(response, "GET done (success).\n\n");
	else
		sprintf(response, "GET done (%s).\n\n", messagge_error);

	write(conn->fd, response, strlen(response));

	sqlite3_free(messagge_error);
}

// POST operation
static void post_operation(struct connection *conn)
{
	char *id, *name, *price, *category, *time_buffer;
	char sql[BUFSIZ], response[BUFSIZ];
	char *messagge_error = NULL;

	id = strtok(conn->http_content, ",");
	name = strtok(NULL, ",");
	price = strtok(NULL, ",");
	category = strtok(NULL, "\r\n");

	time_buffer = get_time();

	sprintf(sql, "INSERT INTO product_catalog VALUES(%s, '%s', %s, '%s',"
		"'%s', '%s')", id, name, price, category, time_buffer, time_buffer);

	sqlite3_exec(DB, sql, NULL, 0, &messagge_error);

	if (messagge_error == NULL)
		sprintf(response, "POST done (success).\n\n");
	else
		sprintf(response, "POST done (%s).\n\n", messagge_error);

	write(conn->fd, response, strlen(response));

	sqlite3_free(messagge_error);
	free(time_buffer);
}

// PUT operation
static void put_operation(struct connection *conn)
{
	char *id, *name, *price, *category, *time_buffer;
	char sql[BUFSIZ], response[BUFSIZ];
	char *messagge_error = NULL;

	id = strtok(conn->http_content, ",");
	name = strtok(NULL, ",");
	price = strtok(NULL, ",");
	category = strtok(NULL, "\r\n");

	time_buffer = get_time();

	sprintf(sql, "UPDATE product_catalog "
		"SET Name = '%s', Price = %s, Category = '%s', " "UpdatedDate = '%s' "
		"WHERE Id = %s;", name, price, category, time_buffer, id);

	sqlite3_exec(DB, sql, NULL, 0, &messagge_error);

	if (messagge_error == NULL)
		sprintf(response, "PUT done (success).\n\n");
	else
		sprintf(response, "PUT done (%s).\n\n", messagge_error);

	write(conn->fd, response, strlen(response));

	sqlite3_free(messagge_error);
	free(time_buffer);
}

// DELETE operation
static void delete_operation(struct connection *conn)
{
	char *id;
	char sql[BUFSIZ], response[BUFSIZ];
	char *messagge_error = NULL;

	id = strtok(conn->http_content, "\r\n");

	sprintf(sql, "DELETE FROM product_catalog "
				"WHERE Id = %s;", id);

	sqlite3_exec(DB, sql, NULL, 0, &messagge_error);

	if (messagge_error == NULL)
		sprintf(response, "DELETE done (success).\n\n");
	else
		sprintf(response, "DELETE done (%s).\n\n", messagge_error);

	write(conn->fd, response, strlen(response));

	sqlite3_free(messagge_error);
}

// Handle a client request on a client connection
static void handle_client_request(struct connection *conn)
{
	enum connection_state ret_state;
	char *pch;
	int user_index = -1, ret;

	ret_state = receive_message(conn);

	if (ret_state == STATE_CONNECTION_CLOSED)
		return;

	// Wait for message
	if (strstr(conn->recv_buffer, "end\r\n\r\n") == NULL)
		return;

	// Print request
	printf("%s", conn->recv_buffer);

	ret = check_bad_request(conn);
	if (ret != SUCCESS)
		return;

	ret = check_unauthorized(&user_index, conn);
	if (ret != SUCCESS)
		return;

	ret = check_too_many_requests(user_index, conn);
	if (ret != SUCCESS)
		return;

	ret = check_not_found(conn);
	if (ret != SUCCESS)
		return;

	// Auxiliary file sent as response
	sprintf(conn->file_path, "request_response_to_%s.txt",
		user_base64_token[user_index]);

	conn->fd = open(conn->file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	DIE(conn->fd < 0, "open");

	if (conn->http_method_type == GET)
		get_operation(conn);
	else if (conn->http_method_type == POST)
		post_operation(conn);
	else if (conn->http_method_type == PUT)
		put_operation(conn);
	else if (conn->http_method_type == DELETE)
		delete_operation(conn);

	if (ret != SUCCESS)
		return;

	conn->file_size = lseek(conn->fd, 0, SEEK_CUR);
	lseek(conn->fd, 0, SEEK_SET);

	pch = strstr(conn->recv_buffer, "HTTP");

	sprintf(conn->send_buffer, "HTTP/1.%c 200 OK\r\n"
		"Content-Length: %ld\r\n"
		"Connection: close\r\n"
		"Content-Type: text/plain\r\n"
		"\r\n", *(pch + 7), conn->file_size);

	conn->send_len = strlen(conn->send_buffer);
}

// Initialize authorized users
static void initialize_authorized_users()
{
	int i;

	for (i = 0; i < NUM_USERS; i++) {
		if (i == 0)
			// user = doro and password = pass_doro
			strcpy(user_base64_token[i], "ZG9ybzpwYXNzX2Rvcm8=\0");
		else if (i == 1)
			// user = claudiu and password = pass_claudiu
			strcpy(user_base64_token[i], "Y2xhdWRpdTpwYXNzX2NsYXVkaXU=\0");
		else if (i == 2)
			// user = florin and password = pass_florin
			strcpy(user_base64_token[i], "ZmxvcmluOnBhc3NfZmxvcmlu\0");

		user_connection_time[i].tv_sec = 0;
		user_num_requests[i] = 0;
	}
}

int main(void)
{
	int rc, ret = 0;
	char sql[BUFSIZ];

	// Init multiplexing
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	// Create server socket
	listenfd = tcp_create_listener(SERVER_LISTEN_PORT,
				DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	// Authorize users
	initialize_authorized_users();

	// Create or open product catalog database
	ret = sqlite3_open(SERVER_REL_DATABASE, &DB);
	DIE(ret != SQLITE_OK, "sqlite3_open");

	// Create table product_catalog if not exists 
	sprintf(sql, "CREATE TABLE IF NOT EXISTS product_catalog"
					"("
						"Id NUMBER(5) PRIMARY KEY,"
						"Name VARCHAR2(30),"
						"Price NUMBER(5,2),"
						"Category VARCHAR2(30),"
						"CreatedDate DATE,"
						"UpdatedDate DATE"
					");");

	ret = sqlite3_exec(DB, sql, NULL, 0, NULL);
	DIE(ret != SQLITE_OK, "sqlite3_exec");

	// Server main loop
	while (1) {
		struct epoll_event rev;

		// Wait for events
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		// Switch event types; consider
		// - new connection requests (on server socket)
		// - socket communication (on connection sockets)
		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			if (rev.events & EPOLLIN)
				handle_client_request(rev.data.ptr);
			if (rev.events & EPOLLOUT)
				send_message(rev.data.ptr);
		}
	}

	sqlite3_close(DB);

	return 0;
}
