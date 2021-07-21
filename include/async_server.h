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

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// 以下是自定义协议结构体
enum data_type_t{
    data_type_http,
    data_type_iso8583,
    data_type_local_protocol,
};

typedef struct local_protocol_data_s local_protocol_data_t;
struct local_protocol_data_s{
    uint64_t id;
    uint64_t clock;
    enum data_type_t data_type;
    uint8_t data[0];
};

// 以下是自定义协议解析
enum local_protocol_state{
    s_local_protocol_head_start = 0,
    s_local_protocol_head_end = 1,
    s_local_protocol_body = 2,
};

typedef struct local_protocol_parser_s local_protocol_parser;
struct local_protocol_parser_s{
    enum local_protocol_state state;
    unsigned char length_n[2];
    uint16_t length;
};

void local_protocol_parser_init(local_protocol_parser *parser);
size_t local_protocol_parser_execute(local_protocol_parser *parser,const void *data,size_t len);
size_t local_protocol_parser_is_done(local_protocol_parser *parser);

#endif