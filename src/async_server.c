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

#include "async_server.h"
#include "rbtree.h"
#include "queue.h"
#include "http_parser.h"
#include "iso8583_parser.h"
#include "local_protocol.h"

#define BUF_MIN (1<<8)  //256
#define BUF_MAX (1<<15) //32768

//对于remote, 流程为read->wait(queue write)->wait(queue read)->write
//对于local, 流程为read/write
enum app_status_t{
    app_status_read   = (1<<0),
    app_status_write  = (1<<1),
    app_status_wait   = (1<<2),
    app_status_close  = (1<<3),
};

typedef struct app_s app_t;
// typedef struct async_server_s async_server_t;

struct app_s{
    int app_fd;                                            // fd
    enum data_type_t app_data_type;                        // 数据类型
    enum app_status_t app_status;                          // 状态
    int app_errno;                                         // 错误码
    ssize_t (*app_read_handler)(app_t* ,async_server_t*);  // read的handler
    ssize_t (*app_write_handler)(app_t* ,async_server_t*); // write的handler

    uint64_t app_read_timestamp;                           // 开始读的时间
    uint64_t app_write_timestamp;                          // 开始写的时间
    uint64_t app_wait_timestamp;                           // 开始等待的时间
    uint64_t app_close_timestamp;                          // 关闭的时间

    unsigned char *app_buf;                                // buf
    size_t app_buf_len;                                    // buf长度
    size_t app_buf_cap;                                    // buf容量

    int app_read_timeout;                                  // 超时时间, ms为单位, -1为infinite
    int app_write_timeout;                                 // 超时时间, ms为单位, -1为infinite
    
    union app_parser_t{
        http_parser http;
        iso8583_parser iso8583;
        local_protocol_parser local_protocol;            
    }app_parser;                                           // 

    rbtree_node_t id_rbtree_node;                          // key为id
    rbtree_node_t timer_rbtree_node;                       // key为到期时间
    queue_t task_worker_queue_node;                        // 队列节点,对于remote，是task队列；对于local，是worker队列
};

struct async_server_s{
    int epfd;                                              // epoll根
    rbtree_t id_rbtree;                                    // id根
    rbtree_t timer_rbtree;                                 // 定时器根
    queue_t worker;                                        // 待分配worker队列
    queue_t task;                                          // 待处理任务队列
    int dummyfd;                                           // for run out of file descriptors (because of resource limits)
    uint64_t id;                                           // id
    char *id_file;                                         // id文件, 初始化读id文件获取id, 关闭server写入id文件
    char *log_file;                                        // log文件
    ssize_t (*write_log)(const char* file_name,const void *buf, size_t n);
};

uint64_t localtime(){
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec*1000 + tv.tv_usec/1000;
}

// --------------iso8583----------------------------
ssize_t recv_iso8583_cb(app_t *app,async_server_t *server){
    if(app->app_status&app_status_close){
        return -1;
    }

    if(app->app_buf_len == app->app_buf_cap && app->app_buf_cap < BUF_MAX){
        app->app_buf_cap<<=1;
        app->app_buf = realloc(app->app_buf,app->app_buf_cap);
        if(app->app_buf == NULL){
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }
    }
    
    int len = recv(app->app_fd,app->app_buf+app->app_buf_len, app->app_buf_cap-app->app_buf_len,0);
    if(len > 0){
        len = iso8583_parser_execute(&app->app_parser.iso8583,app->app_buf+app->app_buf_len,len);
        app->app_buf_len += len;

        if(iso8583_parser_is_done(&app->app_parser.iso8583)){
            //已经读完数据，不再处于epoll，进入等待阶段
            struct epoll_event ev = {0,{0}};
            int opt = EPOLL_CTL_MOD;
            ev.events = EPOLLERR|EPOLLHUP|EPOLLRDHUP;
            ev.data.ptr = app;
            epoll_ctl(server->epfd,opt,app->app_fd,&ev);

            app->app_status = app_status_wait;
            app->app_wait_timestamp = localtime();

            //重新设置超时时间
            rbtree_delete(&server->timer_rbtree,&app->timer_rbtree_node);
            if(app->app_write_timeout == -1)
                app->timer_rbtree_node.key = -1;
            else
                app->timer_rbtree_node.key = app->app_wait_timestamp + app->app_write_timeout;
            rbtree_insert(&server->timer_rbtree,&app->timer_rbtree_node);

            //插入到task任务队列尾
            queue_insert_tail(&server->task,&app->task_worker_queue_node);

            //唤醒所有worker处于可写状态（默认可读）
            queue_t *worker;
            app_t *app_worker;
            for(worker=queue_head(&server->worker);worker!=&server->worker;worker=queue_next(worker)){
                app_worker = queue_data(worker,app_t,task_worker_queue_node);
                if(!(app_worker->app_status & app_status_write)){
                    struct epoll_event ev = {0,{0}};
                    int opt = EPOLL_CTL_MOD;
                    ev.events = EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
                    ev.data.ptr = app_worker;
                    epoll_ctl(server->epfd,opt,app_worker->app_fd,&ev);
                    app_worker->app_status |= app_status_write;
                }
                break;
            }
        }
        return 0;
    }else if(len == 0){
        //对端关闭
        app->app_errno = errno;
        app->app_status |= app_status_close;
        app->app_close_timestamp = localtime();
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
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }
    }
}

