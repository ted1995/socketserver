#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <iostream>
#include <sys/socket.h>  
#include <netinet/in.h>
#include<arpa/inet.h>
#include<time.h>
#include "./timer/mytimer.h"
#include "./http/http.h"
#include "./mysqlconn/sql_connect_pool.h"
#include "./threadpool/threadpool.h"
using namespace std;


//最大事件数量
#define max_events 10000
//最大描述符
#define max_fd 65536


/*  ****定时器相关参数****  */
//定时器传输管道
static int pipefd[2];
//定时器传输链表
static sort_timer_lst timer_lst;
//内核事件表
static int epollfd = 0;
//超时单位
#define timerslot 60


//传递信号的管道
int sig_pipefd[2];


//定时器回调函数,删除非活动连接
void cb_func(client_data* c_data)
{
	//内核事件表删除事件
	epoll_ctl(epollfd, EPOLL_CTL_DEL, c_data->sockfd, 0);
	//关闭客户端socket
	cout<<"定时器到期，关闭次客户端的连接"<<c_data->sockfd<<endl;
	close(c_data->sockfd);
	//用户数减一
	http::m_user_count--;
	cout<<"当前用户数"<<http::m_user_count<<endl;
}

//定时器处理函数
void timer_handler()
{
	//tick函数具体事务：去内核事件表，去客户端sockfd，去定时器
	timer_lst.tick();
	//每次定时器处理函数执行完以后，重新定时, 设置经过timeslot时间后给进程发送SIGALRM信号
	alarm(timerslot);
}


//信号处理函数
void sig_handler(int sig)
{
	//将信号值发送到管道，让主线程接收
	int msg=sig;
	send(sig_pipefd[1],(char*)&msg,1,0);
}

//设置信号的信号处理函数
void addsig(int sig,void(handler)(int),bool restart=true)
{
	struct sigaction sa;
	memset(&sa,'\0',sizeof(sa));
	sa.sa_handler=handler;
	if(restart)
	{
		//信号设置了SA_RESTART，被信号打断的系统调用，会重新启动。
		sa.sa_flags=SA_RESTART;
	}
}

