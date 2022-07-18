#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h> //for socket
#include <netinet/in.h> //for sockaddr_in htons htonl
#include <arpa/inet.h>  //for htons htonl
#include <sys/un.h>     //for struct sockaddr_un
#include <stdlib.h>

#include <errno.h>
#include <string.h>

#include <assert.h>

#include "async_server.h"

#define LOCAL_MAX_WMEM (1024*1024)  // 128K, local socket写缓存大小
#define LOCAL_MAX_RMEM (1024)       // 128K, local socket读缓存大小

#define HTTP_RESPONSE "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"

int main(){
    struct sockaddr_un local_addr = {.sun_family = AF_UNIX,.sun_path = "/tmp/socksdtcp.sock"};

    int sockfd = socket(AF_UNIX,SOCK_STREAM,0);
    assert(sockfd != -1);

    assert(connect(sockfd,(struct sockaddr*)&local_addr,sizeof(local_addr)) != -1);

    local_protocol_parser parser;
    unsigned char *recvData=NULL,*sendData=NULL,*p;
    ssize_t len,len_parsed;
    int sendLen=0,recvLen=0,offset;
    local_protocol_data_t *proto_data,*proto_data_tmp;
    int proto_data_size;
    local_protocol_parser_init(&parser);

    recvData = malloc(LOCAL_MAX_WMEM);
    assert(recvData);

    sendData = malloc(LOCAL_MAX_RMEM);
    assert(sendData);

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
                // printf("id%lu, clock%lu, type%u, ipc_data_size%d\n",
                //     local_protocol_data->id,
                //     local_protocol_data->clock,
                //     local_protocol_data->data_type,
                //     local_protocol_data_size);

                if(proto_data->data_type == DATA_TYPE_HTTP){
                    proto_data_tmp = proto_data;
                    proto_data = (local_protocol_data_t *)(sendData + sizeof(uint16_t));
                    proto_data->id = proto_data_tmp->id;
                    proto_data->clock = proto_data_tmp->clock;
                    proto_data->data_type = proto_data_tmp->data_type;
                    memmove(proto_data->data,HTTP_RESPONSE,sizeof(HTTP_RESPONSE)-1);
                    proto_data_size = sizeof(HTTP_RESPONSE)-1;
                    *(uint16_t*)sendData = htons(sizeof(local_protocol_data_t) + proto_data_size);
                    sendLen = sizeof(uint16_t) + sizeof(local_protocol_data_t) + proto_data_size;
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
    free(recvData);
    free(sendData);
    close(sockfd);
    return 0;
}