ssize_t send_iso8583_cb(app_t *app,async_server_t *server){
    if(app->app_status&app_status_close){
        return -1;
    }

    int len = send(app->app_fd,app->app_buf,app->app_buf_len,0);

    if(len > 0){
        //发送完毕
        app->app_errno =0;
        app->app_close_timestamp = app->app_write_timeout;
        app->app_status |= app_status_close;
        return 0;
    }else{
        if(errno == EAGAIN){
            //写缓存满，等待下一次的写操作
            return 0;
        }else if(errno == EINTR){
            //被信号中断，等待下一次的写操作
            return 0;
        }else if(errno == EPIPE){
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }else{
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }
    }
}

ssize_t accept_iso8583_cb(app_t *app,async_server_t *server){
    if(app->app_status&app_status_close){
        return -1;
    }

    int sockfd;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    sockfd = accept(app->app_fd,(struct sockaddr*)&addr,&addr_len);
    if(sockfd < 0){
        if(errno == EINTR){
            //被信号中断, 下次继续读
            return -1;
        }else if(errno == ENFILE || errno == EMFILE){
            //fd不够分配，应该close
            //LOG_ERROR("run out of file descriptors %d,%s",errno,strerror(errno));
            close(server->dummyfd);
            sockfd = accept(app->app_fd,(struct sockaddr*)&addr,&addr_len);
            if(sockfd >= 0){
                close(sockfd);
            }
            server->dummyfd = open("/dev/null",O_RDONLY|O_CLOEXEC);
            return -1;
        }else if(errno == EAGAIN || errno == EWOULDBLOCK){
            //数据未准备好, 下次继续读
            return -1;
        }else{
            //LOG_ERROR("%d,%s",errno,strerror(errno));
        }
        return -1;
    }

    //非阻塞
    int flag = fcntl(sockfd,F_GETFL);
    if(flag == -1){
        close(sockfd);
        return -1;
    }

    if(!(flag & O_NONBLOCK)){
        if(fcntl(sockfd,F_SETFL,flag | O_NONBLOCK) == -1){
            close(sockfd);
            return -1;
        }
    }

    app_t *app_sock = malloc(sizeof(app_t));
    if(app_sock == NULL){
        close(sockfd);
        return -1;
    }

    app_sock->app_buf_len = 0;
    app_sock->app_buf_cap = BUF_MIN;
    app_sock->app_buf = malloc(app_sock->app_buf_cap);
    if(app_sock->app_buf == NULL){
        close(sockfd);
        return -1;
    }

    struct epoll_event ev = {0,{0}};
    int opt = EPOLL_CTL_ADD;
    ev.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
    ev.data.ptr = app_sock;

    if(epoll_ctl(server->epfd,opt,sockfd,&ev) == -1){
        if(errno == EPERM){
        }else if(errno == ENOENT){
        }else if(errno == EEXIST){
        }
        close(sockfd);
        free(app_sock->app_buf);
        free(app_sock);
        return -1;
    }

    app_sock->app_fd = sockfd;
    app_sock->app_data_type = data_type_iso8583;
    app_sock->app_status = app_status_read;
    app_sock->app_errno = 0;
    app_sock->app_read_handler = recv_iso8583_cb;
    app_sock->app_write_handler = send_iso8583_cb;

    app_sock->app_read_timestamp = localtime();
    app_sock->app_write_timestamp = 0;
    app_sock->app_wait_timestamp = 0;
    app_sock->app_close_timestamp = 0;
    
    app_sock->app_read_timeout = app->app_read_timeout;
    app_sock->app_write_timeout = app->app_write_timeout;

    iso8583_parser_init(&app_sock->app_parser.iso8583);

    //添加id
    app_sock->id_rbtree_node.key = server->id++;
    rbtree_insert(&server->id_rbtree,&app_sock->id_rbtree_node);
    //更新超时时间
    if(app_sock->app_read_timeout == -1)
        app_sock->timer_rbtree_node.key = -1;
    else
        app_sock->timer_rbtree_node.key = app_sock->app_read_timestamp + app_sock->app_read_timeout;
    rbtree_insert(&server->timer_rbtree,&app_sock->timer_rbtree_node);
    //初始化队列
    queue_init(&app_sock->task_worker_queue_node);

    return 0;
}
// --------------iso8583----------------------------

