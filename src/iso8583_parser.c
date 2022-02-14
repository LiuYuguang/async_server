#include <stdio.h>
#include <netinet/in.h>
#include "iso8583_parser.h"


void iso8583_parser_init(iso8583_parser *parser){
    parser->state = s_iso8583_head_start;
}

size_t iso8583_parser_execute(iso8583_parser *parser,const void *data,size_t len){
    const uint8_t *p;
    //网络字节序默认大端
    for(p=data;p<(const uint8_t *)data+len;p++){
        switch (parser->state)
        {
            case s_iso8583_head_start:
                parser->length = *p;
                parser->state = s_iso8583_head_end;
                break;
            case s_iso8583_head_end:
                parser->length = (parser->length<<8) | *p;
                parser->state = s_iso8583_body;
                break;
            case s_iso8583_body:
                if(parser->length <= (const uint8_t *)data+len-p){
                    p += parser->length;
                    parser->length = 0;
                    parser->state = s_iso8583_head_start;
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

size_t iso8583_parser_is_done(iso8583_parser *parser){
    if(parser->state == s_iso8583_head_start){
        return 1;
    }
    return 0;
}