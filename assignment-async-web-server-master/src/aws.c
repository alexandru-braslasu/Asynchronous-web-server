// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
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
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

// vector de evenimente (mi-a fost de folos cand am extras evenimentele cu io_getevents)
// in el sunt salvate starile in care se gasesc operatiunile (in cazul meu de citire)
// cand extrag un eveniment
struct io_event events[1];

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */
	// functia pentru eroare
	snprintf(conn->send_buffer, sizeof(conn->send_buffer), "%s", "HTTP/1.0 404 Not Found\r\n\r\n");
	int bytes_sent = 1;

	conn->send_len = strlen(conn->send_buffer);
	while (bytes_sent > 0) {
		bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_pos, conn->send_len, 0);
		conn->send_len -= bytes_sent;
		conn->send_pos += bytes_sent;
	}
	conn->state = STATE_404_SENT;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */
	// aici apelez intai functia parse_header pentru a pune in request_path calea catre fisierul de citit
	parse_header(conn);
	// in filename pun request_path-ul cu "." in fata ca sa fie calea corect definita
	snprintf(conn->filename, sizeof(conn->filename), "%s%s", AWS_DOCUMENT_ROOT, conn->request_path + 1);
	// vad daca fisierul exista; daca nu returnez eroare 404 prin apelul functiei pentru eroare
	// in send_buffer pun formatul care trebuie in functie de existenta fisierului
	if (access(conn->filename, F_OK) == -1) {
		conn->state = STATE_SENDING_404;
		connection_prepare_send_404(conn);
		return;
	}
	snprintf(conn->send_buffer, sizeof(conn->send_buffer), "%s", "HTTP/1.0 200 OK\r\n\r\n");
	conn->send_len = strlen(conn->send_buffer);
}



static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */
	// aceasta functie returneaza tipul fisierului (static/dinamic)
	if (strstr(conn->filename, AWS_REL_STATIC_FOLDER))
		return RESOURCE_TYPE_STATIC;
	if (strstr(conn->filename, AWS_REL_DYNAMIC_FOLDER))
		return RESOURCE_TYPE_DYNAMIC;
	return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	/* TODO: Initialize connection structure on given socket. */
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");

	conn->sockfd = sockfd;
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	conn->recv_len = conn->send_len = conn->send_pos = 0;
	conn->state = STATE_INITIAL;

	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
	// aici se fac initializarile necesare pentru operatiile asincrone
	int ret;

	conn->ctx = 0;
	conn->eventfd = eventfd(0, 0);
	conn->state = STATE_ASYNC_ONGOING;
	memset(&conn->iocb, 0, sizeof(conn->iocb));
	memset(conn->send_buffer, '\0', BUFSIZ);
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, MIN(BUFSIZ, conn->file_size), conn->file_pos);
	conn->piocb[0] = &conn->iocb;
	io_set_eventfd(&conn->iocb, conn->eventfd);
	ret = io_setup(1, &conn->ctx);
	DIE(ret < 0, "io_setup");
	ret = io_submit(conn->ctx, 1, conn->piocb);
	DIE(ret < 0, "io_submit");
}

void connection_remove(struct connection *conn)
{
	/* TODO: Remove connection handler. */
	// aici ajung cand se incheie o conexiune
	conn->state = STATE_CONNECTION_CLOSED;
	int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);

	DIE(rc < 0, "w_epoll_remove_ptr");
	close(conn->sockfd);
}

void handle_new_connection(void)
{
	/* TODO: Handle a new connection request on the server socket. */
	// aici sunt initializarile necesare pentru o noua conexiune
	static int connectfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* TODO: Accept new connection. */
	connectfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(connectfd < 0, "accept");

	/* TODO: Set socket to be non-blocking. */
	fcntl(connectfd, F_SETFL, fcntl(connectfd, F_GETFL, 0) | O_NONBLOCK);

	DIE(connectfd == -1, "set non-blocking");

	/* TODO: Instantiate new connection handler. */
	conn = connection_create(connectfd);
	conn->request_parser.data = conn;

	/* TODO: Add socket to epoll. */
	rc = w_epoll_add_ptr_in(epollfd, connectfd, conn);
	DIE(rc < 0, "w_epoll_add_in");

	/* TODO: Initialize HTTP_REQUEST parser. */
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
}