// --------------http----------------------------
ssize_t recv_http_cb(app_t *app,async_server_t *server){
    size_t len;
    
    if(app->app_status&app_status_close){
        return -1;
    }
    
    if(app->app_buf_len == app->app_buf_cap && app->app_buf_cap < BUF_MAX){
        app->app_buf_cap<<=1;
        app->app_buf = realloc(app->app_buf,app->app_buf_cap);
        if(app->app_buf == NULL){
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }
    }
    
    len = recv(app->app_fd,app->app_buf+app->app_buf_len, app->app_buf_cap-app->app_buf_len,0);
    if(len > 0){
        len = http_parser_execute(&app->app_parser.http, &settings_default, app->app_buf+app->app_buf_len,len);
        app->app_buf_len += len;
        
        if(app->app_parser.http.http_errno != HPE_OK){
            app->app_errno = ECONNABORTED;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }
        
        if(http_request_is_done(&app->app_parser.http)){
            //已经读完数据，不再处于epoll，进入等待阶段
            struct epoll_event ev = {0,{0}};
            int opt = EPOLL_CTL_MOD;
            ev.events = EPOLLERR|EPOLLHUP|EPOLLRDHUP;
            ev.data.ptr = app;
            epoll_ctl(server->epfd,opt,app->app_fd,&ev);

            app->app_status = app_status_wait;
            app->app_wait_timestamp = localtime();
            
            //重新设置超时时间
            rbtree_delete(&server->timer_rbtree,&app->timer_rbtree_node);
            if(app->app_write_timeout == -1)
                app->timer_rbtree_node.key = -1;
            else
                app->timer_rbtree_node.key = app->app_wait_timestamp + app->app_write_timeout;
            rbtree_insert(&server->timer_rbtree,&app->timer_rbtree_node);
            
            //插入到task任务队列尾
            queue_insert_tail(&server->task,&app->task_worker_queue_node);
            
            //唤醒所有worker处于可写状态（默认可读）
            queue_t *worker;
            app_t *app_worker;
            for(worker=queue_head(&server->worker);worker!=&server->worker;worker=queue_next(worker)){
                app_worker = queue_data(worker,app_t,task_worker_queue_node);
                if(!(app_worker->app_status & app_status_write)){
                    struct epoll_event ev = {0,{0}};
                    int opt = EPOLL_CTL_MOD;
                    ev.events = EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
                    ev.data.ptr = app_worker;
                    epoll_ctl(server->epfd,opt,app_worker->app_fd,&ev);
                    app_worker->app_status |= app_status_write;
                }
                break;
            }
            
        }
        return 0;
    }else if(len == 0){
        //对端关闭
        app->app_errno = errno;
        app->app_status |= app_status_close;
        app->app_close_timestamp = localtime();
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
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }
    }
}

ssize_t send_http_cb(app_t *app,async_server_t *server){
    if(app->app_status&app_status_close){
        return -1;
    }

    int len = send(app->app_fd,app->app_buf,app->app_buf_len,0);

    if(len > 0){
        //发送完毕
        app->app_errno =0;
        app->app_close_timestamp = app->app_write_timeout;
        app->app_status |= app_status_close;
        return 0;
    }else{
        if(errno == EAGAIN){
            //写缓存满，等待下一次的写操作
            return 0;
        }else if(errno == EINTR){
            //被信号中断，等待下一次的写操作
            return 0;
        }else if(errno == EPIPE){
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }else{
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }
    }
}

