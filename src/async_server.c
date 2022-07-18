#include <stdio.h>
#include <stdlib.h>       // for malloc free
#include <unistd.h>       // for close
#include <fcntl.h>        // for nonblock
#include <string.h>       // for memset memcpy

#include <sys/socket.h>   // for socket
#include <netinet/in.h>   // for sockaddr_in htons htonl
#include <arpa/inet.h>    // for htons htonl
#include <sys/un.h>       // for struct sockaddr_un

#include <sys/epoll.h>    // for epoll*
#include <stdint.h>       // for uint64_t uint16_t
#include <signal.h>       // for signal
#include <errno.h>        // for E*
#include <sys/time.h>     // for gettimeofday
#include <time.h>         // for struct tm
#include <linux/limits.h> // PATH_MAX
#include <stdarg.h>       // va_start va_end
#include <sys/stat.h>
#include <libgen.h>

#include "async_server.h"
#include "rbtree.h"
#include "queue.h"
#include "http_parser.h"

#define BACKLOG 256 // listen

#define REMOTE_MIN_BUF (1<<8)  // 256
#define REMOTE_MAX_BUF (1<<15) // 32768, 请勿随意修改, htons最长为65535
#define LOCAL_MAX_RMEM (1<<16) 
#define LOCAL_MAX_SEND_COUNT 50 // local socket连续写次数

// read->wait->write->close
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
    int (*app_handler)(app_t*, async_server_t*);           // handler

    char *app_buf;                                         // buf
    size_t app_buf_len;                                    // buf长度
    size_t app_buf_cap;                                    // buf容量
    int app_timeout;
    uint64_t app_read_timestamp;

    union app_parser_t{
        http_parser http;
        local_protocol_parser local_protocol;            
    }app_parser;                                           // parser

    rbtree_node_t id_node;                                 // 唯一id, fd不可作为唯一id, 会被复用
    rbtree_node_t timer_node;                              // 定时器
    queue_t task_node;                                     // 分配队列, 对于http是任务，对于local是worker
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
    int id_fd;
    FILE *log_stream;
    char buf[0];
};

inline static void _localtime(uint64_t *millisecond){
    struct timeval tv;
    gettimeofday(&tv,NULL);
    *millisecond = tv.tv_sec*1000 + tv.tv_usec/1000;
}

inline static int _flush(async_server_t *server){
    return fflush(server->log_stream);
}

inline static int _write(async_server_t *server, const void *buf, size_t n){
    return fwrite(buf,1,n,server->log_stream);
}

inline static int _write_timestamp_string(async_server_t *server,const char *format, ...){
    va_list va;
    int len;

    struct timeval tv;
    struct tm tm_time;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec,&tm_time);

    len = fprintf(server->log_stream,"%04d-%02d-%02d %02d:%02d:%02d.%06ld ",
        tm_time.tm_year+1900,tm_time.tm_mon+1,tm_time.tm_mday, 
        tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, tv.tv_usec);

    va_start(va, format);
	len = vfprintf(server->log_stream, format, va);
	va_end(va);
    return len;
}

inline static int nonblock_socket(const int sock){
    int flags = fcntl(sock, F_GETFL);

    if (flags != -1){
        flags = fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    return flags;
}

inline static int accept_socket(async_server_t *server, int sock, struct sockaddr *addr, socklen_t *addrlen){
    int sockfd = -1;
    retry:
    sockfd = accept(sock,(struct sockaddr*)&addr,addrlen);
    if(sockfd < 0){
        if(errno == EINTR){
            // 被信号中断, 继续读
            goto retry;
        }else if(errno == ENFILE || errno == EMFILE){
            // fd不够分配，应该close, run out of file descriptors
            close(server->dummyfd);
            *addrlen = sizeof(addr);
            sockfd = accept(sock,(struct sockaddr*)&addr,addrlen);
            if(sockfd >= 0){
                close(sockfd);
            }
            server->dummyfd = open("/dev/null",O_RDONLY | O_CLOEXEC);
            goto retry;
        }else if(errno == EAGAIN || errno == EWOULDBLOCK){
            //数据未准备好, 下次继续读
            return -1;
        }else{
            return -1;
        }
    }
    return sockfd;
}

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

static int send_http_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }

    // 发送数据
    ssize_t len = send(app->app_fd, app->app_buf, app->app_buf_len, 0);
    if(len > 0){
        shutdown(app->app_fd,SHUT_WR);

        _write_timestamp_string(server,"[INFO] - id[%lu] response: len[%lu]",app->id_node.key,app->app_buf_len);
        _write(server,app->app_buf,app->app_buf_len);
        _write(server,"\n",1);

        app->app_status = app_status_close;
        return 0;
    }else{
        if(errno == EAGAIN){
            //写缓存满，等待下一次的写操作
            return 0;
        }else if(errno == EINTR){
            //被信号中断，等待下一次的写操作
            return 0;
        }else{
            _write_timestamp_string(server,"[ERROR] - id[%lu] send fail errno[%d]%s, line%d\n", app->id_node.key,errno,strerror(errno),__LINE__);
            app->app_status = app_status_close;
            return -1;
        }
    }
}