void receive_data(struct connection *conn)
{
	/* TODO: Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	// aici primesc cat de multe date se poate, iar in final intru in starea de request received
	conn->state = STATE_RECEIVING_DATA;
	int rc;
	int bytes_recv = 1;

	while (bytes_recv > 0) {
		bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ, 0);
		conn->recv_len += bytes_recv;
	}
	conn->state = STATE_REQUEST_RECEIVED;
	rc = w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_inout");
}

int connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */
	// aici se deschide fisierul pentru citire si este returnat filedescriptorul
	struct stat st;

	stat(conn->filename, &st);
	conn->file_size = st.st_size;
	conn->fd = open(conn->filename, O_RDONLY);
	if (fstat(conn->fd, &st) < 0)
		return -1;
	return conn->fd;
}

void connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
	// aici inchei un eveniment
	io_destroy(conn->ctx);
	close(conn->eventfd);
}

int parse_header(struct connection *conn)
{
	/* TODO: Parse the HTTP header and extract the file path. */
	/* Use mostly null settings except for on_path callback. */
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};
	// functia aceasta este definita in scheletul de cod si imi extrage informatiile necesare din recv_buffer
	http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);
	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* TODO: Send static data using sendfile(2). */
	// dupa cum sugereaza si numele, aici trimit fisierele statice folosind senfile
	int ret = connection_open_file(conn);
	int bytes_sent = 1;

	if (ret > 0) {
		while (conn->file_size > 0) {
			bytes_sent = sendfile(conn->sockfd, conn->fd, NULL, conn->file_size);
			conn->file_size -= bytes_sent;
		}
	}
	return STATE_NO_STATE;
}

int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	// in aceasta functie intru indiferent de ce tip de fisier am de trimis
	// ramificarea pentru tipurile de fisiere urmand sa fie facuta cateva linii mai jos
	// in functia connection_prepare_send_reply_header fac initializarile necesare
	// si ma folosesc de parser pentru a pune in request_path calea catre fisierul de trimis
	// si alte informatii
	connection_prepare_send_reply_header(conn);
	if (conn->state == STATE_404_SENT)
		return 1;
	int ret;
	int bytes_sent = 1;
	int TOTAL = 0;

	while (bytes_sent > 0 && conn->send_len > 0) {
		bytes_sent = send(conn->sockfd, conn->send_buffer + TOTAL, conn->send_len, 0);
		conn->send_len -= bytes_sent;
		TOTAL += bytes_sent;
	}
	// aici se face ramificarea pentru a trimite in mod diferit fisierele statice si dinamice
	conn->res_type = connection_get_resource_type(conn);
	if (conn->res_type == RESOURCE_TYPE_STATIC)
		connection_send_static(conn);
	else if (conn->res_type == RESOURCE_TYPE_DYNAMIC)
		return connection_send_dynamic(conn);
	conn->state = STATE_DATA_SENT;
	ret = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
	DIE(ret < 0, "w_epoll_update_ptr_in");
	return bytes_sent;
}


