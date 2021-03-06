#ifndef _ASYNC_SERVER_
#define _ASYNC_SERVER_

#include <stddef.h>      //for size_t
#include <sys/types.h>   //for ssize_t
#include <netinet/in.h>  //for struct sockaddr socklen_t

#define DATA_TYPE_HTTP 1
#define DATA_TYPE_LOCAL 2

typedef struct async_server_s async_server_t;

/**
 * 创建async_server_t对象
 * @param[in] file_name id文件名
 * @param[in] file_name log文件名
 * @param[in] cache_size
 * @return not NULL if successful, otherwise NULL
*/
async_server_t* server_create(const char* id_file, const char * log_file, size_t cache_size);

/**
 * 运行server
 * @param[in] server
 * @param[in] maxapps 每次最多处理application的事件个数
 * @return 0 if successful, otherwise an error occurred
*/
int server_start_loop(async_server_t* server, int maxapps);

/**
 * 关闭server, 关闭所有fd, 保存id
 * @param[in] server
*/
void server_destroy(async_server_t* server);

/**
 * 对外端口添加http实例
 * @param[in] server
 * @param[in] addr
 * @param[in] len
 * @param[in] read_timeout 读超时
 * @param[in] write_timeout 写超时
 * @return 0 if successful, otherwise an error occurred
*/
int add_sockets(async_server_t* server, struct sockaddr *addr, socklen_t len, int timeout, uint8_t data_type);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// 以下是自定义协议结构体

typedef struct local_protocol_data_s local_protocol_data_t;
struct local_protocol_data_s{
    uint64_t id;
    uint64_t clock;
    uint8_t data_type;
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
    uint16_t length;
};

/**
 * 初始化自定义协议的parser
 * @param[in] parser
*/
void local_protocol_parser_init(local_protocol_parser *parser);

/**
 * 自定义协议的parser解析数据
 * @param[in] parser
 * @param[in] data
 * @param[in] len
 * @return len 返回实际解析data的长度
*/
size_t local_protocol_parser_execute(local_protocol_parser *parser,const void *data,size_t len);

/**
 * 判断自定义协议的parser时候解析完一个包的数据
 * @param[in] parser
 * @return 1 成功解析完一个包, 0 未成功
*/
size_t local_protocol_parser_is_done(local_protocol_parser *parser);

#endif