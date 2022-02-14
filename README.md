# ASYNC_SERVER  
一个服务端程序, 将多个client的连接, 转为一个连接, 交由后端处理.  
支持设置超时时间, 后端处理超时, 自动关闭client连接.  
支持多种协议(HTTP, ISO8583), 支持监听多个端口  

## 2022/02/10更新
1. 优化部分流程
2. 完善HTTP错误信息返回
3. 添加log cache和写log的操作

---
# 项目文件目录
```bash
.
├── demo
│   ├── async_server_demo.c #async_server实例的demo
│   ├── echo_server_demo.c  #后端程序的实例demo
│   └── Makefile
├── include
│   ├── async_server.h
│   ├── http_parser.h
│   ├── iso8583_parser.h
│   ├── queue.h             #nginx的队列, 有所修改
│   └── rbtree.h
├── Makefile
├── obj
│   ├── async_server_demo.o
│   └── echo_server_demo.o
├── README.md
└── src
    ├── async_server.c      #async_server的代码
    ├── http_parser.c       #http解析
    ├── iso8583_parser.c    #ISO8583解析
    └── rbtree.c            #nginx的红黑树, 有所修改
```

---
# 依赖包
- nginx的rbtree和queue, 有所更改, <https://github.com/nginx/nginx>  
- http_parser, 有所更改, <https://github.com/nodejs/http-parser>