int connection_send_dynamic(struct connection *conn)
{
	/* TODO: Read data asynchronously.
	 * Returns 0 on success and -1 on error.
	 */
	// aici verific daca intru pentru prima data in etapa de trimitere a fisierului dinamic
	// caz in care trebuie deschis fisierul pentru scriere in el, iar apoi trebuie creat un
	// nou eveniment prin care sa se citeasca date in timp ce sunt afisati octetii din buffer
	// si in timp ce evenimentul din epoll face verificarile necesare pentru a reveni sa primeasca
	// din nou ceea ce a fost citit pana ce el s-a intors in functia connection_send_dynamic
	if (conn->state != STATE_ASYNC_ONGOING) {
		int ret = connection_open_file(conn);

		DIE(ret < 0, "connection_open_file");
		connection_start_async_io(conn);
		return 0;
	}
	// functia io_getevents extrage un eveniment care ruleaza asincron, in events[0].res
	// aflandu-se starea evenimentului, adica cati octeti a citit pana acum
	int rc = io_getevents(conn->ctx, 1, 1, events, NULL);

	DIE(rc < 0, "io_getevents");
	conn->send_len = events[0].res;
	conn->file_pos += events[0].res;
	int bytes_sent = 1;
	int TOTAL = 0;

	// aici trimit ceea ce am citit in buffer (e posibil sa nu se trimita tot din prima)
	// de aceea trimit pana cand nu mai e nimic de trimis
	while (bytes_sent > 0) {
		bytes_sent = send(conn->sockfd, conn->send_buffer + TOTAL, conn->send_len, 0);
		conn->send_len -= bytes_sent;
		conn->file_size -= bytes_sent;
		TOTAL += bytes_sent;
	}
	// aici ma opresc cand am trimis tot fisierul
	if (conn->file_size == 0) {
		conn->state = STATE_DATA_SENT;
		connection_complete_async_io(conn);
		connection_remove(conn);
		return 0;
	}
	// daca nu am trimis tot fisierul mai citesc date din el si in timp ce citesc se fac
	// alte operatii
	connection_complete_async_io(conn);
	connection_start_async_io(conn);
	return 0;
}


void handle_input(struct connection *conn)
{
	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */
	// daca ma aflu in starea initiala ma duc in functia receive_data pentru a primi noi date
	// altfel daca sunt in starea in care abia ce am trimis un set de date merg din nou in
	// functia receive_data pentru a verifica daca mai sunt date de primit
	// OBS! desi pe ambele cazuri fac acelasi lucru am separat cazurile pentru a se vedea mai
	// clar cazurile pe care merg
	switch (conn->state) {
	case STATE_INITIAL:
		receive_data(conn);
		break;
	case STATE_DATA_SENT:
		receive_data(conn);
		break;
	default:
		break;
	}
}

void handle_output(struct connection *conn)
{
	/* TODO: Handle output information: may be a new valid requests or notification of
	 * completion of an asynchronous I/O operation or invalid requests.
	 */
	// daca ma aflu in starea in care am primit tot ce se putea primi merg in functia
	// de trimitere a datelor
	switch (conn->state) {
	case STATE_REQUEST_RECEIVED:
		connection_send_data(conn);
		break;
	default:
		break;
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	/* TODO: Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */
	// daca prin conexciune au fost trimise datele, atunci inchid conexiunea
	// verific la intrarea si iesirea din handle_client pentru a nu intra
	// din nou in alte functii (ex: handle_input, handle_output)
	if (conn->state == STATE_DATA_SENT || conn->state == STATE_404_SENT)
		connection_remove(conn);
	// daca starea conexiunii este deja STATE_ASYNC_ONGOING, atunci trec direct
	// in functia de trimitere a fisierelor dinamice
	if (conn->state == STATE_ASYNC_ONGOING) {
		connection_send_dynamic(conn);
		return;
	}
	if (event & EPOLLIN)
		handle_input(conn);
	if (event & EPOLLOUT)
		handle_output(conn);
	if (conn->state == STATE_DATA_SENT || conn->state == STATE_404_SENT)
		connection_remove(conn);
}

int main(void)
{
	int rc;

	/* TODO: Initialize asynchronous operations. */

	/* TODO: Initialize multiplexing. */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* TODO: Create server socket. */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	/* TODO: Add server socket to epoll object*/
	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	/* Uncomment the following line for debugging. */
	//dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* TODO: Wait for events. */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/* TODO: Switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			handle_client(rev.events, rev.data.ptr);
		}
	}
	return 0;
}
