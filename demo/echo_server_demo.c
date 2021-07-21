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

#define BUFFER_LENGTH 8192

int main(){
    const char socket_file[] = "/tmp/socksdtcp.sock";
    int sockfd;
    struct sockaddr_un local_addr;
    local_addr.sun_family = AF_UNIX;
    strcpy(local_addr.sun_path,socket_file);

    sockfd = socket(AF_UNIX,SOCK_STREAM,0);
    assert(sockfd != -1);

    if(connect(sockfd,(struct sockaddr*)&local_addr,sizeof(local_addr)) == -1){
        printf("%d %s\n",errno,strerror(errno));
        return 1;
    }

    local_protocol_parser parser;

    unsigned char recvData[BUFFER_LENGTH],sendData[BUFFER_LENGTH];
    ssize_t len,len_parsed;
    int sendLen=0,recvLen=0;
    local_protocol_data_t *local_protocol_data;
    int local_protocol_data_size;
    local_protocol_parser_init(&parser);

    for(;;){
        //printf("---------------------------\n");
        //printf("recv start len%d cap%d\n",recvLen,recvCap);
        len =  recv(sockfd,recvData + recvLen,BUFFER_LENGTH-recvLen,0);
        if(len == 0){
            printf("close by peer\n");
            break;
        }else if(len == -1){
            printf("errno%d, strerror%s\n",errno,strerror(errno));
            break;
        }

        len_parsed = 0;
        while(len_parsed < len){
            len_parsed =  local_protocol_parser_execute(&parser,recvData+recvLen,len-recvLen);
            recvLen += len_parsed;
            if(local_protocol_parser_is_done(&parser)){
                //完整一个包
                local_protocol_data = (local_protocol_data_t *)(recvData+sizeof(uint16_t));
                local_protocol_data_size = recvLen - sizeof(uint16_t) - sizeof(local_protocol_data_t);
                printf("id%lu, clock%lu, type%u, ipc_data_size%d\n",\
                    local_protocol_data->id,\
                    local_protocol_data->clock,\
                    local_protocol_data->data_type,\
                    local_protocol_data_size);

                if(local_protocol_data->data_type == data_type_iso8583){
                    memcpy(sendData,recvData,recvLen);
                    sendLen = recvLen;
                    send(sockfd,sendData,sendLen,0); 
                }else if(local_protocol_data->data_type == data_type_http){
                    memcpy(sendData,recvData,recvLen);
                    local_protocol_data = (local_protocol_data_t *)(sendData + sizeof(uint16_t));
                    local_protocol_data_size =  sprintf((char*)local_protocol_data->data,"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
                    *(uint16_t*)sendData = htons(local_protocol_data_size + sizeof(local_protocol_data_t));
                    sendLen = local_protocol_data_size + sizeof(local_protocol_data_t) + sizeof(uint16_t);
                    send(sockfd,sendData,sendLen,0); 
                }
                memmove(recvData,recvData + recvLen,len - len_parsed);
                len -= len_parsed;
                recvLen = 0;
                len_parsed = 0;
            }
        }
    }

    close(sockfd);
    return 0;
}