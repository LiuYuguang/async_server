#include <stdio.h>
#include <stdlib.h>     //for malloc free
#include <unistd.h>     //for close
#include <fcntl.h>      //for nonblock
#include <string.h>     //for memset memcpy

#include <sys/socket.h> //for socket
#include <netinet/in.h> //for sockaddr_in htons htonl
#include <arpa/inet.h>  //for htons htonl
#include <sys/un.h>     //for struct sockaddr_un

#include <sys/epoll.h>  //for epoll*
#include <stdint.h>     //for uint64_t uint16_t
#include <signal.h>     //for signal
#include <errno.h>      //for E*
#include <sys/time.h>   //for gettimeofday
#include <time.h>       //for struct tm
#include <linux/limits.h> // PATH_MAX
#include <stdarg.h>     // va_start va_end
#include <sys/stat.h>
#include <libgen.h>

#include "async_server.h"
#include "rbtree.h"
#include "queue.h"
#include "http_parser.h"
#include "iso8583_parser.h"

#define BACKLOG 256 // listen

#define LOG_TIMESTAMP_SIZE 30 // "yyyy-mm-dd HH:MM:SS:ffffff "
#define LOG_SIZE 512      // 

#define REMOTE_MIN_BUF (1<<8)  // 256
#define REMOTE_MAX_BUF (1<<15) // 32768, 请勿随意修改, htons最长为65535

#define LOCAL_MAX_SEND_COUNT 50 // local socket连续写次数
#define LOCAL_MAX_WMEM (128*1024)  // 128K, local socket写缓存大小
#define LOCAL_MAX_RMEM (128*1024)  // 128K, local socket读缓存大小

#define HTTP_400_RESPONSE \
"HTTP/1.1 400 Bad Request\r\n"\
"Content-Length: 183\r\n"\
"Connection: close\r\n"\
"Content-Type: text/html\r\n"\
"\r\n"\
"<!DOCTYPE html>"\
"<html>"\
    "<head>"\
        "<title>400 Bad Request</title>"\
    "</head>"\
    "<body>"\
        "<h1>Bad Request</h1>"\
        "<p>Your browser sent a request that this server could not understand.<br />"\
        "</p>"\
    "</body>"\
"</html>"

#define HTTP_405_RESPONSE \
"HTTP/1.1 405 Method Not Allowed\r\n"\
"Allow: POST\r\n"\
"Content-Length: 154\r\n"\
"Connection: close\r\n"\
"Content-Type: text/html\r\n"\
"\r\n"\
"<!DOCTYPE html>"\
"<html>"\
    "<head>"\
        "<title>405 Method Not Allowed</title>"\
    "</head>"\
    "<body>"\
        "<h1>Method Not Allowed</h1>"\
        "<p>method only allow POST.<br />"\
        "</p>"\
    "</body>"\
"</html>"

#define HTTP_413_RESPONSE \
"HTTP/1.1 413 Payload Too Large\r\n"\
"Content-Length: 158\r\n"\
"Connection: close\r\n"\
"Content-Type: text/html\r\n"\
"\r\n"\
"<html>"\
"<head>"\
    "<title>413 Payload Too Large</title>"\
    "</head>"\
    "<body>"\
        "<h1>Payload Too Large</h1>"\
        "<p>Your browser sent a request that too large .<br />"\
        "</p>"\
    "</body>"\
"</html>"

#define HTTP_503_RESPONSE_READTIMEOUT \
"HTTP/1.1 503 Service Unavailable\r\n"\
"Content-Length: 153\r\n"\
"Connection: close\r\n"\
"Content-Type: text/html\r\n"\
"\r\n"\
"<!DOCTYPE html>"\
"<html>"\
    "<head>"\
        "<title>503 Service Unavailable</title>"\
    "</head>"\
    "<body>"\
        "<h1>Service Unavailable</h1>"\
        "<p>server read timeout.<br />"\
        "</p>"\
    "</body>"\
"</html>"

#define HTTP_503_RESPONSE_WAITTIMEOUT \
"HTTP/1.1 503 Service Unavailable\r\n"\
"Content-Length: 153\r\n"\
"Connection: close\r\n"\
"Content-Type: text/html\r\n"\
"\r\n"\
"<!DOCTYPE html>"\
"<html>"\
    "<head>"\
        "<title>503 Service Unavailable</title>"\
    "</head>"\
    "<body>"\
        "<h1>Service Unavailable</h1>"\
        "<p>server wait timeout.<br />"\
        "</p>"\
    "</body>"\
"</html>"

//对于remote, 流程为read->wait->write->close
//对于local, 流程为read/write->close
enum app_status_t{
    app_status_read   = (1<<0),
    app_status_write  = (1<<1),
    app_status_wait   = (1<<2),
    app_status_close  = (1<<3),
};

typedef struct app_s app_t;
struct app_s{
    int app_fd;                                            // fd
    uint8_t app_data_type;                                 // 数据类型
    enum app_status_t app_status;                          // 状态
    int (*app_read_handler)(app_t* ,async_server_t*);      // read的handler
    int (*app_write_handler)(app_t* ,async_server_t*);     // write的handler

    uint64_t app_read_timestamp;                           // 开始读的时间
    uint64_t app_write_timestamp;                          // 开始写的时间
    uint64_t app_wait_timestamp;                           // 开始等待的时间
    uint64_t app_close_timestamp;                          // 关闭的时间
    uint64_t app_id;                                       // 唯一id

    unsigned char *app_buf;                                // buf
    size_t app_buf_len;                                    // buf长度
    size_t app_buf_cap;                                    // buf容量

    int app_read_timeout;                                  // 超时时间, ms为单位, -1为infinite
    int app_write_timeout;                                 // 超时时间, ms为单位, -1为infinite
    
    union app_parser_t{
        http_parser http;
        iso8583_parser iso8583;
        local_protocol_parser local_protocol;            
    }app_parser;                                           // parser

    rbtree_node_t id_node;                                 // id查找
    rbtree_node_t timer_node;                              // 定时器
    queue_t task_node;                                     // 分配队列, 对于http/8583是任务，对于local是worker
};

struct async_server_s{
    int epfd;                                              // epoll根
    struct epoll_event *ev_events;                         // epoll_wait需要的事件
    rbtree_t id_rbtree;                                    // id根
    rbtree_t timer_rbtree;                                 // 定时器根
    queue_t worker;                                        // 待分配worker队列
    queue_t task;                                          // 待处理任务队列
    int dummyfd;                                           // for run out of file descriptors (because of resource limits)
    uint64_t id;                                           // id
    uint64_t alive_comm;
    char id_file[PATH_MAX+1];                              // id文件, 初始化读id文件获取id, 关闭server写入id文件
    char log_file[PATH_MAX+1];                             // log文件
    size_t log_cap;                                        // log缓存上限
    size_t log_len;                                        // log缓存长度
    unsigned char log_cache[0];                            // log缓存, 注意!!!!!不要再下面新加成员!!!!!在上面添加
};

static void _localtime(uint64_t *millisecond){
    struct timeval tv;
    gettimeofday(&tv,NULL);
    *millisecond = tv.tv_sec*1000 + tv.tv_usec/1000;
}

ssize_t write_log(const char* file_name,const void *buf, size_t n);

ssize_t _flush(async_server_t *server){
    if(server->log_file[0] == '\0'){
        return -1;
    }

    if(server->log_len == 0){
        return 0;
    }

    ssize_t retu = write_log(server->log_file,server->log_cache,server->log_len);
    server->log_len = 0;
    return retu;
}

ssize_t _write(async_server_t *server,const void *buf, size_t n){
    if(server->log_file[0] == '\0'){
        return -1;
    }

    if(server->log_len + n > server->log_cap){
        _flush(server);
    }

    if(server->log_len + n > server->log_cap){
        return write_log(server->log_file,buf,n);
    }

    memcpy(server->log_cache + server->log_len,buf,n);
    server->log_len += n;
    return n;
}

