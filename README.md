# socketserver
1.socket编程实现的多线程http服务器，使用epoll复用机制。
2.主线程监听serverfd，根据epoll的结果分别进行以下处理：
    1）接收客户端的连接请求，再次加入到epoll的监听队列。
    2）处理客户端fd的读事件，在读事件就绪时，调用http类的读方法（ET），将数据读入到http类对象的读缓冲区中。
    3）处理客户端fd的写事件，写事件就绪时，调用http类的写方法，将数据发送给客户端。
3.http类对象作为资源放入同步队列，由线程池中的线程获取http类对象，并调用http类内的相应方法处理接收到的数据，并按照要求制作回应数据，之后注册写事件EPOLLOUT，写事件只要发送缓冲区不满就会响应，因此会立刻触发主线程clientfd的写事件。
4.可以处理get与post请求，使用简单的数据库知识，可以实现用户的注册与登录。
