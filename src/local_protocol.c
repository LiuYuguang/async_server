#include <netinet/in.h>
#include "local_protocol.h"

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
                parser->length_n[0] = *p;
                parser->state = s_local_protocol_head_end;
                break;
            case s_local_protocol_head_end:
                parser->length_n[1] = *p;
                parser->state = s_local_protocol_body;
                parser->length = ntohs(*(uint16_t*)parser->length_n);
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