ssize_t _write_timestamp(async_server_t *server){
    if(server->log_file[0] == '\0'){
        return -1;
    }
    int size=0,len=0;
    size = server->log_cap - server->log_len - 1;
    if(size < LOG_TIMESTAMP_SIZE){
        _flush(server);
    }

    struct timeval tv;
    struct tm tm_time;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec,&tm_time);
    
    len = snprintf((char*)server->log_cache + server->log_len,size,
        "%04d-%02d-%02d %02d:%02d:%02d.%06ld ",
        tm_time.tm_year+1900,tm_time.tm_mon+1,tm_time.tm_mday, 
        tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, tv.tv_usec
    );
    if(len >= size){
        len = size-1;
    }
    server->log_len += len;
    return len;
}

ssize_t _write_string(async_server_t *server,const char *format, ...){
    if(server->log_file[0] == '\0'){
        return -1;
    }
    int size=0,len=0;
    size = server->log_cap - server->log_len - 1;
    if(size < LOG_SIZE){
        _flush(server);
    }

    va_list arg;
    va_start(arg, format);
	len = vsnprintf((char*)server->log_cache + server->log_len, size, format, arg);
	va_end(arg);
    if(len >= size){
        len = size-1;
    }
    server->log_len += len;
    return len;
}

ssize_t _write_timestamp_string(async_server_t *server,const char *format, ...){
    if(server->log_file[0] == '\0'){
        return -1;
    }
    int size=0,total_len=0,len=0;

    total_len = _write_timestamp(server);

    size = server->log_cap - server->log_len - 1;
    if(size < LOG_SIZE){
        _flush(server);
    }

    va_list arg;
    va_start(arg, format);
	len = vsnprintf((char*)server->log_cache + server->log_len, size, format, arg);
	va_end(arg);
    if(len >= size){
        len = size - 1;
    }
    total_len += len;
    server->log_len += len;
    return total_len;
}

// --------------iso8583----------------------------
static int recv_iso8583_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }

    // 扩容
    if(app->app_buf_len == app->app_buf_cap && app->app_buf_cap < REMOTE_MAX_BUF){
        app->app_buf_cap<<=1;
        app->app_buf = realloc(app->app_buf,app->app_buf_cap);
        if(app->app_buf == NULL){
            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] realloc fail errno[%d]%s, line%d\n",
            app->app_id,errno,strerror(errno),__LINE__);

            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);
            return -1;
        }
    }
    
    // 接收数据
    ssize_t len = recv(app->app_fd,app->app_buf+app->app_buf_len, app->app_buf_cap-app->app_buf_len,0);
    if(len > 0){
        // ---------------------------------------
        // 写log
        _write_timestamp_string(server,"[INFO] - id[%lu] request: len[%lu]",app->app_id,app->app_buf_len+len);
        int i,l;
        for(i=0,l=app->app_buf_len+len;i<l;i++){
            _write_string(server,"%02x",app->app_buf[i]);
        }
        _write(server,"\n",1);
        // ---------------------------------------

        len = iso8583_parser_execute(&app->app_parser.iso8583,app->app_buf+app->app_buf_len,len);
        app->app_buf_len += len;

        if(iso8583_parser_is_done(&app->app_parser.iso8583)){
            // 已经读完数据

            // 更改epoll的状态
            struct epoll_event ev = {0,{0}};
            int opt = EPOLL_CTL_MOD;
            ev.events = EPOLLERR|EPOLLHUP|EPOLLRDHUP;
            ev.data.ptr = app;
            epoll_ctl(server->epfd,opt,app->app_fd,&ev);

            // 更新状态和时间和超时时间
            app->app_status = app_status_wait;
            _localtime(&app->app_wait_timestamp);
            rbtree_delete(&server->timer_rbtree,&app->timer_node);
            if(app->app_write_timeout != -1){
                app->timer_node.key = app->app_wait_timestamp + app->app_write_timeout;
            }else{
                app->timer_node.key = -1;
            }
            rbtree_insert(&server->timer_rbtree,&app->timer_node);

            // 插入到task任务队列尾
            queue_insert_tail(&server->task,&app->task_node);

            // 插入id树
            rbtree_insert(&server->id_rbtree,&app->id_node);
        }
        return 0;
    }else if(len == 0){
        if(app->app_buf_len == app->app_buf_cap){
            // 数据过长
            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);

            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv len too long %d, line%d\n",app->app_id,app->app_buf_len,__LINE__);
            return -1;
        }else{
            // 对端关闭
            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);

            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv close by peer, line%d\n",app->app_id,__LINE__);
            return -1;
        }
    }else{
        if(errno == EINTR){
            // 被信号打断
            // 等待下一次读事件，不用操作
            return 0;
        }else if(errno == EAGAIN || errno == EWOULDBLOCK){
            // 数据未准备好
            // 等待下一次读事件，不用操作
            return 0;
        }else{
            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);

            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv fail errno[%d]%s, line%d\n",
            app->app_id,errno,strerror(errno),__LINE__);
            return -1;
        }
    }
}

static int send_iso8583_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }

    // 发送数据
    ssize_t len = send(app->app_fd,app->app_buf,app->app_buf_len,0);
    if(len > 0){
        // 发送完毕
        shutdown(app->app_fd,SHUT_WR);

        // ---------------------------------------
        // 写log
        _write_timestamp_string(server,"[INFO] - id[%lu] response: len[%lu]",app->app_id,app->app_buf_len);
        int i;
        for(i=0;i<app->app_buf_len;i++){
            _write_string(server,"%02x",app->app_buf[i]);
        }
        _write(server,"\n",1);
        // ---------------------------------------
        
        app->app_status = app_status_close;
        _localtime(&app->app_close_timestamp);
        return 0;
    }else{
        if(errno == EAGAIN){
            // 写缓存满，等待下一次的写操作
            return 0;
        }else if(errno == EINTR){
            // 被信号中断，等待下一次的写操作
            return 0;
        }else{// EPIPE
            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);

            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] send fail errno[%d]%s, line%d\n",
            app->app_id,errno,strerror(errno),__LINE__);
            return -1;
        }
    }
}