ssize_t accept_http_cb(app_t *app,async_server_t *server){
    if(app->app_status&app_status_close){
        return -1;
    }

    int sockfd;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    sockfd = accept(app->app_fd,(struct sockaddr*)&addr,&addr_len);
    if(sockfd < 0){
        if(errno == EINTR){
            //被信号中断, 下次继续读
            return -1;
        }else if(errno == ENFILE || errno == EMFILE){
            //fd不够分配，应该close
            //LOG_ERROR("run out of file descriptors %d,%s",errno,strerror(errno));
            close(server->dummyfd);
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
            //LOG_ERROR("%d,%s",errno,strerror(errno));
        }
        return -1;
    }

    //非阻塞
    int flag = fcntl(sockfd,F_GETFL);
    if(flag == -1){
        close(sockfd);
        return -1;
    }

    if(!(flag & O_NONBLOCK)){
        if(fcntl(sockfd,F_SETFL,flag | O_NONBLOCK) == -1){
            close(sockfd);
            return -1;
        }
    }
    
    app_t *app_sock = malloc(sizeof(app_t));
    if(app_sock == NULL){
        close(sockfd);
        return -1;
    }
    
    app_sock->app_buf_len = 0;
    app_sock->app_buf_cap = BUF_MIN;
    app_sock->app_buf = malloc(app_sock->app_buf_cap);
    if(app_sock->app_buf == NULL){
        close(sockfd);
        return -1;
    }
    
    struct epoll_event ev = {0,{0}};
    int opt = EPOLL_CTL_ADD;
    ev.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
    ev.data.ptr = app_sock;

    if(epoll_ctl(server->epfd,opt,sockfd,&ev) == -1){
        if(errno == EPERM){
        }else if(errno == ENOENT){
        }else if(errno == EEXIST){
        }
        close(sockfd);
        free(app_sock->app_buf);
        free(app_sock);
        return -1;
    }
    
    app_sock->app_fd = sockfd;
    app_sock->app_data_type = data_type_http;
    app_sock->app_status = app_status_read;
    app_sock->app_errno = 0;
    app_sock->app_read_handler = recv_http_cb;
    app_sock->app_write_handler = send_http_cb;

    app_sock->app_read_timestamp = localtime();
    app_sock->app_write_timestamp = 0;
    app_sock->app_wait_timestamp = 0;
    app_sock->app_close_timestamp = 0;
    
    app_sock->app_read_timeout = app->app_read_timeout;
    app_sock->app_write_timeout = app->app_write_timeout;

    http_parser_init(&app_sock->app_parser.http,HTTP_REQUEST);

    //添加id
    app_sock->id_rbtree_node.key = server->id++;
    rbtree_insert(&server->id_rbtree,&app_sock->id_rbtree_node);
    //更新超时时间
    if(app_sock->app_read_timeout == -1)
        app_sock->timer_rbtree_node.key = -1;
    else
        app_sock->timer_rbtree_node.key = app_sock->app_read_timestamp + app_sock->app_read_timeout;
    rbtree_insert(&server->timer_rbtree,&app_sock->timer_rbtree_node);
    //初始化队列
    queue_init(&app_sock->task_worker_queue_node);
    
    return 0;
}
// --------------http----------------------------