int main()
{
	
	//创建数据库连接池
	sql_connect_pool* sql_pool = sql_connect_pool::get_sql_pool();
	//初始化数据库连接
	sql_pool->init("localhost", "root", "123456", "yourdb", 3306);

	//创建线程池,传入数据库连接池实例，供线程池中的线程使用
	threadpool<http>* pool = new threadpool<http>(sql_pool);

	//预先为每一个可能的客户连接分配一个http对象
	http* http_users = new http[max_fd];

	//初始化数据库读取表
	http_users->initmysql_result(sql_pool);


	//创建监听socket
	
	int listenfd=socket(AF_INET,SOCK_STREAM,0);
	
	//SO_REUSEADDR使得listenfd即使处于time_wait状态，绑定的地址也可以立即重用
	//timew_wait状态是经历过四次挥手后的客户端socket处于的状态，此后在经历一固定时间后，socket将真正关闭，变为closed状态
	int myflag=1;
	setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&myflag,sizeof(myflag));


	struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    int port=12345;
    addr.sin_port = htons(port);

	
	//绑定地址
	int ret=0;
	ret=bind(listenfd,(struct sockaddr*)&addr,sizeof(addr));

	//开始监听，5这个参数是全连接队列的连接总数，确保同一时间不会有太多的连接。
	ret=listen(listenfd,5);
	if(ret==0)
	{
		cout<<"start listen"<<listenfd<<endl;
	}
	//创建事件数组
	epoll_event events[max_events];
	//创建内核事件表
	epollfd=epoll_create(5);
	//将listenfd的事件放入内核事件表中
	addfd(epollfd,listenfd,false);

	//http连接使用的内核事件表，m_epollfd添加的内核事件也会反映在epollfd中
	//类静态成员变量始终存在，且只有一个，所有的类对象共享此变量
	http::m_epollfd=epollfd;

	//创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    //管道写端非阻塞
    setnonblock(pipefd[1]);
    //管道读端也加入内核事件表
    addfd(epollfd, pipefd[0], false);

    //设置信号处理函数，SIGALRM是定时器信号，SIGTERM是进程终止信号
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    //当客户端处于fin——wait——2状态时，服务端如果继续调用write往此scoket发送消息会收到客户端的回应的rst报文，断开连接
    //忽略可能会产生的SIGPIPE信号，防止进程终止
	addsig(SIGPIPE, SIG_IGN);

    //服务器停止标志
    bool stop_server = false;

    //用户数据数组，与描述符对应
    client_data* c_data=new client_data[max_fd];

    //超时标志
    bool timeout=false;
    //定时,设置经过timeslot时间后给进程发送SIGALRM信号
    alarm(timerslot);
	
    while(!stop_server)
    {
		cout<<"start epoll"<<endl;
    	//从epollfd中找出活动事件，存放到events中，返回值是活动事件的个数
    	int number=epoll_wait(epollfd,events,max_events,-1);
    	//epoll_wait调用失败
    	//epoll_wait系统调用被阻塞过程中，进程接收到某种信号而执行信号处理函数时，系统调用被中断，此时调用会返回EINTR错误
    	if((number<0) && (errno!=EINTR))
    	{
    		cout<<"epoll fail"<<endl;
    		break;
    	}

		cout<<"epoll success number:"<<number<<endl;
    	//遍历处理所有事件
		for (int i = 0; i < number; i++)
		{
			//取出活动事件所属的描述符fd
			int sockfd = events[i].data.fd;
			//处理新到来的客户端连接
			if (sockfd == listenfd)
			{
				cout<<"-----------------------------"<<endl;	
				cout<<"服务器接收到客户端请求"<<endl;
				cout<<"listenfd:"<<listenfd<<endl;
				struct sockaddr_in client_aaddress;
				socklen_t client_addrlength = sizeof(client_aaddress);
				//接受连接
				int clientfd = accept(listenfd, (struct sockaddr*)&client_aaddress, &client_addrlength);
				//accept失败
				if (clientfd < 0)
				{
					cout << "accept fail" << endl;
					continue;
				}
				else
				{
					cout<<"accept success"<<endl;
					cout<<"clientfd:"<<clientfd<<endl;
				}
				//用户太多，不再接受新的连接请求
				if (http::m_user_count >= max_fd)
				{
					cout << "too many users" << endl;
					continue;
				}
              
				//接受到一个新的连接，就把这个连接的参数传到http处理任务中，由其他线程处理这个任务
				http_users[clientfd].http_init(clientfd, client_aaddress);

				//创建定时器
				util_timer* timer = new util_timer;

				//设置客户端fd的相关用户数据
				c_data[clientfd].address = client_aaddress;
				c_data[clientfd].sockfd = clientfd;
				c_data[clientfd].timer = timer;
				cout<<"clientfd:"<<clientfd<<" ->timer:"<<timer<<endl;
				//将相关数据与定时器绑定
				//用户数据
				timer->user_data = &c_data[clientfd];
				//回调函数
				timer->cb_func = cb_func;
				//当前时间
				time_t cur = time(NULL);
				//到期时间
				timer->expire = cur + 10 * timerslot;
				//将定时器加入链表
				timer_lst.add_timer(timer);
				cout<<"-----------------------------"<<endl;
			}
			//客户端正常关闭连接close，触发EPOLLIN和EPOLLRDHUP
			//可以使用EPOLLRDHUP来检测客户端关闭事件，也可以使用EPOLLIN，read返回0来检测客户端关闭事件
			else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
			{	
				cout<<"-----------------------------"<<endl;
				cout<<"客户端关闭链接:"<<sockfd<<endl;
				util_timer* timer = c_data[sockfd].timer;
				//关闭服务端的clientsocket，从内核事件表中一处，完成四次挥手，正常断开连接
				timer->cb_func(&c_data[sockfd]);
				//删除对应的定时器
				if (timer)
				{
					timer_lst.del_timer(timer);
				}
			}
			//处理信号事件，管道读事件
			else if ((sockfd == pipefd[0]) && (events[i].events&EPOLLIN))
			{
				cout<<"-----------------------------"<<endl;
				cout<<"处理信号事件"<<endl;
				int sig;
				char signals[1024];
				//接收信号处理函数通过管道传递过来的信号
				ret = recv(pipefd[0], signals, sizeof(signals), 0);
				if (ret == -1)
				{	//接受失败
					continue;
				}
				else if (ret == 0)
				{	//没接收到数据
					continue;
				}
				else
				{
					for (int i = 0; i < ret; i++)
					{
						//遍历传递过来的信号
						switch (signals[i])
						{
							//定时器信号
						case SIGALRM:
						{
							//定时器到期标志
							timeout = true;
							break;
						}
						//进程终止信号
						case SIGTERM:
						{
							stop_server = true;
							break;
						}
						}
					}
				}
			}
			//处理客户连接上收到的数据
			else if (events[i].events&EPOLLIN)
			{
				cout<<"-----------------------------"<<endl;
				cout<<"接受客户端收到的数据:"<<sockfd<<endl;
				util_timer* timer = c_data[sockfd].timer;
				//ET模式，一次性将数据读完，此http实例对象已成为一个具体的任务
				if (http_users[sockfd].read_once())
				{	
					cout<<"已读取完客户端的数据"<<endl;
					//将此请求任务放到队列中，从线程池中调用空闲线程处理请求任务
					cout<<"此次生成的http请求："<<http_users+sockfd<<endl;
					pool->append(http_users + sockfd);

					//若有数据传输，将定时器往后延迟10个单位，并调整定时器的位置
					if (timer)
					{
						time_t cur = time(NULL);
						timer->expire = cur + 10 * timerslot;
						timer_lst.adjust_timer(timer);
					}
				}
				//读取数据出错，比如客户端关闭连接，调用定时器回调函数，结束这个处理
				else
				{
					timer->cb_func(&c_data[sockfd]);
					if (timer)
					{
						timer_lst.del_timer(timer);
					}
				}
				cout<<"-----------------------------"<<endl;
			}
			//向发送缓冲区中写入数据
			else if (events[i].events&EPOLLOUT)
			{
				cout<<"-----------------------------"<<endl;
				cout<<"向客户端发送数据"<<endl;
				util_timer* timer = c_data[sockfd].timer;
				//数据已全部写入发送缓冲区或者部分写入但是缓冲区已满
				if (http_users[sockfd].write())
				{
					//延长定时器，等待下次请求或者继续写入数据
					if (timer)
					{
						cout<<"timer:"<<timer<<endl;
						time_t cur = time(NULL);
						timer->expire = cur + 10 * timerslot;
						timer_lst.adjust_timer(timer);
					}
				}
				//客户端断开连接或者发送数据出错
				else
				{
					timer->cb_func(&c_data[sockfd]);
					if (timer)
					{
						timer_lst.del_timer(timer);
					}
				}
				cout<<"-----------------------------"<<endl;
			}
		}
		//遍历完所有事件，如果有定时器到期，执行定时器处理函数
		if (timeout)
		{
			timer_handler();
			timeout = false;
		}
    }
    return 0;
}