static int accept_iso8583_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }

    int sockfd,i,flag;
    char ip_address[50];
    struct sockaddr_in addr;
    socklen_t addr_len;

    for(i=0;i<BACKLOG;i++){
        addr_len = sizeof(addr);
        sockfd = accept(app->app_fd,(struct sockaddr*)&addr,&addr_len);
        if(sockfd < 0){
            if(errno == EINTR){
                // 被信号中断, 下次继续读
                return -1;
            }else if(errno == ENFILE || errno == EMFILE){
                // fd不够分配，应该close, run out of file descriptors
                close(server->dummyfd);
                addr_len = sizeof(addr);
                sockfd = accept(app->app_fd,(struct sockaddr*)&addr,&addr_len);
                if(sockfd >= 0){
                    close(sockfd);
                }
                server->dummyfd = open("/dev/null",O_RDONLY|O_CLOEXEC);
                continue;
            }else if(errno == EAGAIN || errno == EWOULDBLOCK){
                // 数据未准备好, 下次继续读
                return -1;
            }else{
                _write_timestamp_string(server,"[WARN] - 8583 accpet fail, errno[%d]%s\n",errno,strerror(errno));
                return -1;
            }
        }

        //非阻塞
        flag = fcntl(sockfd,F_GETFL);
        if(flag == -1){
            _write_timestamp_string(server,"[WARN] - 8583 F_GETFL fail, errno[%d]%s\n",errno,strerror(errno));
            close(sockfd);
            return -1;
        }

        if(!(flag & O_NONBLOCK)){
            if(fcntl(sockfd,F_SETFL,flag | O_NONBLOCK) == -1){
                _write_timestamp_string(server,"[WARN] - 8583 F_SETFL O_NONBLOCK fail, errno[%d]%s\n",errno,strerror(errno));
                close(sockfd);
                return -1;
            }
        }

        app_t *app_sock = malloc(sizeof(app_t));
        if(app_sock == NULL){
            _write_timestamp_string(server,"[WARN] - 8583 malloc fail, errno[%d]%s\n",errno,strerror(errno));
            close(sockfd);
            return -1;
        }

        app_sock->app_buf_len = 0;
        app_sock->app_buf_cap = REMOTE_MIN_BUF;
        app_sock->app_buf = malloc(app_sock->app_buf_cap);
        if(app_sock->app_buf == NULL){
            _write_timestamp_string(server,"[WARN] - 8583 malloc fail, errno[%d]%s\n",errno,strerror(errno));
            close(sockfd);
            free(app_sock);
            return -1;
        }

        // 加入到epoll
        struct epoll_event ev = {0,{0}};
        int opt = EPOLL_CTL_ADD;
        ev.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
        ev.data.ptr = app_sock;

        if(epoll_ctl(server->epfd,opt,sockfd,&ev) == -1){
            if(errno == EPERM){
            }else if(errno == ENOENT){
            }else if(errno == EEXIST){
            }
            _write_timestamp_string(server,"[WARN] - 8583 epoll_ctl fail, errno[%d]%s\n",errno,strerror(errno));
            close(sockfd);
            free(app_sock->app_buf);
            free(app_sock);
            return -1;
        }

        app_sock->app_fd = sockfd;
        app_sock->app_data_type = DATA_TYPE_ISO8583;
        app_sock->app_status = app_status_read;
        app_sock->app_read_handler = recv_iso8583_cb;
        app_sock->app_write_handler = send_iso8583_cb;

        _localtime(&app_sock->app_read_timestamp);
        app_sock->app_write_timestamp = 0;
        app_sock->app_wait_timestamp = 0;
        app_sock->app_close_timestamp = 0;

        app_sock->app_read_timeout = app->app_read_timeout;
        app_sock->app_write_timeout = app->app_write_timeout;

        // 初始化parser
        iso8583_parser_init(&app_sock->app_parser.iso8583);

        // id赋值，并初始化id结点
        app_sock->app_id = app_sock->id_node.key = server->id++;
        rbtree_node_init(&app_sock->id_node);

        // 更新超时时间，并插入timer树
        if(app_sock->app_read_timeout != -1){
            app_sock->timer_node.key = app_sock->app_read_timestamp + app_sock->app_read_timeout;
        }else{
            app_sock->timer_node.key = -1;
        }
        rbtree_insert(&server->timer_rbtree,&app_sock->timer_node);

        // 初始化队列结点
        queue_init(&app_sock->task_node);

        // 活跃连接数+1
        server->alive_comm++;

        _write_timestamp_string(server,"[INFO] - 8583 accept %s:%d, id[%lu], alive_comm[%lu]\n",
            inet_ntop(AF_INET,&addr.sin_addr,ip_address,sizeof(ip_address)),ntohs(addr.sin_port),
            app_sock->app_id,
            server->alive_comm
        );
    }
    return 0;
}
// --------------iso8583----------------------------

// --------------http----------------------------
static int on_info_default(http_parser* p) {
  return 0;
}

static int on_data_default(http_parser* p, const char *at, size_t length) {
  return 0;
}

static http_parser_settings settings_default = {
  .on_message_begin = on_info_default,
  .on_headers_complete = on_info_default,
  .on_message_complete = on_info_default,
  .on_header_field = on_data_default,
  .on_header_value = on_data_default,
  .on_url = on_data_default,
  .on_status = on_data_default,
  .on_body = on_data_default
};

static int recv_http_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }
    
    // 扩容
    if(app->app_buf_len == app->app_buf_cap && app->app_buf_cap < REMOTE_MAX_BUF){
        app->app_buf_cap<<=1;
        app->app_buf = realloc(app->app_buf,app->app_buf_cap);
        if(app->app_buf == NULL){
            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] realloc fail errno[%d]%s, line%d\n",
            app->app_id,errno,strerror(errno),__LINE__);

            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);
            return -1;
        }
    }
    
    // 接收数据
    ssize_t len = recv(app->app_fd,app->app_buf+app->app_buf_len, app->app_buf_cap-app->app_buf_len,0);
    if(len > 0){
        // ---------------------------------------
        // 写log
        _write_timestamp_string(server,"[INFO] - id[%lu] request: len[%lu]",app->app_id,app->app_buf_len+len);
        _write(server,app->app_buf,app->app_buf_len+len);
        _write(server,"\n",1);
        // ---------------------------------------

        len = http_parser_execute(&app->app_parser.http, &settings_default, (char*)app->app_buf+app->app_buf_len,len);
        app->app_buf_len += len;
        
        // 格式错误
        if(app->app_parser.http.http_errno != HPE_OK){
            shutdown(app->app_fd,SHUT_RD);

            _write_timestamp_string(server,"[ERROR] - id[%lu] http format error %d\n",app->app_id,app->app_parser.http.http_errno);

            if(app->app_buf_cap < (sizeof(HTTP_400_RESPONSE)-1)){
                while(app->app_buf_cap < (sizeof(HTTP_400_RESPONSE)-1)){
                    app->app_buf_cap<<=1;
                }
                app->app_buf = realloc(app->app_buf,app->app_buf_cap);
                if(app->app_buf == NULL){
                    // 写log
                    _write_timestamp_string(server,"[ERROR] - id[%lu] realloc fail errno[%d]%s, line%d\n",
                    app->app_id,errno,strerror(errno),__LINE__);

                    app->app_status = app_status_close;
                    _localtime(&app->app_close_timestamp);
                    return -1;
                }
            }

            app->app_status = app_status_write;
            _localtime(&app->app_write_timestamp);

            // 响应错误信息
            app->app_buf_len = sizeof(HTTP_400_RESPONSE)-1;
            memcpy(app->app_buf,HTTP_400_RESPONSE,app->app_buf_len);

            // 更新epoll状态
            struct epoll_event ev = {0,{0}};
            int opt = EPOLL_CTL_MOD;
            ev.events = EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
            ev.data.ptr = app;
            epoll_ctl(server->epfd,opt,app->app_fd,&ev);
            return -1;
        }
        
        // 已经读完数据
        if(http_request_is_done(&app->app_parser.http)){
            // 更改epoll的状态为监听socket错误
            struct epoll_event ev = {0,{0}};
            int opt = EPOLL_CTL_MOD;
            ev.events = EPOLLERR|EPOLLHUP|EPOLLRDHUP;
            ev.data.ptr = app;
            epoll_ctl(server->epfd,opt,app->app_fd,&ev);

            //更新状态
            app->app_status = app_status_wait;
            _localtime(&app->app_wait_timestamp);
            
            //重新设置超时时间
            rbtree_delete(&server->timer_rbtree,&app->timer_node);
            if(app->app_write_timeout != -1){
                app->timer_node.key = app->app_wait_timestamp + app->app_write_timeout;
            }else{
                app->timer_node.key = -1;
            }
            rbtree_insert(&server->timer_rbtree,&app->timer_node);
            
            //插入到task任务队列尾
            queue_insert_tail(&server->task,&app->task_node);

            // 插入id树
            rbtree_insert(&server->id_rbtree,&app->id_node);
        }
        return 0;
    }else if(len == 0){
        if(app->app_buf_len == app->app_buf_cap){
            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);

            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv len too long %d, line%d\n",app->app_id,app->app_buf_len,__LINE__);
            return -1;
        }else{
            //对端关闭
            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);

            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv close by peer, line%d\n",app->app_id,__LINE__);
            return -1;
        }
    }else{
        if(errno == EINTR){
            //被信号打断
            //等待下一次读事件，不用操作
            return 0;
        }else if(errno == EAGAIN || errno == EWOULDBLOCK){
            //数据未准备好
            //等待下一次读事件，不用操作
            return 0;
        }else{
            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);

            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv fail errno[%d]%s, line%d\n",
            app->app_id,errno,strerror(errno),__LINE__);
            return -1;
        }
    }
}

static int send_http_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }

    // 发送数据
    ssize_t len = send(app->app_fd,app->app_buf,app->app_buf_len,0);
    if(len > 0){
        //发送完毕
        shutdown(app->app_fd,SHUT_WR);

        // ---------------------------------------
        // 写log
        _write_timestamp_string(server,"[INFO] - id[%lu] response: len[%lu]",app->app_id,app->app_buf_len);
        _write(server,app->app_buf,app->app_buf_len);
        _write(server,"\n",1);
        // ---------------------------------------

        app->app_status = app_status_close;
        _localtime(&app->app_close_timestamp);
        return 0;
    }else{
        if(errno == EAGAIN){
            //写缓存满，等待下一次的写操作
            return 0;
        }else if(errno == EINTR){
            //被信号中断，等待下一次的写操作
            return 0;
        }else{
            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);

            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] send fail errno[%d]%s, line%d\n",
            app->app_id,errno,strerror(errno),__LINE__);
            return -1;
        }
    }
}