// --------------local----------------------------
ssize_t recv_local_protocol_cb(app_t *app,async_server_t *server){
    if(app->app_status&app_status_close){
        return -1;
    }

    if(app->app_buf_len == app->app_buf_cap && app->app_buf_cap < BUF_MAX){
        app->app_buf_cap<<=1;
        app->app_buf = realloc(app->app_buf,app->app_buf_cap);
        if(app->app_buf == NULL){
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }
    }

    int len,len_parser;
    local_protocol_data_t *local_protocol_data;
    int local_protocol_data_size;
    len = recv(app->app_fd,app->app_buf+app->app_buf_len, app->app_buf_cap-app->app_buf_len,0);
    if(len > 0){
        len_parser = 0;
        while(len_parser < len){
            len_parser = local_protocol_parser_execute(&app->app_parser.local_protocol,app->app_buf+app->app_buf_len,len);
            app->app_buf_len += len_parser;
            if(local_protocol_parser_is_done(&app->app_parser.local_protocol)){
                //读完数据
                //ipc头两位为长度
                local_protocol_data = (local_protocol_data_t *)(app->app_buf+sizeof(uint16_t));
                local_protocol_data_size = app->app_buf_len - sizeof(uint16_t) - sizeof(local_protocol_data_t);

                //根据id查找原fd
                rbtree_node_t *rbtree_node = rbtree_search(&server->id_rbtree,local_protocol_data->id);
                if(rbtree_node != NULL){
                    //fd未被关闭，获得原ev
                    app_t* app_task = rbtree_data(rbtree_node,app_t,id_rbtree_node);
                    if(local_protocol_data->data_type == data_type_iso8583){
                        //8583前两位为长度
                        if(app_task->app_buf_cap < local_protocol_data_size+sizeof(uint16_t)){
                            app_task->app_buf_cap = local_protocol_data_size+sizeof(uint16_t);
                            app_task->app_buf = realloc(app_task->app_buf,app_task->app_buf_cap);
                        }
                        if(app_task->app_buf != NULL){
                            memcpy(app_task->app_buf + sizeof(uint16_t),local_protocol_data->data,local_protocol_data_size);
                            *(uint16_t*)app_task->app_buf = htons(local_protocol_data_size);
                            app_task->app_buf_len = sizeof(uint16_t) + local_protocol_data_size;
                        }
                    }else{
                        if(app_task->app_buf_cap < local_protocol_data_size){
                            app_task->app_buf_cap = local_protocol_data_size;
                            app_task->app_buf = realloc(app_task->app_buf,app_task->app_buf_cap);
                        }
                        if(app_task->app_buf != NULL){
                            memcpy(app_task->app_buf,local_protocol_data->data,local_protocol_data_size);
                            app_task->app_buf_len = local_protocol_data_size;
                        }
                    }

                    //原ev添加写事件，重新加入epoll，原本应该是处于waiting状态
                    struct epoll_event ev = {0,{0}};
                    int opt = EPOLL_CTL_MOD;
                    ev.events = EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
                    ev.data.ptr = app_task;
                    epoll_ctl(server->epfd,opt,app_task->app_fd,&ev);
                    if(app_task->app_buf != NULL){
                        app_task->app_status = app_status_write;
                        app_task->app_write_timestamp = localtime();
                    }else{
                        app_task->app_errno = errno;
                        app_task->app_status = app_status_close;
                        app_task->app_write_timestamp = localtime();
                        app_task->app_close_timestamp = app_task->app_write_timestamp;
                    }
                }
                //移除已解析的数据
                memmove(app->app_buf,app->app_buf+app->app_buf_len,len-len_parser);
                len -= len_parser;
                len_parser = 0;
                app->app_buf_len = 0;
            }
        }
        return 0;
    }else if(len == 0){
        //对端关闭
        app->app_errno = errno;
        app->app_status |= app_status_close;
        app->app_close_timestamp = localtime();
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
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }
    }
}

ssize_t send_local_protocol_cb(app_t *app,async_server_t *server){
    static char buf[BUF_MAX] = {0};
    
    if(app->app_status&app_status_close){
        return -1;
    }
    
    if(queue_empty(&server->task)){
        //任务队列为空，不再等待写事件，只保留读事件
        struct epoll_event ev = {0,{0}};
        int opt = EPOLL_CTL_MOD;
        ev.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
        ev.data.ptr = app;
        epoll_ctl(server->epfd,opt,app->app_fd,&ev);
        app->app_status = app_status_read;
        return 0;
    }
    
    //取任务队列头结点
    queue_t *task = queue_head(&server->task);
    app_t * app_task = queue_data(task,app_t,task_worker_queue_node);
    int len;

    //local_protocol_data前两位为长度
    local_protocol_data_t *local_protocol_data;
    local_protocol_data = (local_protocol_data_t *)(buf + sizeof(uint16_t));
    local_protocol_data->id = app_task->id_rbtree_node.key;
    local_protocol_data->clock = app_task->app_read_timestamp;
    local_protocol_data->data_type = app_task->app_data_type;
    if(app_task->app_data_type == data_type_iso8583){
        //8583前两位为长度,需要去掉
        memcpy(local_protocol_data->data,app_task->app_buf+sizeof(uint16_t),app_task->app_buf_len-sizeof(uint16_t));
        *(uint16_t*)buf = htons(sizeof(local_protocol_data_t)+app_task->app_buf_len-sizeof(uint16_t));
        len = sizeof(local_protocol_data_t)+app_task->app_buf_len;//+sizeof(uint16_t)ipc头 - sizeof(uint16_t)8583头
    }else{
        memcpy(local_protocol_data->data,app_task->app_buf,app_task->app_buf_len);
        *(uint16_t*)buf = htons(sizeof(local_protocol_data_t)+app_task->app_buf_len);
        len = sizeof(uint16_t)+sizeof(local_protocol_data_t)+app_task->app_buf_len;
    }
    
    len = send(app->app_fd,buf,len,0);
    
    if(len > 0){
        //发送成功，任务从队列中取出
        queue_remove(&app_task->task_worker_queue_node);

        //将worker放到队列尾
        queue_remove(&app->task_worker_queue_node);
        queue_insert_tail(&server->worker,&app->task_worker_queue_node);

        return 0;
    }else{
        if(errno == EAGAIN){
            //写缓存满，等待下一次的写操作
            return 0;
        }else if(errno == EINTR){
            //被信号中断，等待下一次的写操作
            return 0;
        }else if(errno == EPIPE){
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }else{
            app->app_errno = errno;
            app->app_status |= app_status_close;
            app->app_close_timestamp = localtime();
            return -1;
        }
    }
}