static int recv_http_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }
    
    // 扩容
    if((app->app_buf_len == app->app_buf_cap) && (app->app_buf_cap < REMOTE_MAX_BUF)){
        app->app_buf_cap <<= 1;
        char *p = app->app_buf;
        app->app_buf = realloc(app->app_buf,app->app_buf_cap);
        if(app->app_buf == NULL){
            free(p);
            _write_timestamp_string(server,"[ERROR] - id[%lu] realloc fail errno[%d]%s, line%d\n",app->id_node.key,errno,strerror(errno),__LINE__);
            app->app_status = app_status_close;
            return -1;
        }
    }
    
    // 接收数据
    ssize_t len = recv(app->app_fd, app->app_buf+app->app_buf_len, app->app_buf_cap-app->app_buf_len, 0);
    if(len > 0){
        _write_timestamp_string(server,"[INFO] - id[%lu] request: len[%lu]",app->id_node.key,app->app_buf_len+len);
        _write(server,app->app_buf,app->app_buf_len+len);
        _write(server,"\n",1);

        len = http_parser_execute(&app->app_parser.http, &settings_default, (char*)app->app_buf+app->app_buf_len,len);
        app->app_buf_len += len;
        
        // 格式错误
        if(app->app_parser.http.http_errno != HPE_OK){
            shutdown(app->app_fd, SHUT_RD);
            _write_timestamp_string(server,"[ERROR] - id[%lu] http format error %d\n",app->id_node.key,app->app_parser.http.http_errno);
            app->app_status = app_status_close;
            return -1;
        }
        
        // 已经读完数据
        if(http_request_is_done(&app->app_parser.http)){
            // 更改epoll的状态为监听socket错误
            struct epoll_event ev = {.events = EPOLLRDHUP, .data.ptr = app};
            epoll_ctl(server->epfd,EPOLL_CTL_MOD,app->app_fd,&ev);

            //更新状态
            app->app_status = app_status_wait;
            app->app_handler = send_http_cb;
            
            //插入到task任务队列尾
            queue_insert_tail(&server->task, &app->task_node);
        }
        return 0;
    }else if(len == 0){
        if(app->app_buf_len == app->app_buf_cap){
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv len too long %d, line%d\n",app->id_node.key,app->app_buf_len,__LINE__);
        }else{
            //对端关闭
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv close by peer, line%d\n",app->id_node.key,__LINE__);
        }
        app->app_status = app_status_close;
        return -1;
    }else{
        if(errno == EINTR){
            // 被信号打断，等待下一次读事件，不用操作
            return 0;
        }else if(errno == EAGAIN || errno == EWOULDBLOCK){
            // 数据未准备好，等待下一次读事件，不用操作
            return 0;
        }else{
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv fail errno[%d]%s, line%d\n", app->id_node.key,errno,strerror(errno),__LINE__);
            app->app_status = app_status_close;
            return -1;
        }
    }
}

