# ASYNC_SERVER  
一个服务端程序, 将多个client的连接, 转为一个连接, 交由后端处理.  
支持设置超时时间, 后端处理超时, 自动关闭client连接.
支持多种协议, 多个监听端口  

---
# 依赖包
- nginx的rbtree和queue, 有所更改, <https://github.com/nginx/nginx>  
- http_parser, 有所更改, <https://github.com/nodejs/http-parser>