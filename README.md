# ASYNC_SERVER  
一个服务端程序, 将多个client的连接, 转为一个连接, 交由后端处理.  
支持设置超时时间, 后端处理超时, 自动关闭client连接.  

---
# 依赖包
- nginx的rbtree和queue, <https://github.com/nginx/nginx>  
- http_parser, <https://github.com/nodejs/http-parser>