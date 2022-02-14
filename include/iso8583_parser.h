#ifndef _ISO8583_PARSER_H_
#define _ISO8583_PARSER_H_

#include <stdint.h>
#include <stddef.h>

enum iso8583_state{
    s_iso8583_head_start = 0,
    s_iso8583_head_end = 1,
    s_iso8583_body = 2,
};

typedef struct iso8583_parser_s iso8583_parser;
struct iso8583_parser_s{
    enum iso8583_state state;
    uint16_t length;
};

void iso8583_parser_init(iso8583_parser *parser);
size_t iso8583_parser_execute(iso8583_parser *parser,const void *data,size_t len);
size_t iso8583_parser_is_done(iso8583_parser *parser);

#endif