#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h> //for socket
#include <netinet/in.h> //for sockaddr_in htons htonl
#include <arpa/inet.h>  //for htons htonl
#include <sys/un.h>     //for struct sockaddr_un

#include <errno.h>
#include <string.h>

#include <assert.h>

#include "async_server.h"

#define LOCAL_MAX_WMEM (128*1024)  // 128K, local socket写缓存大小
#define LOCAL_MAX_RMEM (128*1024)  // 128K, local socket读缓存大小

int main(){
    const char socket_file[] = "/tmp/socksdtcp.sock";
    int sockfd;
    struct sockaddr_un local_addr;
    local_addr.sun_family = AF_UNIX;
    strcpy(local_addr.sun_path,socket_file);

    sockfd = socket(AF_UNIX,SOCK_STREAM,0);
    assert(sockfd != -1);

    assert(connect(sockfd,(struct sockaddr*)&local_addr,sizeof(local_addr)) != -1);

    int send_buf = LOCAL_MAX_WMEM, recv_buf = LOCAL_MAX_RMEM;
    socklen_t send_buf_size = sizeof(send_buf), recv_buf_size = sizeof(recv_buf);
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const char*)&send_buf, sizeof(send_buf));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (const char*)&recv_buf, sizeof(recv_buf));

    getsockopt(sockfd,SOL_SOCKET,SO_SNDBUF,&send_buf,&send_buf_size);
    getsockopt(sockfd,SOL_SOCKET,SO_RCVBUF,&recv_buf,&recv_buf_size);

    local_protocol_parser parser;

    unsigned char recvData[LOCAL_MAX_RMEM],sendData[LOCAL_MAX_WMEM],*p;
    ssize_t len,len_parsed;
    int sendLen=0,recvLen=0,offset;
    local_protocol_data_t *proto_data;
    int proto_data_size;
    local_protocol_parser_init(&parser);

    for(;;){
        len = recv(sockfd,recvData + recvLen,LOCAL_MAX_RMEM-recvLen,0);
        if(len == 0){
            printf("close by peer\n");
            break;
        }else if(len == -1){
            printf("errno%d, strerror%s\n",errno,strerror(errno));
            break;
        }

        p = recvData;
        offset = recvLen;
        len_parsed = 0;
        while(len_parsed < len){
            len_parsed = local_protocol_parser_execute(&parser,p+offset,len);
            offset += len_parsed;
            if(local_protocol_parser_is_done(&parser)){
                //完整一个包
                proto_data = (local_protocol_data_t *)(p+sizeof(uint16_t));
                proto_data_size = offset - sizeof(uint16_t) - sizeof(local_protocol_data_t);
                //printf("id%lu, clock%lu, type%u, ipc_data_size%d\n",\
                    local_protocol_data->id,\
                    local_protocol_data->clock,\
                    local_protocol_data->data_type,\
                    local_protocol_data_size);

                if(proto_data->data_type == DATA_TYPE_ISO8583){
                    memcpy(sendData,p,offset);
                    sendLen = offset;
                    send(sockfd,sendData,sendLen,0); 
                }else if(proto_data->data_type == DATA_TYPE_HTTP){
                    memcpy(sendData,p,offset);
                    proto_data = (local_protocol_data_t *)(sendData + sizeof(uint16_t));
                    proto_data_size =  sprintf((char*)proto_data->data,"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
                    *(uint16_t*)sendData = htons(proto_data_size + sizeof(local_protocol_data_t));
                    sendLen = proto_data_size + sizeof(local_protocol_data_t) + sizeof(uint16_t);
                    send(sockfd,sendData,sendLen,0); 
                }
                p += offset;
                offset = 0;
                len -= len_parsed;
                len_parsed = 0;
            }
        }

        memmove(recvData,p,offset);
        recvLen = offset;
    }

    close(sockfd);
    return 0;
}