static int accept_http_cb(app_t *app,async_server_t *server){
    // 已关闭的app
    if(app->app_status&app_status_close){
        return -1;
    }

    int sockfd,i;
    char ip_address[50];
    struct sockaddr_in addr;
    socklen_t addr_len;

    for(i=0;i<BACKLOG;i++){
        addr_len = sizeof(addr);
        sockfd = accept_socket(server,app->app_fd,(struct sockaddr*)&addr,&addr_len);
        if(sockfd < 0){
            _write_timestamp_string(server,"[WARN] - http accpet fail, errno[%d]%s\n",errno,strerror(errno));
            return -1;
        }

        // 非阻塞
        if(nonblock_socket(sockfd) == -1){
            _write_timestamp_string(server,"[WARN] - http F_GETFL fail, errno[%d]%s\n",errno,strerror(errno));
            close(sockfd);
            //return -1;
            continue;
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
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = app_sock};
        if(epoll_ctl(server->epfd,EPOLL_CTL_ADD,sockfd,&ev) == -1){
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
        app_sock->app_handler = recv_http_cb;

        app_sock->app_timeout = app->app_timeout;
        _localtime(&app_sock->app_read_timestamp);

        // 初始化parser
        http_parser_init(&app_sock->app_parser.http,HTTP_REQUEST);

        // id赋值，并初始化id结点
        app_sock->id_node.key = server->id++;
        rbtree_insert(&server->id_rbtree, &app_sock->id_node);

        //更新超时时间
        if(app_sock->app_timeout != -1){
            app_sock->timer_node.key = app_sock->app_read_timestamp + app_sock->app_timeout;
            rbtree_insert(&server->timer_rbtree,&app_sock->timer_node);
        }else{
            rbtree_node_init(&app_sock->timer_node);
        }

        //初始化队列
        queue_init(&app_sock->task_node);

        // 活跃连接数+1
        server->alive_comm++;

        _write_timestamp_string(server,"[INFO] - http accept %s:%d, id[%lu], alive_comm[%lu]\n",
            inet_ntop(AF_INET,&addr.sin_addr,ip_address,sizeof(ip_address)),ntohs(addr.sin_port),
            app_sock->id_node.key,
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
    char *p;
    size_t offset;

    len = recv(app->app_fd,app->app_buf+app->app_buf_len, app->app_buf_cap-app->app_buf_len,0);
    if(len > 0){
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
                    }
                    struct epoll_event ev = {.events = EPOLLOUT|EPOLLRDHUP, .data.ptr = app_task};
                    epoll_ctl(server->epfd,EPOLL_CTL_MOD,app_task->app_fd,&ev);

                    app_task->app_status = app_status_write;
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
        _write_timestamp_string(server,"[ERROR] - id[%lu] close by peer\n",app->id_node.key);
        app->app_status = app_status_close;
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
            _write_timestamp_string(server,"[ERROR] - id[%lu] recv fail errno[%d]%s, line%d\n",app->id_node.key,errno,strerror(errno),__LINE__);
            app->app_status = app_status_close;
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
        // 任务队列为空，不再等待写事件
        struct epoll_event ev = {.events = EPOLLRDHUP, .data.ptr = app};
        epoll_ctl(server->epfd,EPOLL_CTL_MOD,app->app_fd,&ev);
        app->app_status = app_status_wait;
        return 0;
    }

    // 取任务队列头结点
    queue_t *task;
    app_t *app_task;
    ssize_t len;
    int i;
    local_protocol_data_t *proto_data;
    for(i=0,task = queue_head(&server->task);task!=queue_sentinel(&server->task)&&i<LOCAL_MAX_SEND_COUNT;i++,task=queue_head(&server->task)){
        app_task = queue_data(task,app_t,task_node);

        if(app->app_buf_cap < sizeof(uint16_t) + sizeof(local_protocol_data_t) + app_task->app_buf_len){
            app->app_buf_cap = sizeof(uint16_t) + sizeof(local_protocol_data_t) + app_task->app_buf_len;
            char *p = app->app_buf;
            app->app_buf = realloc(app->app_buf,app->app_buf_cap);
            if(app->app_buf == NULL){
                free(p);
                app->app_buf_len = app->app_buf_cap = 0;
                _write_timestamp_string(server,"[ERROR] %s realloc fail\n",__FUNCTION__);
                return -1;
            }
        }

        // local_protocol_data前两位为长度
        proto_data = (local_protocol_data_t *)(app->app_buf + sizeof(uint16_t));
        proto_data->id = app_task->id_node.key;
        proto_data->clock = app_task->app_read_timestamp;
        proto_data->data_type = app_task->app_data_type;
        // 赋值请求数据
        memcpy(proto_data->data,app_task->app_buf,app_task->app_buf_len);
        
        *(uint16_t*)app->app_buf = htons(sizeof(local_protocol_data_t)+app_task->app_buf_len);
        app->app_buf_len = sizeof(uint16_t)+sizeof(local_protocol_data_t)+app_task->app_buf_len;

        len = send(app->app_fd,app->app_buf,app->app_buf_len,0);
        if(len > 0){
            //发送成功，任务从队列中取出
            queue_remove(&app_task->task_node);
            _write_timestamp_string(server,"[INFO] - local send id[%lu] finish\n",app_task->id_node.key);
            // return 0;
        }else{
            if(errno == EAGAIN){
                return 0;
                // break;
            }else if(errno == EINTR){
                return 0;
                // break;
            }else{
                _write_timestamp_string(server,"[ERROR] - id[%lu] send fail errno[%d]%s, line%d\n",app->id_node.key,errno,strerror(errno),__LINE__);
                app->app_status = app_status_close;
                return -1;
            }
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
    sockfd = accept_socket(server,app->app_fd,(struct sockaddr*)&addr,&addr_len);
    if(sockfd < 0){
        _write_timestamp_string(server,"[WARN] - local accpet fail, errno[%d]%s\n",errno,strerror(errno));
        return -1;
    }
    
    // 非阻塞
    if(nonblock_socket(sockfd) == -1){
        _write_timestamp_string(server,"[WARN] - local F_GETFL fail, errno[%d]%s\n",errno,strerror(errno));
        close(sockfd);
        return -1;
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

    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = app_sock};
    if(epoll_ctl(server->epfd,EPOLL_CTL_ADD,sockfd,&ev) == -1){
        _write_timestamp_string(server,"[WARN] - local epoll_ctl fail, errno[%d]%s\n",errno,strerror(errno));
        close(sockfd);
        free(app_sock->app_buf);
        free(app_sock);
        return -1;
    }

    app_sock->app_fd = sockfd;
    app_sock->app_data_type = app->app_data_type;
    app_sock->app_status = app_status_read;
    app_sock->app_handler = recv_local_cb;

    app_sock->app_timeout = app->app_timeout;
    _localtime(&app_sock->app_read_timestamp);

    // 初始化parser
    local_protocol_parser_init(&app_sock->app_parser.local_protocol);

    // id赋值，并初始化id结点
    app_sock->id_node.key = server->id++;
    rbtree_insert(&server->id_rbtree,&app_sock->id_node);

    // 不添加到timer
    rbtree_node_init(&app_sock->timer_node);

    // 不添加到worker队列
    queue_init(&app_sock->task_node);

    // 活跃连接数+1
    server->alive_comm++;

    _write_timestamp_string(server,"[INFO] - local accept %s:%d, id[%lu], alive_comm[%lu]\n",
        inet_ntop(AF_INET,&addr.sin_addr,ip_address,sizeof(ip_address)),ntohs(addr.sin_port),
        app_sock->id_node.key,
        server->alive_comm
    );

    {
        sockfd = dup(sockfd);
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

        struct epoll_event ev = {.events = EPOLLOUT|EPOLLRDHUP, .data.ptr = app_sock};
        if(epoll_ctl(server->epfd,EPOLL_CTL_ADD,sockfd,&ev) == -1){
            _write_timestamp_string(server,"[WARN] - local epoll_ctl fail, errno[%d]%s\n",errno,strerror(errno));
            close(sockfd);
            free(app_sock->app_buf);
            free(app_sock);
            return -1;
        }

        app_sock->app_fd = sockfd;
        app_sock->app_data_type = app->app_data_type;
        app_sock->app_status = app_status_write;
        app_sock->app_handler = send_local_cb;

        app_sock->app_timeout = app->app_timeout;
        _localtime(&app_sock->app_read_timestamp);

        // 初始化parser
        local_protocol_parser_init(&app_sock->app_parser.local_protocol);

        // id赋值，并初始化id结点
        app_sock->id_node.key = server->id++;
        rbtree_insert(&server->id_rbtree,&app_sock->id_node);

        // 不添加到timer
        rbtree_node_init(&app_sock->timer_node);

        // 添加到worker队列
        queue_insert_tail(&server->worker,&app_sock->task_node);

        // 活跃连接数+1
        server->alive_comm++;

        _write_timestamp_string(server,"[INFO] - local accept %s:%d, id[%lu], alive_comm[%lu]\n",
            inet_ntop(AF_INET,&addr.sin_addr,ip_address,sizeof(ip_address)),ntohs(addr.sin_port),
            app_sock->id_node.key,
            server->alive_comm
        );
    }

    return 0;
}
// --------------local----------------------------

async_server_t* server_create(const char* id_file, const char *log_file, size_t buf_size){
    if(buf_size < BUFSIZ){
        buf_size = BUFSIZ;
    }

    async_server_t *server = malloc(sizeof(async_server_t) + buf_size);
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
    server->id_fd = -1;
    server->log_stream = NULL;

    if(id_file != NULL){
        server->id_fd = open(id_file,O_RDWR|O_CREAT,0664);
        if(server->id_fd >= 0){
            if(read(server->id_fd,&server->id,sizeof(server->id)) != sizeof(server->id)){
                server->id = 0;
            }
        }
    }

    if(log_file != NULL){
        server->log_stream = fopen(log_file,"a");
    }
    if(server->log_stream == NULL){
        server->log_stream = stderr;
    }
    setvbuf(server->log_stream,server->buf,_IOFBF,buf_size);

    return server;
}

int server_start_loop(async_server_t* server, int maxapps){
    if(server == NULL){
        return -1;
    }

    rbtree_node_t* node;
    queue_t *qnode;
    app_t *app;
    int ev_nready, timeout, i;
    uint64_t now_millisecond;

    server->ev_events = malloc(sizeof(struct epoll_event) * maxapps);
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

            app->app_status = app_status_close;
            _write_timestamp_string(server,"[ERROR] - TIMEOUT!! id[%lu]\n",app->id_node.key);
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
        
        if(node == NULL || node->key == (uint64_t)-1){
            timeout = -1;
        }else{
            // 计算将要过期的app时间差
            timeout = node->key - now_millisecond;
        }

        // 等待事件触发或者超时
        ev_nready = epoll_wait(server->epfd,server->ev_events,maxapps,timeout);
        if(ev_nready == 0){
            //超时
        }else if(ev_nready == -1){
            if(errno == EINTR){
                //被信号中断
            }
        }else{
            for(i=0;i<ev_nready;i++){
                app = (app_t*)server->ev_events[i].data.ptr;

                // 读事件 写事件
                if((server->ev_events[i].events & EPOLLIN) 
                    || (server->ev_events[i].events & EPOLLOUT)){
                    app->app_handler(app,server);
                }
                
                if(server->ev_events[i].events & EPOLLHUP){// epoll默认监听这个参数
                    // 对端关闭
                    app->app_status = app_status_close;
                    _write_timestamp_string(server,"[ERROR] - id[%lu] EPOLLHUP close by peer\n",app->id_node.key);
                }

                if(server->ev_events[i].events & EPOLLRDHUP){
                    // 对端关闭或者调用shutdown write
                    app->app_status = app_status_close;
                    _write_timestamp_string(server,"[ERROR] - id[%lu] EPOLLRDHUP\n",app->id_node.key);
                }

                if(app->app_status&app_status_close){
                    _write_timestamp_string(server,"[INFO] - FINISH!! id[%lu]alive_comm[%lu]\n",app->id_node.key,server->alive_comm);

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
                    struct epoll_event ev = {.events = EPOLLOUT|EPOLLRDHUP, .data.ptr = app};
                    epoll_ctl(server->epfd,EPOLL_CTL_MOD,app->app_fd,&ev);
                    app->app_status=app_status_write;
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

    //释放id队列的所有app
    rbtree_node_t* node;
    app_t *app;
    node = rbtree_min(&server->id_rbtree,server->id_rbtree.root);
    while(node != NULL){
        app = rbtree_data(node,app_t,id_node);
        node = rbtree_next(&server->id_rbtree,node);

        app->app_status = app_status_close;

        _write_timestamp_string(server,"[INFO] - FINISH!! id[%lu]\n",app->id_node.key);

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
    }

    _flush(server);

    close(server->epfd);
    close(server->dummyfd);

    if(server->id_fd != -1){
        lseek(server->id_fd,0,SEEK_SET);
        if(write(server->id_fd,&server->id,sizeof(server->id)) == -1){}
        close(server->id_fd);
    }
    fclose(server->log_stream);

    free(server->ev_events);
    free(server);
    return;
}

int add_sockets(async_server_t* server, struct sockaddr *addr, socklen_t len, int timeout, uint8_t data_type){
    if(server == NULL){
        return -1;
    }

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

    if(nonblock_socket(fd) == -1){
        fprintf(stderr,"F_SETFL O_NONBLOCK fail errno[%d]%s\n",errno,strerror(errno));
        close(fd);
        return -1;
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

    app_t *app = malloc(sizeof(app_t));
    if(app == NULL){
        fprintf(stderr,"malloc fail errno[%d]%s\n",errno,strerror(errno));
        close(fd);
        return -1;
    }

    app->app_fd = fd;

    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = app};
    if(epoll_ctl(server->epfd,EPOLL_CTL_ADD,fd,&ev) == -1){
        close(fd);
        free(app);
        return -1;
    }

    app->app_data_type = data_type;
    if(data_type == DATA_TYPE_HTTP){
        app->app_handler = accept_http_cb;
    }else if(data_type == DATA_TYPE_LOCAL){
        app->app_handler = accept_local_cb;
    }else{
        app->app_handler = NULL;
    }
    app->app_status = app_status_read;

    app->app_buf = NULL;
    app->app_buf_len = 0;
    app->app_buf_cap = 0;

    app->app_timeout = timeout;

    // id赋值，并初始化id结点
    app->id_node.key = server->id++;
    rbtree_insert(&server->id_rbtree,&app->id_node);
    
    // 不加入timer
    rbtree_node_init(&app->timer_node);

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