ssize_t accept_local_protocol_cb(app_t *app,async_server_t *server){
    if(app->app_status&app_status_close){
        return -1;
    }

    int sockfd;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    sockfd = accept(app->app_fd,(struct sockaddr*)&addr,&addr_len);
    if(sockfd < 0){
        if(errno == EINTR){
            //被信号中断, 下次继续读
            return -1;
        }else if(errno == ENFILE || errno == EMFILE){
            //fd不够分配，应该close
            //LOG_ERROR("run out of file descriptors %d,%s",errno,strerror(errno));
            close(server->dummyfd);
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
            //LOG_ERROR("%d,%s",errno,strerror(errno));
        }
        return -1;
    }
    
    //非阻塞
    int flag = fcntl(sockfd,F_GETFL);
    if(flag == -1){
        close(sockfd);
        return -1;
    }

    if(!(flag & O_NONBLOCK)){
        if(fcntl(sockfd,F_SETFL,flag | O_NONBLOCK) == -1){
            close(sockfd);
            return -1;
        }
    }

    app_t *app_sock = malloc(sizeof(app_t));
    if(app_sock == NULL){
        close(sockfd);
        return -1;
    }

    app_sock->app_buf_len = 0;
    app_sock->app_buf_cap = BUF_MIN;
    app_sock->app_buf = malloc(app_sock->app_buf_cap);
    if(app_sock->app_buf == NULL){
        close(sockfd);
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
        close(sockfd);
        free(app_sock->app_buf);
        free(app_sock);
        return -1;
    }

    app_sock->app_fd = sockfd;
    app_sock->app_data_type = data_type_local_protocol;
    app_sock->app_status = app_status_read | app_status_write;
    app_sock->app_errno = 0;
    app_sock->app_read_handler = recv_local_protocol_cb;
    app_sock->app_write_handler = send_local_protocol_cb;

    app_sock->app_read_timestamp = localtime();
    app_sock->app_write_timestamp = app_sock->app_read_timestamp;
    app_sock->app_wait_timestamp = 0;
    app_sock->app_close_timestamp = 0;
    
    app_sock->app_read_timeout = app->app_read_timeout;
    app_sock->app_write_timeout = app->app_write_timeout;

    local_protocol_parser_init(&app_sock->app_parser.local_protocol);

    //添加id
    app_sock->id_rbtree_node.key = server->id++;
    rbtree_insert(&server->id_rbtree,&app_sock->id_rbtree_node);
    //更新超时时间
    if(app_sock->app_read_timeout == -1)
        app_sock->timer_rbtree_node.key = -1;
    else
        app_sock->timer_rbtree_node.key = app_sock->app_read_timestamp + app_sock->app_read_timeout;
    rbtree_insert(&server->timer_rbtree,&app_sock->timer_rbtree_node);
    //添加到worker队列
    queue_insert_tail(&server->worker,&app_sock->task_worker_queue_node);
    
    return 0;
}
// --------------local----------------------------

async_server_t* server_create(){
    async_server_t *server = malloc(sizeof(async_server_t));
    if(server == NULL){
        return NULL;
    }
    server->epfd = epoll_create(1);
    if(server->epfd == -1){
        free(server);
        return NULL;
    }
    rbtree_init(&server->id_rbtree);
    rbtree_init(&server->timer_rbtree);
    queue_init(&server->worker);
    queue_init(&server->task);
    server->dummyfd = open("/dev/null",O_RDONLY | O_CLOEXEC);
    server->id = 0;
    server->id_file = NULL;
    server->log_file = NULL;
    server->write_log = NULL;

    return server;
}

