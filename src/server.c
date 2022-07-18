#include <stdio.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>     //for struct sockaddr_un
#include "async_server.h"
#include <setjmp.h>     //for jmp_buf, setjmp, longjmp
#include <stdlib.h>      //for exit
#include <signal.h>      //for signal
#include <unistd.h>

jmp_buf jmpbuf;
void signal_handler(int signo){
    longjmp(jmpbuf,1);
}

void signal_pipe_handle(int signo){

}

int main(){
    signal(SIGINT,signal_handler);
    signal(SIGTERM,signal_handler);
    signal(SIGPIPE,signal_pipe_handle);

    async_server_t* server = server_create("./id_file","/dev/null",4096*4096);

    short port;
    for(port=8890;port<8900;port++){
        struct sockaddr_in sin;
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        inet_pton(AF_INET,"0.0.0.0",&sin.sin_addr);
        add_sockets(server,(struct sockaddr*)&sin,sizeof(sin),5000,DATA_TYPE_HTTP);
    }

    const char socket_file[] = "/tmp/socksdtcp.sock";
    struct sockaddr_un local_addr;
    unlink(socket_file);//https://gavv.github.io/articles/unix-socket-reuse/
    local_addr.sun_family = AF_UNIX;
    strcpy(local_addr.sun_path,socket_file);
    add_sockets(server,(struct sockaddr*)&local_addr,sizeof(local_addr),-1,DATA_TYPE_LOCAL);

    if(setjmp(jmpbuf)){
        server_destroy(server);
        exit(0);    
    }
    server_start_loop(server,10000);
    return 0;
}
