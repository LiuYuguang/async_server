#ifndef _ASYNC_SERVER_
#define _ASYNC_SERVER_

#include <stddef.h>      //for size_t
#include <sys/types.h>   //for ssize_t
#include <netinet/in.h>  //for struct sockaddr socklen_t

typedef struct async_server_s async_server_t;

async_server_t* server_create();
ssize_t server_set_id_file(async_server_t* server,const char *file_name);
ssize_t server_set_log_file(async_server_t* server,const char *file_name,ssize_t (*write_log)(const char* file_name,const void *buf, size_t n));
ssize_t server_start_loop(async_server_t* server);
void server_destroy(async_server_t* server);

ssize_t add_remote_sockets_http(async_server_t* server, struct sockaddr *addr, socklen_t len, int read_timeout, int write_timeout);
ssize_t add_remote_sockets_iso8583(async_server_t* server, struct sockaddr *addr, socklen_t len, int read_timeout, int write_timeout);
ssize_t add_local_sockets(async_server_t* server, struct sockaddr *addr, socklen_t len, int read_timeout, int write_timeout);

#endif