static int accept_http_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }

    int sockfd,i,flag;
    char ip_address[50];
    struct sockaddr_in addr;
    socklen_t addr_len;

    for(i=0;i<BACKLOG;i++){
        addr_len = sizeof(addr);
        sockfd = accept(app->app_fd,(struct sockaddr*)&addr,&addr_len);
        if(sockfd < 0){
            if(errno == EINTR){
                // 被信号中断, 下次继续读
                return -1;
            }else if(errno == ENFILE || errno == EMFILE){
                // fd不够分配，应该close, run out of file descriptors
                close(server->dummyfd);
                addr_len = sizeof(addr);
                sockfd = accept(app->app_fd,(struct sockaddr*)&addr,&addr_len);
                if(sockfd >= 0){
                    close(sockfd);
                }
                server->dummyfd = open("/dev/null",O_RDONLY | O_CLOEXEC);
                //return -1;
                continue;
            }else if(errno == EAGAIN || errno == EWOULDBLOCK){
                //数据未准备好, 下次继续读
                return -1;
            }else{
                _write_timestamp_string(server,"[WARN] - http accpet fail, errno[%d]%s\n",errno,strerror(errno));
                return -1;
            }
        }

        //非阻塞
        flag = fcntl(sockfd,F_GETFL);
        if(flag == -1){
            _write_timestamp_string(server,"[WARN] - http F_GETFL fail, errno[%d]%s\n",errno,strerror(errno));
            close(sockfd);
            //return -1;
            continue;
        }

        if(!(flag & O_NONBLOCK)){
            if(fcntl(sockfd,F_SETFL,flag | O_NONBLOCK) == -1){
                _write_timestamp_string(server,"[WARN] - http F_SETFL O_NONBLOCK fail, errno[%d]%s\n",errno,strerror(errno));
                close(sockfd);
                //return -1;
                continue;
            }
        }
        
        app_t *app_sock = malloc(sizeof(app_t));
        if(app_sock == NULL){
            _write_timestamp_string(server,"[WARN] - http malloc fail, errno[%d]%s\n",errno,strerror(errno));
            close(sockfd);
            //return -1;
            continue;
        }
        
        app_sock->app_buf_len = 0;
        app_sock->app_buf_cap = REMOTE_MIN_BUF;
        app_sock->app_buf = malloc(app_sock->app_buf_cap);
        if(app_sock->app_buf == NULL){
            close(sockfd);
            free(app_sock);
            return -1;
        }
        
        // 加入到epoll
        struct epoll_event ev = {0,{0}};
        int opt = EPOLL_CTL_ADD;
        ev.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
        ev.data.ptr = app_sock;

        if(epoll_ctl(server->epfd,opt,sockfd,&ev) == -1){
            if(errno == EPERM){
            }else if(errno == ENOENT){
            }else if(errno == EEXIST){
            }
            _write_timestamp_string(server,"[WARN] - http epoll_ctl fail, errno[%d]%s\n",errno,strerror(errno));
            close(sockfd);
            free(app_sock->app_buf);
            free(app_sock);
            //return -1;
            continue;
        }
        
        app_sock->app_fd = sockfd;
        app_sock->app_data_type = DATA_TYPE_HTTP;
        app_sock->app_status = app_status_read;
        app_sock->app_read_handler = recv_http_cb;
        app_sock->app_write_handler = send_http_cb;

        _localtime(&app_sock->app_read_timestamp);
        app_sock->app_write_timestamp = 0;
        app_sock->app_wait_timestamp = 0;
        app_sock->app_close_timestamp = 0;

        app_sock->app_read_timeout = app->app_read_timeout;
        app_sock->app_write_timeout = app->app_write_timeout;

        // 初始化parser
        http_parser_init(&app_sock->app_parser.http,HTTP_REQUEST);

        // id赋值，并初始化id结点
        app_sock->app_id = app_sock->id_node.key = server->id++;
        rbtree_node_init(&app_sock->id_node);

        //更新超时时间
        if(app_sock->app_read_timeout != -1){
            app_sock->timer_node.key = app_sock->app_read_timestamp + app_sock->app_read_timeout;
        }else{
            app_sock->timer_node.key = -1;
        }
        rbtree_insert(&server->timer_rbtree,&app_sock->timer_node);

        //初始化队列
        queue_init(&app_sock->task_node);

        // 活跃连接数+1
        server->alive_comm++;

        _write_timestamp_string(server,"[INFO] - http accept %s:%d, id[%lu], alive_comm[%lu]\n",
            inet_ntop(AF_INET,&addr.sin_addr,ip_address,sizeof(ip_address)),ntohs(addr.sin_port),
            app_sock->app_id,
            server->alive_comm
        );
    }
    return 0;
}
// --------------http----------------------------

// --------------local----------------------------
static int recv_local_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }

    ssize_t len;
    size_t len_parser;
    local_protocol_data_t *proto_data;
    int proto_data_size;
    unsigned char *p;
    size_t offset;

    len = recv(app->app_fd,app->app_buf+app->app_buf_len, app->app_buf_cap-app->app_buf_len,0);
    if(len > 0){
        _write_timestamp_string(server,"[INFO] - local recv, id[%lu], buf_len[%lu],buf_cap[%lu],len[%lu]\n",
            app->app_id,app->app_buf_len,app->app_buf_cap,len
        );

        p = app->app_buf;
        offset = app->app_buf_len;
        len_parser = 0;
        while(len_parser < len){
            len_parser = local_protocol_parser_execute(&app->app_parser.local_protocol,p+offset,len);
            offset += len_parser;
            if(local_protocol_parser_is_done(&app->app_parser.local_protocol)){
                // 完整一个包
                // local_protocol头两位为长度
                proto_data = (local_protocol_data_t *)(p+sizeof(uint16_t));
                proto_data_size = offset-sizeof(uint16_t)-sizeof(local_protocol_data_t);

                _write_timestamp_string(server,"[INFO] - local recv, proto_data id[%lu]\n",proto_data->id);

                //根据id查找原fd
                rbtree_node_t *rbtree_node = rbtree_search(&server->id_rbtree,proto_data->id);
                if(rbtree_node != NULL){
                    // 匹配成功
                    app_t* app_task = rbtree_data(rbtree_node,app_t,id_node);
                    _write_timestamp_string(server,"[INFO] - local recv, proto_data match id[%lu]\n",proto_data->id);

                    if(app_task->app_buf_cap < proto_data_size){
                        app_task->app_buf_cap = proto_data_size;
                        // app_task->app_buf = realloc(app_task->app_buf,app_task->app_buf_cap);
                        free(app_task->app_buf);
                        app_task->app_buf = malloc(app_task->app_buf_cap);
                    }
                    if(app_task->app_buf != NULL){
                        memcpy(app_task->app_buf,proto_data->data,proto_data_size);
                        app_task->app_buf_len = proto_data_size;

                        struct epoll_event ev = {0,{0}};
                        int opt = EPOLL_CTL_MOD;
                        ev.events = EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
                        ev.data.ptr = app_task;
                        epoll_ctl(server->epfd,opt,app_task->app_fd,&ev);

                        app_task->app_status = app_status_write;
                        _localtime(&app_task->app_write_timestamp);
                    }else{
                        struct epoll_event ev = {0,{0}};
                        int opt = EPOLL_CTL_MOD;
                        ev.events = EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
                        ev.data.ptr = app_task;
                        epoll_ctl(server->epfd,opt,app_task->app_fd,&ev);

                        app_task->app_status = app_status_close;
                        _localtime(&app_task->app_close_timestamp);
                    }

                    // 移除id树
                    rbtree_delete(&server->id_rbtree,&app_task->id_node);
                }
                p += offset;
                offset = 0;
                len -= len_parser;
                len_parser = 0;
            }
        }
        memmove(app->app_buf,p,offset);
        app->app_buf_len = offset;
        return 0;
    }else if(len == 0){
        // 写log
        _write_timestamp_string(server,"[ERROR] - id[%lu] close by peer\n",app->app_id);

        app->app_status = app_status_close;
        _localtime(&app->app_close_timestamp);
        return -1;
    }else{
        if(errno == EINTR){
            //被信号打断
            //等待下一次读事件，不用操作
            return 0;
        }else if(errno == EAGAIN || errno == EWOULDBLOCK){
            //数据未准备好
            //等待下一次读事件，不用操作
            return 0;
        }else{
            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);

            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv fail errno[%d]%s, line%d\n",
            app->app_id,errno,strerror(errno),__LINE__);
            return -1;
        }
    }
    
}