ssize_t server_set_id_file(async_server_t* server,const char *file_name){
    if(server == NULL){
        return -1;
    }

    if(server->id_file != NULL){
        free(server->id_file);
    }

    int file_name_len = strlen(file_name);
    server->id_file = malloc(file_name_len+1);
    strcpy(server->id_file,file_name);
    server->id_file[file_name_len] = 0;

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

ssize_t server_set_log_file(async_server_t* server,const char *file_name,ssize_t (*write_log)(const char* file_name,const void *buf, size_t n)){
    if(server == NULL){
        return -1;
    }
    if(server->log_file != NULL){
        free(server->log_file);
    }

    int file_name_len = strlen(file_name);
    server->log_file = malloc(file_name_len+1);
    strcpy(server->log_file,file_name);
    server->log_file[file_name_len] = 0;
    
    server->write_log = write_log;
    return 0;
}

ssize_t server_start_loop(async_server_t* server){
    if(server == NULL){
        return -1;
    }

    rbtree_node_t* node;
    struct epoll_event *ev_events;
    app_t *app;
    int ev_eventsSize = 10000;
    int ev_nready, timeout, i;
    uint64_t ltime;

    ev_events = malloc(sizeof(struct epoll_event) * ev_eventsSize);
    if(ev_events == NULL){
        return -1;
    }

    for(;;){
        ltime = localtime();

        //获取最小等待时间
        node = rbtree_min(&server->timer_rbtree,server->timer_rbtree.root);
        if(node != NULL && node->key <= ltime){
            app = rbtree_data(node,app_t,timer_rbtree_node);
            app->app_errno = ETIMEDOUT;
            app->app_close_timestamp = localtime();
            app->app_status |= app_status_close;
            //log...

            //先获取下一个节点
            node = rbtree_next(&server->timer_rbtree,&app->timer_rbtree_node);
            //删除epoll事件
            epoll_ctl(server->epfd,EPOLL_CTL_DEL,app->app_fd,NULL);
            //删除时间
            rbtree_delete(&server->timer_rbtree,&app->timer_rbtree_node);
            //删除id
            rbtree_delete(&server->id_rbtree,&app->id_rbtree_node);
            //删除队列
            queue_remove(&app->task_worker_queue_node);
            //释放buf
            free(app->app_buf);
            //关闭fd
            close(app->app_fd);
            free(app);
        }

        if(node == NULL){
            timeout = -1;
        }else if(node->key == (uint64_t)-1){
            timeout = -1;
        }else{
            timeout = node->key - ltime;
        }

        ev_nready = epoll_wait(server->epfd,ev_events,ev_eventsSize,timeout);
        if(ev_nready == 0){
            //timeout
            continue;
        }

        if(ev_nready == -1){
            if(errno == EINTR){
                //被信号中断
            }
            continue;
        }

        for(i=0;i<ev_nready;i++){
            app = (app_t*)ev_events[i].data.ptr;

            if(ev_events[i].events & EPOLLIN){
                app->app_read_handler(app,server);
            }

            if(ev_events[i].events & EPOLLOUT){
                app->app_write_handler(app,server);
            }

            if(ev_events[i].events & EPOLLERR){
                app->app_errno = errno;
                app->app_close_timestamp = localtime();
                app->app_status |= app_status_close;
            }
            
            if(ev_events[i].events & EPOLLHUP){
                //对端关闭
                app->app_errno = errno;
                app->app_close_timestamp = localtime();
                app->app_status |= app_status_close;
            }

            if(ev_events[i].events & EPOLLRDHUP){
                //对端调用shutdown write
                app->app_errno = errno;
                app->app_close_timestamp = localtime();
                app->app_status |= app_status_close;
            }

            if(app->app_status&app_status_close){
                //log...

                //删除epoll事件
                epoll_ctl(server->epfd,EPOLL_CTL_DEL,app->app_fd,NULL);
                //删除时间
                rbtree_delete(&server->timer_rbtree,&app->timer_rbtree_node);
                //删除id
                rbtree_delete(&server->id_rbtree,&app->id_rbtree_node);
                //删除队列
                queue_remove(&app->task_worker_queue_node);
                //释放buf
                free(app->app_buf);
                //关闭fd
                close(app->app_fd);
                free(app);
            }
        }
    }
    return 0;
}

void server_destroy(async_server_t* server){
    if(server == NULL){
        return ;
    }

    close(server->epfd);
    close(server->dummyfd);

    if(server->id_file != NULL){
        int fd = open(server->id_file,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(fd != -1){
            write(fd,&server->id,sizeof(server->id));
            close(fd);
        }
    }
    free(server->id_file);
    free(server->log_file);
    free(server);
    return;
}

int create_bind_socket_nonblock(struct sockaddr *addr,socklen_t len){
    int fd = socket(addr->sa_family,SOCK_STREAM,0);
    if(fd == -1){
        return -1;
    }

    int flag;

    flag = 1;
    if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag)) < 0){
        close(fd);
        return -1;
    }

    if((flag = fcntl(fd,F_GETFL)) < 0){
        close(fd);
        return -1;
    }

    if(!(flag & O_NONBLOCK)){
        if(fcntl(fd,F_SETFL,flag|O_NONBLOCK) < 0){
            close(fd);
            return -1;
        }
    }

    if(bind(fd,addr,len) == -1){
        close(fd);
        return -1;
    }

    if(listen(fd,5) == -1){
        close(fd);
        return -1;
    } 
    
    return fd;
}

