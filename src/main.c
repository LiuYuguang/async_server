#include <stdio.h>
#include <netinet/in.h>
#include <sys/un.h>     //for struct sockaddr_un
#include "async_server.h"


int main(){
    async_server_t* server = server_create();

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
	sin.sin_port = htons(8888);
    inet_pton(AF_INET,"0.0.0.0",&sin.sin_addr);
    add_remote_sockets_http(server,(struct sockaddr*)&sin,sizeof(sin),5000,5000);

    const char socket_file[] = "/tmp/socksdtcp.sock";
    struct sockaddr_un local_addr;
    unlink(socket_file);//https://gavv.github.io/articles/unix-socket-reuse/
    local_addr.sun_family = AF_UNIX;
    strcpy(local_addr.sun_path,socket_file);
    add_local_sockets(server,(struct sockaddr*)&local_addr,sizeof(local_addr),-1,-1);

    server_start_loop(server);
    server_destroy(server);
}