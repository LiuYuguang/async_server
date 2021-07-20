#ifndef _LOCAL_PROTOCOL_H_
#define _LOCAL_PROTOCOL_H_

#include <stdint.h>
#include <stddef.h>

enum local_protocol_state{
    s_local_protocol_head_start = 0,
    s_local_protocol_head_end = 1,
    s_local_protocol_body = 2,
};

typedef struct local_protocol_parser_s local_protocol_parser;
struct local_protocol_parser_s{
    enum local_protocol_state state;
    unsigned char length_n[2];
    uint16_t length;
};

void local_protocol_parser_init(local_protocol_parser *parser);
size_t local_protocol_parser_execute(local_protocol_parser *parser,const void *data,size_t len);
size_t local_protocol_parser_is_done(local_protocol_parser *parser);

enum data_type_t{
    data_type_http,
    data_type_iso8583,
    data_type_local_protocol,
};

typedef struct local_protocol_data_s local_protocol_data_t;

struct local_protocol_data_s{
    uint64_t id;
    uint64_t clock;
    enum data_type_t data_type;
    uint8_t data[0];
};


#endif