static int send_local_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }

    if(queue_empty(&server->task)){
        _write_timestamp_string(server,"[INFO] - empty query\n");
        // 任务队列为空，不再等待写事件，只保留读事件
        struct epoll_event ev = {0,{0}};
        int opt = EPOLL_CTL_MOD;
        ev.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
        ev.data.ptr = app;
        epoll_ctl(server->epfd,opt,app->app_fd,&ev);
        app->app_status = app_status_read;
        return 0;
    }
    
    unsigned char buf[LOCAL_MAX_WMEM],*p=buf;
    uint16_t* pbuf;
    size_t buf_len = 0;

    // 取任务队列头结点
    queue_t *task;
    app_t *app_task;
    ssize_t len;
    int i,j;
    local_protocol_data_t *proto_data;
    for(i=0,task = queue_head(&server->task);task!=queue_sentinel(&server->task)&&i<LOCAL_MAX_SEND_COUNT;i++,task=queue_next(task)){
        app_task = queue_data(task,app_t,task_node);

        if(buf_len+sizeof(uint16_t)+sizeof(local_protocol_data_t)+app_task->app_buf_len > LOCAL_MAX_WMEM){
            break;
        }

        // local_protocol_data前两位为长度
        proto_data = (local_protocol_data_t *)(p + sizeof(uint16_t));
        proto_data->id = app_task->app_id;
        proto_data->clock = app_task->app_read_timestamp;
        proto_data->data_type = app_task->app_data_type;
        // 赋值请求数据
        memcpy(proto_data->data,app_task->app_buf,app_task->app_buf_len);
        pbuf = (uint16_t*)p;
        *pbuf = htons(sizeof(local_protocol_data_t)+app_task->app_buf_len);
        buf_len += sizeof(uint16_t)+sizeof(local_protocol_data_t)+app_task->app_buf_len;
        p += sizeof(uint16_t)+sizeof(local_protocol_data_t)+app_task->app_buf_len;
    }

    len = send(app->app_fd,buf,buf_len,0);
    if(len > 0){
        for(j=0;j<i;j++){
            task = queue_head(&server->task);
            app_task = queue_data(task,app_t,task_node);
            //发送成功，任务从队列中取出
            queue_remove(&app_task->task_node);
            _write_timestamp_string(server,"[INFO] - local send id[%lu] finish\n",app_task->app_id);
        }
        // return 0;
    }else{
        if(errno == EAGAIN){
            return 0;
            // break;
        }else if(errno == EINTR){
            return 0;
            // break;
        }else{
            app->app_status = app_status_close;
            _localtime(&app->app_close_timestamp);

            // 写log
            _write_timestamp_string(server,"[ERROR] - id[%lu] send fail errno[%d]%s, line%d\n",
            app->app_id,errno,strerror(errno),__LINE__);
            return -1;
        }
    }
    
    //将自己放到队尾, 负载均衡
    queue_remove(&app->task_node);
    queue_insert_tail(&server->worker, &app->task_node);
    return 0;
}

static int accept_local_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }

    int sockfd;
    char ip_address[50];
    struct sockaddr_in addr;
    socklen_t addr_len;
    
    addr_len = sizeof(addr);
    sockfd = accept(app->app_fd,(struct sockaddr*)&addr,&addr_len);
    if(sockfd < 0){
        if(errno == EINTR){
            //被信号中断, 下次继续读
            return -1;
        }else if(errno == ENFILE || errno == EMFILE){
            //fd不够分配，应该close, run out of file descriptors
            close(server->dummyfd);
            addr_len = sizeof(addr);
            sockfd = accept(app->app_fd,(struct sockaddr*)&addr,&addr_len);
            if(sockfd >= 0){
                close(sockfd);
            }
            server->dummyfd = open("/dev/null",O_RDONLY | O_CLOEXEC);
            return -1;
        }else if(errno == EAGAIN || errno == EWOULDBLOCK){
            //数据未准备好, 下次继续读
            return -1;
        }else{
            _write_timestamp_string(server,"[WARN] - local accpet fail, errno[%d]%s\n",errno,strerror(errno));
            return -1;
        }
    }
    
    //非阻塞
    int flag = fcntl(sockfd,F_GETFL);
    if(flag == -1){
        _write_timestamp_string(server,"[WARN] - local F_GETFL fail, errno[%d]%s\n",errno,strerror(errno));
        close(sockfd);
        return -1;
    }

    if(!(flag & O_NONBLOCK)){
        if(fcntl(sockfd,F_SETFL,flag | O_NONBLOCK) == -1){
            _write_timestamp_string(server,"[WARN] - local F_SETFL O_NONBLOCK fail, errno[%d]%s\n",errno,strerror(errno));
            close(sockfd);
            return -1;
        }
    }

    app_t *app_sock = malloc(sizeof(app_t));
    if(app_sock == NULL){
        _write_timestamp_string(server,"[WARN] - local malloc fail, errno[%d]%s\n",errno,strerror(errno));
        close(sockfd);
        return -1;
    }

    app_sock->app_buf_len = 0;
    app_sock->app_buf_cap = LOCAL_MAX_RMEM;
    app_sock->app_buf = malloc(app_sock->app_buf_cap);
    if(app_sock->app_buf == NULL){
        close(sockfd);
        free(app_sock);
        return -1;
    }

    struct epoll_event ev = {0,{0}};
    int opt = EPOLL_CTL_ADD;
    ev.events = EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
    ev.data.ptr = app_sock;

    if(epoll_ctl(server->epfd,opt,sockfd,&ev) == -1){
        if(errno == EPERM){
        }else if(errno == ENOENT){
        }else if(errno == EEXIST){
        }
        _write_timestamp_string(server,"[WARN] - local epoll_ctl fail, errno[%d]%s\n",errno,strerror(errno));
        close(sockfd);
        free(app_sock->app_buf);
        free(app_sock);
        return -1;
    }

    app_sock->app_fd = sockfd;
    app_sock->app_data_type = DATA_TYPE_LOCAL;
    app_sock->app_status = app_status_read | app_status_write;
    app_sock->app_read_handler = recv_local_cb;
    app_sock->app_write_handler = send_local_cb;

    _localtime(&app_sock->app_read_timestamp);
    app_sock->app_write_timestamp = 0;
    app_sock->app_wait_timestamp = 0;
    app_sock->app_close_timestamp = 0;

    app_sock->app_read_timeout = app->app_read_timeout;
    app_sock->app_write_timeout = app->app_write_timeout;

    // 初始化parser
    local_protocol_parser_init(&app_sock->app_parser.local_protocol);

    // id赋值，并初始化id结点
    app_sock->app_id = app_sock->id_node.key = server->id++;
    rbtree_node_init(&app_sock->id_node);

    // 更新超时时间
    if(app_sock->app_read_timeout != -1){
        app_sock->timer_node.key = app_sock->app_read_timestamp + app_sock->app_read_timeout;
    }else{
        app_sock->timer_node.key = -1;
    }
    rbtree_insert(&server->timer_rbtree,&app_sock->timer_node);

    // 添加到worker队列
    queue_insert_tail(&server->worker,&app_sock->task_node);

    // 活跃连接数+1
    server->alive_comm++;

    _write_timestamp_string(server,"[INFO] - local accept %s:%d, id[%lu], alive_comm[%lu]\n",
        inet_ntop(AF_INET,&addr.sin_addr,ip_address,sizeof(ip_address)),ntohs(addr.sin_port),
        app_sock->app_id,
        server->alive_comm
    );

    //设置socket心跳包, 只能是TCP才有作用
    // int val = 1;
	// setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
	// val = 120;
	// setsockopt(sockfd, SOL_TCP, TCP_KEEPIDLE, &val, sizeof(val));
	// val = 30;
	// setsockopt(sockfd, SOL_TCP, TCP_KEEPINTVL, &val, sizeof(val));

    // 设置缓存
    int send_buf = LOCAL_MAX_WMEM, recv_buf = LOCAL_MAX_RMEM;
    socklen_t send_buf_size = sizeof(send_buf), recv_buf_size = sizeof(recv_buf);
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const char*)&send_buf, sizeof(send_buf));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (const char*)&recv_buf, sizeof(recv_buf));

    getsockopt(sockfd,SOL_SOCKET,SO_SNDBUF,&send_buf,&send_buf_size);
    getsockopt(sockfd,SOL_SOCKET,SO_RCVBUF,&recv_buf,&recv_buf_size);

    _write_timestamp_string(server,"[INFO] - id[%lu] SNDBUF[%d]RCVBUF[%d]\n",
        app_sock->app_id,
        send_buf,recv_buf
    );


    return 0;
}
// --------------local----------------------------