ssize_t add_remote_sockets_http(async_server_t* server, struct sockaddr *addr, socklen_t len, int read_timeout, int write_timeout){
    if(server == NULL){
        return -1;
    }

    int fd = create_bind_socket_nonblock(addr,len);
    if(fd == -1){
        return -1;
    }

    app_t *app = malloc(sizeof(app_t));
    if(app == NULL){
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

    app->app_data_type = data_type_http;
    app->app_status = app_status_read;
    app->app_read_handler = accept_http_cb;
    app->app_write_handler = NULL;
    app->app_read_timestamp = 0;
    app->app_write_timestamp = 0;

    app->app_buf = NULL;
    app->app_buf_len = 0;
    app->app_buf_cap = 0;

    app->app_read_timeout = read_timeout;
    app->app_write_timeout = write_timeout;

    //添加id
    app->id_rbtree_node.key = server->id++;
    rbtree_insert(&server->id_rbtree,&app->id_rbtree_node);

    rbtree_node_init(&app->timer_rbtree_node);
    queue_init(&app->task_worker_queue_node);
    return 0;
}

ssize_t add_remote_sockets_iso8583(async_server_t* server, struct sockaddr *addr, socklen_t len, int read_timeout, int write_timeout){
    if(server == NULL){
        return -1;
    }

    int fd = create_bind_socket_nonblock(addr,len);
    if(fd == -1){
        return -1;
    }

    app_t *app = malloc(sizeof(app_t));
    if(app == NULL){
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

    app->app_data_type = data_type_iso8583;
    app->app_status = app_status_read;
    app->app_read_handler = accept_iso8583_cb;
    app->app_write_handler = NULL;
    app->app_read_timestamp = 0;
    app->app_write_timestamp = 0;

    app->app_buf = NULL;
    app->app_buf_len = 0;
    app->app_buf_cap = 0;

    app->app_read_timeout = read_timeout;
    app->app_write_timeout = write_timeout;

    app->id_rbtree_node.key = server->id++;
    rbtree_insert(&server->id_rbtree,&app->id_rbtree_node);

    rbtree_node_init(&app->timer_rbtree_node);
    queue_init(&app->task_worker_queue_node);
    return 0;
}

ssize_t add_local_sockets(async_server_t* server, struct sockaddr *addr, socklen_t len, int read_timeout, int write_timeout){
    if(server == NULL){
        return -1;
    }

    int fd = create_bind_socket_nonblock(addr,len);
    if(fd == -1){
        return -1;
    }

    app_t *app = malloc(sizeof(app_t));
    if(app == NULL){
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

    app->app_data_type = data_type_local_protocol;
    app->app_status = app_status_read;
    app->app_read_handler = accept_local_protocol_cb;
    app->app_write_handler = NULL;
    app->app_read_timestamp = 0;
    app->app_write_timestamp = 0;

    app->app_buf = NULL;
    app->app_buf_len = 0;
    app->app_buf_cap = 0;

    app->app_read_timeout = read_timeout;
    app->app_write_timeout = write_timeout;

    app->id_rbtree_node.key = server->id++;
    rbtree_insert(&server->id_rbtree,&app->id_rbtree_node);

    rbtree_node_init(&app->timer_rbtree_node);
    queue_init(&app->task_worker_queue_node);
    return 0;
}