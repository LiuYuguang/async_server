#include <stdio.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/un.h>     //for struct sockaddr_un
#include "async_server.h"
#include <setjmp.h>     //for jmp_buf, setjmp, longjmp
#include <stdlib.h>      //for exit
#include <signal.h>      //for signal

ssize_t write_log(const char* file_name,const void *buf, size_t n){
    int fd = open(file_name,O_WRONLY|O_CREAT|O_APPEND,0664);
    if(fd == -1){
        return -1;
    }
    ssize_t ret = write(fd,buf,n);
    close(fd);
    return ret;
}

jmp_buf jmpbuf;
void signal_handler(int signo){
    longjmp(jmpbuf,1);
}

int main(){
    signal(SIGINT,signal_handler);
    signal(SIGTERM,signal_handler);

    async_server_t* server = server_create();

    server_set_id_file(server,"./id_file");
    //server_set_log_file(server,"./log",write_log);

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
	sin.sin_port = htons(8888);
    inet_pton(AF_INET,"0.0.0.0",&sin.sin_addr);
    add_remote_sockets_http(server,(struct sockaddr*)&sin,sizeof(sin),5000,5000);

    //struct sockaddr_in sin;
    sin.sin_family = AF_INET;
	sin.sin_port = htons(8889);
    inet_pton(AF_INET,"0.0.0.0",&sin.sin_addr);
    add_remote_sockets_iso8583(server,(struct sockaddr*)&sin,sizeof(sin),5000,5000);

    const char socket_file[] = "/tmp/socksdtcp.sock";
    struct sockaddr_un local_addr;
    unlink(socket_file);//https://gavv.github.io/articles/unix-socket-reuse/
    local_addr.sun_family = AF_UNIX;
    strcpy(local_addr.sun_path,socket_file);
    add_local_sockets(server,(struct sockaddr*)&local_addr,sizeof(local_addr),-1,-1);

    if(setjmp(jmpbuf)){
        server_destroy(server);
        exit(0);    
    }
    server_start_loop(server,10000);
}