async_server_t* server_create(size_t cache_size){
    if(cache_size < SERVER_CACHE_SIZE){
        cache_size = SERVER_CACHE_SIZE;
    }

    async_server_t *server = malloc(sizeof(async_server_t) + cache_size);
    if(server == NULL){
        fprintf(stderr,"malloc fail errno[%d]%s\n",errno,strerror(errno));
        return NULL;
    }
    server->epfd = epoll_create(1);
    if(server->epfd == -1){
        fprintf(stderr,"epoll_create fail errno[%d]%s\n",errno,strerror(errno));
        free(server);
        return NULL;
    }
    server->ev_events = NULL;
    rbtree_init(&server->id_rbtree);
    rbtree_init(&server->timer_rbtree);
    queue_init(&server->worker);
    queue_init(&server->task);
    server->dummyfd = open("/dev/null",O_RDONLY | O_CLOEXEC);
    server->id = 0;
    server->alive_comm=0;
    memset(server->id_file,0,sizeof(server->id_file));
    memset(server->log_file,0,sizeof(server->log_file));
    server->log_cap = cache_size;
    server->log_len = 0;
    return server;
}

int server_set_id_file(async_server_t* server,const char *file_name){
    if(server == NULL || file_name==NULL){
        return -1;
    }
    if(strlen(file_name) > PATH_MAX){
        return -1;
    }
    strcpy(server->id_file,file_name);
    int fd = open(file_name,O_RDONLY);
    if(fd == -1){
        return -1;
    }
    if(read(fd,&server->id,sizeof(server->id)) != sizeof(server->id)){
        server->id = 0;
    }
    close(fd);
    return 0;
}

int server_set_log_file(async_server_t* server,const char *file_name){
    if(server == NULL||file_name==NULL){
        return -1;
    }
    if(strlen(file_name) > PATH_MAX){
        return -1;
    }
    strcpy(server->log_file,file_name);
    return 0;
}

int server_start_loop(async_server_t* server, int maxapps){
    if(server == NULL){
        return -1;
    }

    rbtree_node_t* node;
    queue_t *qnode;
    app_t *app;
    int ev_eventsSize = maxapps;
    int ev_nready, timeout, i;
    uint64_t now_millisecond;

    server->ev_events = malloc(sizeof(struct epoll_event) * ev_eventsSize);
    if(server->ev_events == NULL){
        fprintf(stderr,"malloc fail errno[%d]%s\n",errno,strerror(errno));
        return -1;
    }

    for(;;){
        _flush(server);

        _write_timestamp_string(server,"[DEBUG] start loop\n");
        // 当前时间
        _localtime(&now_millisecond);

        //获取最小等待时间
        for(node = rbtree_min(&server->timer_rbtree,server->timer_rbtree.root);node != NULL && node->key <= now_millisecond;){
            app = rbtree_data(node,app_t,timer_node);
            node = rbtree_next(&server->timer_rbtree,node);

            if(app->app_data_type == DATA_TYPE_HTTP && (app->app_status&app_status_wait)){
                if(app->app_buf_cap < (sizeof(HTTP_503_RESPONSE_WAITTIMEOUT)-1)){
                    while(app->app_buf_cap < (sizeof(HTTP_503_RESPONSE_WAITTIMEOUT)-1)){
                        app->app_buf_cap<<=1;
                    }
                    app->app_buf = realloc(app->app_buf,app->app_buf_cap);
                }
                if(app->app_buf != NULL){
                    // 响应错误信息
                    app->app_buf_len = sizeof(HTTP_503_RESPONSE_WAITTIMEOUT)-1;
                    memcpy(app->app_buf,HTTP_503_RESPONSE_WAITTIMEOUT,app->app_buf_len);

                    app->app_status = app_status_write;
                    _localtime(&app->app_write_timestamp);

                    // 删除id
                    rbtree_delete(&server->id_rbtree,&app->id_node);

                    // 删除队列
                    queue_remove(&app->task_node);

                    // 更新epoll状态
                    struct epoll_event ev = {0,{0}};
                    ev.events = EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
                    ev.data.ptr = app;
                    epoll_ctl(server->epfd,EPOLL_CTL_MOD,app->app_fd,&ev);
                }else{
                    app->app_status = app_status_close;
                    _localtime(&app->app_close_timestamp);
                }
            }else if(app->app_data_type == DATA_TYPE_HTTP && app->app_status&app_status_read){
                if(app->app_buf_cap < (sizeof(HTTP_503_RESPONSE_READTIMEOUT)-1)){
                    while(app->app_buf_cap < (sizeof(HTTP_503_RESPONSE_READTIMEOUT)-1)){
                        app->app_buf_cap<<=1;
                    }
                    app->app_buf = realloc(app->app_buf,app->app_buf_cap);
                }
                if(app->app_buf != NULL){
                    // 响应错误信息
                    app->app_buf_len = sizeof(HTTP_503_RESPONSE_READTIMEOUT)-1;
                    memcpy(app->app_buf,HTTP_503_RESPONSE_READTIMEOUT,app->app_buf_len);

                    app->app_status = app_status_write;
                    _localtime(&app->app_write_timestamp);

                    // 删除id
                    rbtree_delete(&server->id_rbtree,&app->id_node);

                    // 删除队列
                    queue_remove(&app->task_node);

                    // 更新epoll状态
                    struct epoll_event ev = {0,{0}};
                    ev.events = EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
                    ev.data.ptr = app;
                    epoll_ctl(server->epfd,EPOLL_CTL_MOD,app->app_fd,&ev);
                }else{
                    app->app_status = app_status_close;
                    _localtime(&app->app_close_timestamp);
                }
            }else if(app->app_status&app_status_write){
                // 不操作
            }else{
                app->app_status = app_status_close;
                _localtime(&app->app_close_timestamp);
            }

            if(app->app_status&app_status_close){
                _write_timestamp_string(server,"[ERROR] - TIMEOUT!! id[%lu]",app->app_id);
                _write_string(server,",read[%lu]",app->app_read_timestamp);
                uint64_t timestamp = app->app_read_timestamp;
                if(app->app_wait_timestamp){
                    _write_string(server,",wait[+%lu]",app->app_wait_timestamp-timestamp);
                    timestamp = app->app_wait_timestamp;
                }
                if(app->app_write_timestamp){
                    _write_string(server,",write[+%lu]",app->app_write_timestamp-timestamp);
                    timestamp = app->app_write_timestamp;
                }
                if(app->app_close_timestamp){
                    _write_string(server,",close[+%lu]",app->app_close_timestamp-timestamp);
                    timestamp = app->app_close_timestamp;
                }
                _write(server,"\n",1);

                // 删除epoll事件
                epoll_ctl(server->epfd,EPOLL_CTL_DEL,app->app_fd,NULL);
                // 删除时间
                rbtree_delete(&server->timer_rbtree,&app->timer_node);
                // 删除id
                rbtree_delete(&server->id_rbtree,&app->id_node);
                // 删除队列
                queue_remove(&app->task_node);
                // 关闭fd
                close(app->app_fd);
                // 释放buf
                free(app->app_buf);
                free(app);
                server->alive_comm--;
            }
        }
        
        if(node == NULL || node->key == (uint64_t)-1){
            timeout = -1;
        }else{
            // 计算将要过期的app时间差
            timeout = node->key - now_millisecond;
        }

        _write_timestamp_string(server,"[DEBUG] start loop timeout[%d]\n",timeout);

        // 等待事件触发或者超时
        ev_nready = epoll_wait(server->epfd,server->ev_events,ev_eventsSize,timeout);
        _write_timestamp_string(server,"[DEBUG] end   loop [%d]\n",ev_nready);
        if(ev_nready == 0){
            //超时
        }else if(ev_nready == -1){
            if(errno == EINTR){
                //被信号中断
            }
        }else{
            for(i=0;i<ev_nready;i++){
                app = (app_t*)server->ev_events[i].data.ptr;

                // 读事件
                if(server->ev_events[i].events & EPOLLIN){
                    app->app_read_handler(app,server);
                }
                // 写事件
                if(server->ev_events[i].events & EPOLLOUT){
                    app->app_write_handler(app,server);
                }
                // 其他错误事件
                if(server->ev_events[i].events & EPOLLERR){
                    app->app_status = app_status_close;
                    _localtime(&app->app_close_timestamp);
                    _write_timestamp_string(server,"[ERROR] - id[%lu] EPOLLERR\n",app->app_id);
                }
                
                if(server->ev_events[i].events & EPOLLHUP){
                    //对端关闭
                    app->app_status = app_status_close;
                    _localtime(&app->app_close_timestamp);
                    _write_timestamp_string(server,"[ERROR] - id[%lu] close by peer\n",app->app_id);
                }

                if(server->ev_events[i].events & EPOLLRDHUP){
                    //对端调用shutdown write
                    app->app_status = app_status_close;
                    _localtime(&app->app_close_timestamp);
                    _write_timestamp_string(server,"[ERROR] - id[%lu] EPOLLRDHUP\n",app->app_id);
                }

                if(app->app_status&app_status_close){
                    _write_timestamp_string(server,"[INFO] - FINISH!! id[%lu]alive_comm[%lu]",app->app_id,server->alive_comm);
                    _write_string(server,",read[%lu]",app->app_read_timestamp);
                    uint64_t timestamp = app->app_read_timestamp;
                    if(app->app_wait_timestamp){
                        _write_string(server,",wait[+%lu]",app->app_wait_timestamp-timestamp);
                        timestamp = app->app_wait_timestamp;
                    }
                    if(app->app_write_timestamp){
                        _write_string(server,",write[+%lu]",app->app_write_timestamp-timestamp);
                        timestamp = app->app_write_timestamp;
                    }
                    if(app->app_close_timestamp){
                        _write_string(server,",close[+%lu]",app->app_close_timestamp-timestamp);
                        timestamp = app->app_close_timestamp;
                    }
                    _write(server,"\n",1);

                    // 删除epoll事件
                    epoll_ctl(server->epfd,EPOLL_CTL_DEL,app->app_fd,NULL);
                    // 删除时间
                    rbtree_delete(&server->timer_rbtree,&app->timer_node);
                    // 删除id
                    rbtree_delete(&server->id_rbtree,&app->id_node);
                    // 删除队列
                    queue_remove(&app->task_node);
                    // 关闭fd
                    close(app->app_fd);
                    // 释放buf
                    free(app->app_buf);
                    free(app);
                    server->alive_comm--;
                }
            }
        }

        // 唤醒work的写操作
        if(!queue_empty(&server->task)){
            for(qnode = queue_head(&server->worker);qnode != queue_sentinel(&server->worker);qnode = queue_next(qnode)){
                app = queue_data(qnode,app_t,task_node);
                if(!(app->app_status&app_status_write)){
                    struct epoll_event ev = {0,{0}};
                    int opt = EPOLL_CTL_MOD;
                    ev.events = EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
                    ev.data.ptr = app;
                    epoll_ctl(server->epfd,opt,app->app_fd,&ev);
                    app->app_status|=app_status_write;
                }
            }
        }
    }

    free(server->ev_events);
    server->ev_events = NULL;
    return 0;
}

