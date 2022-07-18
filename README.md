# ASYNC_SERVER  
一个服务端程序, 将多个client的连接, 转为一个连接, 交由后端处理.  
支持设置超时时间, 后端处理超时, 自动关闭client连接.  
支持多种协议(HTTP, ~~ISO8583~~), 支持监听多个端口  

## 2022/02/10更新
1. 优化部分流程
~~2. 完善HTTP错误信息返回~~
~~3. 添加log cache和写log的操作~~

## 2022/07/18更新
4. 简化代码  

---
# 项目文件目录
```bash
.
├── include
│   ├── async_server.h
│   ├── http_parser.h       # http解析
│   ├── queue.h             # nginx的队列, 有所修改
│   └── rbtree.h            # nginx的红黑树, 有所修改
├── Makefile
├── README.md
└── src
    ├── async_server.c      # async_server的代码
    ├── echo.c              # 反射程序, 配合server使用, 固定响应
    ├── http_parser.c       # http解析
    ├── rbtree.c            # nginx的红黑树, 有所修改
    └── server.c            # server程序
```

---
# 依赖包
- nginx的rbtree和queue, 有所更改, <https://github.com/nginx/nginx>  
- http_parser, 有所更改, <https://github.com/nodejs/http-parser>  



-------------------------------------------------------------------
## 2022/05/11  
最近在研究协程，是时候重构啦。除此之外，还想解决当前版本的一些不足  
1. 数据读取的buf是存在内存的，通过realloc扩容，但是不可能做到很大。是否可以先缓存到硬盘？  
2. 使用协程的话，很多接口都需要重写，包括队列的操作  
3. 在key查询的时候，能否用hash map代替rbtree  