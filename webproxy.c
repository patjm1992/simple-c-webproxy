#include <sys/socket.h> 
#include <stdio.h> 
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <netdb.h> 
#include <sys/types.h> 
#include <string.h> 
#include <netinet/in.h> 
#include <arpa/inet.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>


#define DEBUG 0
#define CHUNKSIZE 512

char recv_buf[65000];

int TCP_PORT; // Given as command line argument

sem_t mutex;
int totalsize;

struct Http_Req {
	char *method;
	char *uri;
	char *http_ver;
	char *host;
	char entire_req[512];
};

void error_die(char *msg);

/**
 * Given the request from the client, parse it for various fields, store in an
 * Http_Req struct and return the struct. I don't really know if I need to parse it
 * for every single field, but I do anyway.
 */ 
struct Http_Req * parse_http(char *req_buf)
{
	char *request_line;
	char *host_line;

	struct Http_Req *req = (struct Http_Req *) malloc(sizeof(struct Http_Req));

	memcpy(req->entire_req,  req_buf, sizeof(req->entire_req));
		
	char *token = NULL;
	token = strtok(req_buf, "\n");
		
	request_line = token;
	token = strtok(NULL, "\n");
	host_line = token;

	token = strtok(request_line, " ");
	req->method = token;
	token = strtok(NULL, " ");
	req->uri = token;
	token = strtok(NULL, " ");
	req->http_ver = token;

	token  = strtok(host_line, " ");
	token = strtok(NULL, " ");
	req->host = token;
		
	printf("\nREQUEST RECEIVED:\n-------------------\n%s", req->entire_req);
	fflush(stdout);

	return req;
}

/**
 * Send the contents of recv_buf (filled by forward_request()) to the client (browser).
 */ 
void forward_response(int client_socket)
{
	int n_bytes;

	if (DEBUG) {
		printf("Sending to browser:\n--------------------- \n%s", recv_buf);
	}

	sem_wait(&mutex);
	recv_buf[totalsize] = '\0';
	
	while ((n_bytes = send(client_socket, recv_buf, strlen(recv_buf), 0)) < totalsize);
		
	
	printf("\n%d bytes sent to client.\n", n_bytes);
	sem_post(&mutex);
} 

/**
 * Given the HTTP_Request structure, form a GET request and send to the origin server.
 */ 
void forward_request(struct Http_Req *req)
{
	char send_buf[1024];

	int n_bytes;
	int sock = socket(AF_INET, SOCK_STREAM, 0);	
	struct sockaddr_in sa_origin;

	sa_origin.sin_family = AF_INET;
	sa_origin.sin_port = htons(80);

	req->host[strlen(req->host) - 1] = '\0';
	
	struct hostent *origin = gethostbyname(req->host);
	
	if (origin == NULL) {
		error_die("gethostbyname");
	}
	
	memcpy(&sa_origin.sin_addr.s_addr, origin->h_addr, origin->h_length);

	socklen_t len = sizeof(sa_origin);
	
	if ((connect(sock, (struct sockaddr *) &sa_origin, len)) < 0) {
		error_die("connect");
	}

	/* Fill buffer with the new HTTP object */
	sprintf(send_buf, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", req->uri, req->host);
	
	printf("Sending the following HTTP object: \n%s", send_buf);

	while ((n_bytes = send(sock, send_buf, strlen(send_buf), 0)) < strlen(send_buf));
	
	printf("Sent %d bytes.\n", n_bytes);
	fflush(stdout);

	sem_wait(&mutex);
	
	char *ptr = recv_buf;
	char chunk[512];
	totalsize = 0;
	/* Recv the response from the origin server */
	while ((n_bytes = recv(sock, chunk, CHUNKSIZE, 0)) > 0) {
		memcpy(ptr, chunk, CHUNKSIZE);
		totalsize += n_bytes;
		fflush(stdout);
		ptr += CHUNKSIZE;
		

		if (n_bytes == 0) {  
			break;
		}
	}

	sem_post(&mutex);
	
	printf("\nGot %d bytes from origin.\n", totalsize);

	if (DEBUG) {
		printf("%s", recv_buf);
	}

	fflush(stdout);
}


/**
 * Thread -- calls routines to parse HTTP, send a new HTTP object to the origin web
 * server, and then send the response to the client browser.
 */
void *proxy_thread(void *param)
{
	int client_sock = (int) param;
	int n_bytes = 0;
	char req_buf[512];

	memset(&req_buf[0], 0, sizeof(req_buf));

	/* Try to receive a request */
	while ((n_bytes = recv(client_sock, req_buf, sizeof(req_buf), 0))) {

		struct Http_Req *req = parse_http(req_buf); // Parse the HTTP
		forward_request(req);                       // Forward the request to origin
		forward_response(client_sock);              // Send response to browser
		
	}

	return NULL; // avoid warnings
}


int main(int argc, char **argv)
{
	struct sockaddr_in sa;
	
	socklen_t len;
	pthread_t tid;

	sem_init(&mutex, 0, 1);

	if (argc < 2) {
		printf("No port number provided.\n");
		exit(1);
	}

	TCP_PORT = atoi(argv[1]);

	/* Initialize TCP sockets */
	int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	int accept_sock = socket(AF_INET, SOCK_STREAM, 0);

	/* Fill sockaddr_in structure */
	sa.sin_family = AF_INET;
	sa.sin_port = htons(TCP_PORT);
	

	 if (inet_pton(AF_INET, "127.0.0.1", &(sa.sin_addr)) <= 0) { 
	 	error_die("inet_pton"); 
	 } 

	/* Bind */
	if (bind(listen_sock, (struct sockaddr *) &sa, sizeof(sa)) == -1) {
		error_die("bind");
	}

	len = sizeof(sa);

	while (1) {

		printf("TCP: Waiting for incoming requests...\n");
	
		/* Listen */
		if ((listen(listen_sock, 10)) < 0) {
			error_die("listen");
		}

		accept_sock = accept(listen_sock, (struct sockaddr *) &sa, &len);

		if (accept_sock > -1) {
			printf("\n****************************************************\n");
			printf("Connection with client %s:%d accepted.\n", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
			printf("****************************************************\n\n");

                        /* Connection accepted -- spawn a new thread for it */
			pthread_create(&tid, NULL, (void *) &proxy_thread, (void *) accept_sock);
		
		}  else if (accept_sock == -1) {
			error_die("accept");
		}
		

	}

	return 0;
}






void error_die(char *msg)
{
	perror(msg);
	exit(1);
}