void server_destroy(async_server_t* server){
    if(server == NULL){
        return ;
    }

    _flush(server);

    //释放id队列的所有app
    rbtree_node_t* node;
    app_t *app;
    node = rbtree_min(&server->timer_rbtree,server->timer_rbtree.root);
    while(node != NULL){
        app = rbtree_data(node,app_t,timer_node);
        node = rbtree_next(&server->timer_rbtree,node);

        app->app_status = app_status_close;
        _localtime(&app->app_close_timestamp);

        _write_timestamp_string(server,"[INFO] - FINISH!! id[%lu]alive_comm[%lu]",app->app_id,server->alive_comm);
        _write_string(server,",read[%lu]",app->app_read_timestamp);
        uint64_t timestamp = app->app_read_timestamp;
        if(app->app_wait_timestamp){
            _write_string(server,",wait[+%lu]",app->app_wait_timestamp-timestamp);
            timestamp = app->app_wait_timestamp;
        }
        if(app->app_write_timestamp){
            _write_string(server,",write[+%lu]",app->app_write_timestamp-timestamp);
            timestamp = app->app_write_timestamp;
        }
        if(app->app_close_timestamp){
            _write_string(server,",close[+%lu]",app->app_close_timestamp-timestamp);
            timestamp = app->app_close_timestamp;
        }
        _write(server,"\n",1);

        // 删除epoll事件
        epoll_ctl(server->epfd,EPOLL_CTL_DEL,app->app_fd,NULL);
        // 删除时间
        rbtree_delete(&server->timer_rbtree,&app->timer_node);
        // 删除id
        rbtree_delete(&server->id_rbtree,&app->id_node);
        // 删除队列
        queue_remove(&app->task_node);
        // 关闭fd
        close(app->app_fd);
        // 释放buf
        free(app->app_buf);
        free(app);
        server->alive_comm--;
    }

    _flush(server);

    close(server->epfd);
    close(server->dummyfd);

    if(server->id_file[0]!='\0'){
        int fd = open(server->id_file,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(fd != -1){
            if(write(fd,&server->id,sizeof(server->id)) == -1){

            }
            close(fd);
        }
    }
    free(server->ev_events);
    free(server);
    return;
}

int create_bind_socket_nonblock(struct sockaddr *addr,socklen_t len){
    int fd = socket(addr->sa_family,SOCK_STREAM,0);
    if(fd == -1){
        fprintf(stderr,"create socket fail errno[%d]%s\n",errno,strerror(errno));
        return -1;
    }

    int flag;
    flag = 1;
    if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag)) < 0){
        fprintf(stderr,"setsockopt SO_REUSEADDR fail errno[%d]%s\n",errno,strerror(errno));
        close(fd);
        return -1;
    }

    if((flag = fcntl(fd,F_GETFL)) < 0){
        fprintf(stderr,"F_GETFL fail errno[%d]%s\n",errno,strerror(errno));
        close(fd);
        return -1;
    }

    if(!(flag & O_NONBLOCK)){
        if(fcntl(fd,F_SETFL,flag|O_NONBLOCK) < 0){
            fprintf(stderr,"F_SETFL O_NONBLOCK fail errno[%d]%s\n",errno,strerror(errno));
            close(fd);
            return -1;
        }
    }

    if(bind(fd,addr,len) == -1){
        fprintf(stderr,"bind fail errno[%d]%s\n",errno,strerror(errno));
        close(fd);
        return -1;
    }

    if(listen(fd,BACKLOG) == -1){
        fprintf(stderr,"listen fail errno[%d]%s\n",errno,strerror(errno));
        close(fd);
        return -1;
    } 
    
    return fd;
}

