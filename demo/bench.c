#include <stdio.h>      //for printf
#include <stdlib.h>     //for malloc free
#include <unistd.h>     //for close
#include <sys/socket.h> //for socket
#include <netinet/in.h> //for struct sockaddr htons
#include <arpa/inet.h>  //for inet_pton
#include <errno.h>      //for errno
#include <string.h>     //for strerror
#include <sys/epoll.h>  //for epoll*
#include <pthread.h>    //for pthread*


#define MAX_CONNECT 30000
#define HOST "127.0.0.1"
#define PORT 6678

int epfd;
pthread_t tid;
pthread_mutex_t mutex;
pthread_cond_t cond;
int count,fd[2],flag = 0;

void *handler(void* arg){
    int nready,i;
    struct epoll_event *events = malloc(sizeof(struct epoll_event) * 10000);
    for(;flag == 0;){
        nready = epoll_wait(epfd,events,10000,-1);
        if(nready == 0){
            //timeout
            continue;
        }
        if(nready == -1){
            printf("child thread %lu, %d, %s\n",pthread_self(),errno,strerror(errno));
            if(errno == EINTR){
                continue;
            }
            break;
        }
        for(i=0;i<nready;i++){
            close(events[i].data.fd);
            pthread_mutex_lock(&mutex);
            count--;
            pthread_mutex_unlock(&mutex);
            pthread_cond_signal(&cond);
        }
    }
    free(events);
    pthread_exit(NULL);
}

int main(){
    //init
    epfd = epoll_create(1);
    pthread_mutex_init(&mutex,NULL);
    pthread_cond_init(&cond,NULL);
    count = 0;
    pipe(fd);

    //子线程
    pthread_create(&tid,NULL,handler,NULL);
    
    //主线程
    int sockfd;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET,HOST,&addr.sin_addr);

    struct epoll_event ev;

    ev.data.fd = fd[0];
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    epoll_ctl(epfd,EPOLL_CTL_ADD,fd[0],&ev);

    for(;;){
        pthread_mutex_lock(&mutex);
        while(count >= MAX_CONNECT)
            pthread_cond_wait(&cond,&mutex);

        sockfd = socket(AF_INET,SOCK_STREAM,0);
        if(connect(sockfd,(struct sockaddr*)&addr,sizeof(addr)) == -1){
            printf("main thread %lu, %d, %s\n",pthread_self(),errno,strerror(errno));
            pthread_mutex_unlock(&mutex);
            break;
        }

        ev.data.fd = sockfd;
        ev.events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
        epoll_ctl(epfd,EPOLL_CTL_ADD,sockfd,&ev);

        count++;
        pthread_mutex_unlock(&mutex);
    }

    //退出
    flag = 1;
    write(fd[1],"1",1);
    pthread_join(tid,NULL);

    close(epfd);
    pthread_mutex_lock(&mutex);
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);
    return 0;
}