int add_remote_sockets_http(async_server_t* server, struct sockaddr *addr, socklen_t len, int read_timeout, int write_timeout){
    if(server == NULL){
        return -1;
    }

    int fd = create_bind_socket_nonblock(addr,len);
    if(fd == -1){
        return -1;
    }

    app_t *app = malloc(sizeof(app_t));
    if(app == NULL){
        fprintf(stderr,"malloc fail errno[%d]%s\n",errno,strerror(errno));
        close(fd);
        return -1;
    }

    app->app_fd = fd;

    struct epoll_event ev = {0,{0}};
    int opt = EPOLL_CTL_ADD;
    ev.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
    ev.data.ptr = app;

    if(epoll_ctl(server->epfd,opt,fd,&ev) == -1){
        if(errno == EPERM){
        }else if(errno == ENOENT){
        }else if(errno == EEXIST){
        }
        close(fd);
        free(app);
        return -1;
    }

    app->app_data_type = DATA_TYPE_HTTP;
    app->app_status = app_status_read;
    app->app_read_handler = accept_http_cb;
    app->app_write_handler = NULL;
    _localtime(&app->app_read_timestamp);
    app->app_write_timestamp = 0;
    app->app_wait_timestamp = 0;
    app->app_close_timestamp = 0;

    app->app_buf = NULL;
    app->app_buf_len = 0;
    app->app_buf_cap = 0;

    app->app_read_timeout = read_timeout;
    app->app_write_timeout = write_timeout;

    // id赋值，并初始化id结点
    app->app_id = app->id_node.key = server->id++;
    rbtree_node_init(&app->id_node);
    
    // timer
    app->timer_node.key = -1;
    rbtree_insert(&server->timer_rbtree,&app->timer_node);

    // 不加入queue
    queue_init(&app->task_node);

    return 0;
}

int add_remote_sockets_iso8583(async_server_t* server, struct sockaddr *addr, socklen_t len, int read_timeout, int write_timeout){
    if(server == NULL){
        return -1;
    }

    int fd = create_bind_socket_nonblock(addr,len);
    if(fd == -1){
        return -1;
    }

    app_t *app = malloc(sizeof(app_t));
    if(app == NULL){
        fprintf(stderr,"malloc fail errno[%d]%s\n",errno,strerror(errno));
        close(fd);
        return -1;
    }

    app->app_fd = fd;

    struct epoll_event ev = {0,{0}};
    int opt = EPOLL_CTL_ADD;
    ev.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
    ev.data.ptr = app;

    if(epoll_ctl(server->epfd,opt,fd,&ev) == -1){
        if(errno == EPERM){
        }else if(errno == ENOENT){
        }else if(errno == EEXIST){
        }
        close(fd);
        free(app);
        return -1;
    }

    app->app_data_type = DATA_TYPE_ISO8583;
    app->app_status = app_status_read;
    app->app_read_handler = accept_iso8583_cb;
    app->app_write_handler = NULL;
    _localtime(&app->app_read_timestamp);
    app->app_write_timestamp = 0;
    app->app_wait_timestamp = 0;
    app->app_close_timestamp = 0;

    app->app_buf = NULL;
    app->app_buf_len = 0;
    app->app_buf_cap = 0;

    app->app_read_timeout = read_timeout;
    app->app_write_timeout = write_timeout;

    // id赋值，并初始化id结点
    app->app_id = app->id_node.key = server->id++;
    rbtree_node_init(&app->id_node);

    // timer
    app->timer_node.key = -1;
    rbtree_insert(&server->timer_rbtree,&app->timer_node);

    //不加入queue
    queue_init(&app->task_node);

    return 0;
}

int add_local_sockets(async_server_t* server, struct sockaddr *addr, socklen_t len, int read_timeout, int write_timeout){
    if(server == NULL){
        return -1;
    }

    int fd = create_bind_socket_nonblock(addr,len);
    if(fd == -1){
        return -1;
    }

    app_t *app = malloc(sizeof(app_t));
    if(app == NULL){
        fprintf(stderr,"malloc fail errno[%d]%s\n",errno,strerror(errno));
        close(fd);
        return -1;
    }

    app->app_fd = fd;

    struct epoll_event ev = {0,{0}};
    int opt = EPOLL_CTL_ADD;
    ev.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
    ev.data.ptr = app;

    if(epoll_ctl(server->epfd,opt,fd,&ev) == -1){
        if(errno == EPERM){
        }else if(errno == ENOENT){
        }else if(errno == EEXIST){
        }
        close(fd);
        free(app);
        return -1;
    }

    app->app_data_type = DATA_TYPE_LOCAL;
    app->app_status = app_status_read;
    app->app_read_handler = accept_local_cb;
    app->app_write_handler = NULL;
    _localtime(&app->app_read_timestamp);
    app->app_write_timestamp = 0;
    app->app_wait_timestamp = 0;
    app->app_close_timestamp = 0;

    app->app_buf = NULL;
    app->app_buf_len = 0;
    app->app_buf_cap = 0;

    app->app_read_timeout = read_timeout;
    app->app_write_timeout = write_timeout;

    // id赋值，并初始化id结点
    app->app_id = app->id_node.key = server->id++;
    rbtree_node_init(&app->id_node);
    
    // timer
    app->timer_node.key = -1;
    rbtree_insert(&server->timer_rbtree,&app->timer_node);

    // 不加入queue
    queue_init(&app->task_node);
    return 0;
}



//--------------------------------------------------------
void local_protocol_parser_init(local_protocol_parser *parser){
    parser->state = s_local_protocol_head_start;
}

size_t local_protocol_parser_execute(local_protocol_parser *parser,const void *data,size_t len){
    const uint8_t *p;
    //网络字节序默认大端
    for(p=data;p<(const uint8_t *)data+len;p++){
        switch (parser->state)
        {
            case s_local_protocol_head_start:
                parser->length = *p;
                parser->state = s_local_protocol_head_end;
                break;
            case s_local_protocol_head_end:
                parser->length = (parser->length<<8) | *p;
                parser->state = s_local_protocol_body;
                break;
            case s_local_protocol_body:
                if(parser->length <= (const uint8_t *)data+len-p){
                    p += parser->length;
                    parser->length = 0;
                    parser->state = s_local_protocol_head_start;
                }else{
                    parser->length -= ((const uint8_t *)data+len-p);
                    p = (const uint8_t *)data + len;
                }
                goto finish;
                break;
            default:
                break;
        }
    }
    finish:
    return (p-(const uint8_t *)data);
}

size_t local_protocol_parser_is_done(local_protocol_parser *parser){
    if(parser->state == s_local_protocol_head_start){
        return 1;
    }
    return 0;
}

//--------------------------------------------------------
int makedirs(const char *path){
    //参数错误
    if(path == NULL){
        errno = EINVAL;
        return -1;
    }

    struct stat file_stat;
    //路径存在
    if(stat(path,&file_stat) == 0 && S_ISDIR(file_stat.st_mode)){
        return 0;
    }

	char path_tmp[PATH_MAX+1]={0},path_total[PATH_MAX+1]={0};
    char *index;

    if(strlen(path) > PATH_MAX){
        errno = E2BIG;
        return -1;
    }
    strcpy(path_tmp,path);

    if(path_tmp[0] == '/'){
        strcat(path_total,"/");
    }
    
	for(index = strtok(path_tmp,"/");index!=NULL;index=strtok(NULL,"/")){
        strcat(path_total,index);
        strcat(path_total,"/");
        if(stat(path_total,&file_stat) == 0){
            if(S_ISDIR(file_stat.st_mode)){
                //路径存在
                continue;
            }
            else{
                //其他类型文件
                return -1;
            }
        }

        if(mkdir(path_total,0775) == -1){
            return -1;
        }
    }
    return 0;
}

ssize_t write_log(const char* file_name,const void *buf, size_t n){
	int fd,retu=-1;
	struct stat fd_stat;
	fd = open(file_name,O_WRONLY|O_CREAT|O_APPEND,0666);
	if(fd == -1){
		char file_path[PATH_MAX+1]={0};
		strcpy(file_path,file_name);
		//创建路径
        if(makedirs(dirname(file_path)) == -1){
            return -1;
        }
		fd = open(file_name,O_WRONLY|O_CREAT|O_APPEND,0666);
		if(fd == -1){
			return -1;
		}
	}
	if(fstat(fd,&fd_stat) == -1){
        close(fd);
        return -1;
    }
	//不是常规文件
    if(!S_ISREG(fd_stat.st_mode)){
        close(fd);
        errno = EACCES;
        return -1;
    }

	retu = write(fd,buf,n);
	close(fd);
	return retu;
}