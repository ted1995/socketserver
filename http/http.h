//http连接
#pragma once
#include<iostream>
#include<sys/epoll.h>
#include<fcntl.h>
#include<unistd.h>
#include<string.h>
#include<string>
#include<errno.h>
#include<map>
#include<stdio.h>
#include<sys/uio.h>
#include<sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include<sys/types.h>
#include"../mysqlconn/sql_connect_pool.h"



//写缓冲区大小
#define WRITE_BUFFER_SIZE 1024
//读缓冲区大小
#define READ_BUFFER_SIZE 2048
#define filename_len 200


//用户名和密码放入map
map<string, string> users;
//互斥锁
locker h_lock;


//定义http响应状态信息
const char* ok_200_title = "ok";
const char* error_400_title = "bad_retuest";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//主状态机三种状态
enum parent_state
{	
	state_requestline=0,  	//分析请求行
	state_header,			//分析请求头部
	state_content			//分析请求内容
};

//从状态机三种状态
enum child_state
{
	line_full=0,    //读取完整的行
	line_bad, 		//行出错
	line_unfull		//读取行不完整
};


//服务器处理http请求的结果
enum http_code
{
	no_request=0,		//请求不完整，继续读取客户数据
	get_request,		//完整的客户请求
	bad_request,		//错误的客户端请求
	no_resource,		//没有资源
	file_request,		//文件请求
	forbid_request,		//客户端没有访问权限
	internel_error,		//服务器内部错误
	closed_connect 		//客户端关闭连接
};

enum method
{
	GET=0,
	POST,
	HEAD,
	PUT,
	DELETE,
	TRACE,
	OPTION,
	CONNECT,
	PATH
};


//设置描述符非阻塞
int setnonblock(int fd)
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}


//将文件描述符中的事件注册到内核时间表中
void addfd(int epollfd,int fd,bool oneshot)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	if (oneshot == true)
	{
		event.events |= EPOLLONESHOT;
	}

	//将fd的写事件加入内核事件表
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
	setnonblock(fd);
}



//将事件重置为epolloneshot
//注册epolloneshot的描述符，操作系统最多触发其上注册的一个可读，可写或异常事件。但是只能使用一次，每次处理完这个描述符，都要重新设置epolloneshot。
void reset_oneshot(int epollfd, int fd, int ev)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
	//EPOLL_CTL_MOD修改fd上的注册事件
	epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//从内核事件表中删除监听的客户端事件
void removefd(int epollfd, int fd)
{
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
	close(fd);
}


class http
{
private:

	//客户端socket
	int m_sockfd;
	//客户端地址
	sockaddr_in m_address;

	//buffer中尾字节的下一字节
	int read_index;
	//读缓冲区
	char read_buffer[READ_BUFFER_SIZE];
	//buffer中正在解析的字节
	int check_index;

	//
	int write_index;
	//写缓冲区
	char write_buf[WRITE_BUFFER_SIZE];

	//域名
	char* url;
	//主状态机状态
	parent_state p_state;
	//
	method h_method;
	//
	char* m_version;
	//
	char* host;
	//
	int content_length;
	//
	char* login_data;
	//
	int start_line;
	//
	char real_file[filename_len];
	//服务器根目录路径
	const char* root_path = "/home/wang/webserver/html";
	//文件属性结构体stat
	struct stat file_stat;
	//
	char* file_address;
	//
	bool linger;
	//向量元素
	struct iovec iv[2];
	//
	int iv_count;
	//
	int send_byte_num;
	//
	int have_send_num;
	//
	int cgi;

public:
	static int m_epollfd;
	static int m_user_count;
	MYSQL* mysql;


	//关闭一个连接，客户端减一
	void close_connect(bool real_close)
	{
		if (real_close && (m_sockfd != -1))
		{
			removefd(m_epollfd, m_sockfd);
			m_sockfd = -1;
			m_user_count--;
		}
	}

	//初始化数据库读取表
	//传入数据库连接池对象
	void initmysql_result(sql_connect_pool *s_c_pool)
	{
		//创建MYSQL对象
		MYSQL* sql = NULL;
		//从数据库连接池中取出一个连接，赋值给sql		
		sql=s_c_pool->get_connection();		

		//在user数据表中检索用户密码是否正确
		if (mysql_query(sql, "SELECT username,passwd FROM user"))
		{
			cout << "fail" << endl;
		}

		//从表中检索完整的结果集
		MYSQL_RES *result = mysql_store_result(sql);

		//从结果集中将对应的用户名和密码存入map中,方便服务器核对校验
		while (MYSQL_ROW row = mysql_fetch_row(result))
		{
			string temp1(row[0]);
			string temp2(row[1]);
			users[temp1] = temp2;
		}

		//释放一个MYSQL连接		
		s_c_pool->release_connection(sql);

	}


	//参数初始化
	void para_init()
	{
		this->mysql = 0;
		this->send_byte_num = 0;
		this->have_send_num = 0;
		this->p_state = state_requestline;
		this->linger = false;
		this->h_method =GET;
		this->url = 0;
		this->m_version = 0;
		this->content_length = 0;
		this->host = 0;
		this->start_line = 0;
		this->check_index = 0;
		this->read_index = 0;
		this->write_index = 0;
		this->cgi = 0;
		memset(read_buffer, '\0', READ_BUFFER_SIZE);
		memset(write_buf, '\0', WRITE_BUFFER_SIZE);
		memset(real_file, '\0', filename_len);
	}

	//在接收到新的连接之后，http连接初始化结构体，将客户端事件加入内核事件表
	void http_init(int sockfd, const sockaddr_in &addr)
	{
		m_sockfd = sockfd;
		m_address = addr;
		addfd(m_epollfd, m_sockfd, true);
		cout<<"加入内核事件表"<<endl;
		//客户数量加1
		m_user_count++;
		//初始化此http连接
		para_init();
		cout<<"初始化此次http链接"<<endl;
	}


	
	bool print_buffer()
	{
		cout<<read_buffer<<endl;
	}

	//ET模式下，一次性读取所有数据
	bool read_once()
	{
		if (read_index >= READ_BUFFER_SIZE)
		{
			return false;
		}
		int byte_read = 0;
		//非阻塞模式下，没有读取到数据recv会直接返回，不会等待，因此为了将数据全部读取出来，使用了while循环来实现
		while (true)
		{
			//从read_buffer的read_index处开始读取数据到此缓冲区中
			byte_read = recv(m_sockfd, read_buffer + read_index, READ_BUFFER_SIZE - read_index, 0);
			if (byte_read == -1)
			{
				//下面的条件表示socket中的数据已读取完毕
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;//读完
			}
			else if (byte_read == 0)
			{
				//客户段关闭连接，有接受事件但接受到的数据是0
				return false;
			}
			//没读完继续循环读取
			read_index += byte_read;
		}
		//cout<<"读取到的数据:"<<read_buffer<<endl;
		return true;
	}

	//从状态机,取出完整的一行数据,返回值为行的读取状态，line_full。。。
	child_state get_full_line()
	{
		cout<<"请求获得完整行"<<endl;
		char temp;
		//check_index指向buffer中正在解析的字节，read_index指向buffer中尾字节的下一字节。
		for (; check_index < read_index; check_index++)
		{
			//获得当前要分析的字节,read_buffer是读缓存区
			temp = read_buffer[check_index];
			//若是‘\r’，即回车符，则说明可能读取到一个完整的行
			if (temp == '\r')
			{
				//如果\r碰巧是目前buffer中的最后一个已被读入的客户数据，那么这次分析没有读取到完整的行，返回line_unfull继续读取客户数据
				if ((check_index + 1) == read_index)
				{
					return line_unfull;
				}
				//如果下一个字符是\n，则成功读取到一个完整的行。
				else if (read_buffer[check_index + 1] == '\n')
				{
					//read_buffer[check_index++]='\0' 变为 read_buffer[check_index]='\0' , check_index++
					//将当前\r变成\0，移动到下一个字符，再将\n变成\0，移动到下一个待解析字符
					
					read_buffer[check_index++] = '\0';
					read_buffer[check_index++] = '\0';
					return line_full;
				}
				//存在语法问题
				else
				{
					return line_bad;
				}
			}
			//如果当前读到的字节是\n，即换行符，则也可能读到一个完整的行
			else if (temp == '\n')
			{
				if ((check_index > 1) && (read_buffer[check_index - 1] == '\r'))
				{
					//同上，将\r\n变为\0\0，再将check_index移动到下一个字符
					read_buffer[check_index - 1] = '\0';
					read_buffer[check_index++] = '\0';
					return line_full;
				}
				else
				{
					return line_bad;
				}
			}
		}
		//buffer解析完毕，仍然没有遇到\r字符，说明没有读取完，需要继续读取数据。
		return line_unfull;
	}


	//分析http请求行,获得请求方法，目标url，http版本号，返回http请求的处理结果
	http_code parse_requestline(char* temp)
	{
		/*
			http请求报文的格式：
				请求行》
					请求方法+空格+url+空格+协议版本+回车符+换行符
				请求头部》
					头部字段名：值+回车符+换行符
					。。。
				空白行》
					回车符+换行符
				请求正文》
					。。。
		*/
		//该函数返回 str1 中第一个匹配字符串 str2 中任一字符的位置，如果未找到字符则返回 NULL。
		//url跳到请求方法后的空格上
		cout<<"temp:"<<temp<<endl;
		url = strpbrk(temp, " ");
		//如果请求行中没有'\t'字符(空格)，则http请求必有问题
		if (!url)
		{
			cout<<"parse_requestline:bad_request"<<endl;
			return bad_request;
		}
		//等价于*url='\0',url++
		//当前位空格置为null字符，再指向下一个位置。
		*url++ = '\0';

		//取出具体的方法
		char* m_method = temp;
		//strcasecmp()用来比较参数s1 和s2 字符串是否相等，相同就返回0
		if (strcasecmp(m_method, "GET") == 0)
		{
			h_method = GET;
			cout<<"parse_requestline: GET"<<endl;
		}
		else if (strcasecmp(m_method, "POST") == 0)
		{
			h_method = POST;
			cgi = 1;
		}
		else
		{
			return bad_request;
		}


		//略过可能还存在的空白字符。
		url += strspn(url, " ");
		cout<<"url:"<<url<<endl;
		//跳到url后的空格上
		m_version = strpbrk(url, " ");
		if (!m_version)
		{
			return bad_request;
		}
		//将当前空格置为null字符，再指向下一个位置。
		*m_version++ = '\0';
		//略过可能还存在的空白字符。
		m_version += strspn(m_version, " ");
		//比较协议版本是否正确
		if (strcasecmp(m_version, "HTTP/1.1") != 0)
		{
			return bad_request;
		}
		else
		{
			cout<<"parse_requestline: HTTP/1.1"<<endl;
		}

		//比较url的域名头部是否正确，例，此时*url=https://www.runoob.com/
		if (strncasecmp(url, "http://", 7) == 0)
		{
			//url跳到http://之后的域名本身部分，例，此时*url=www.runoob.com/
			url += 7;
			//该函数返回在字符串 str 中第一次出现字符 c 的位置，如果未找到该字符则返回 NULL。
			//url跳到域名本身部分的最后/处，例，此时*url=/
			url = strchr(url, '/');
		}
		if (strncasecmp(url, "https://", 8) == 0)
		{
			//同上
			url += 8;
			url = strchr(url, '/');
		}
		//如果没找到。或者找到了，但是字符不是/，失败
		if (!url && url[0] != '/')
		{
			return bad_request;
		}
		//拼接域名
		if (strlen(url) == 1)
		{
			//把 src 所指向的字符串追加到 dest 所指向的字符串的结尾。
			//*url=/judge.html
			strcat(url, "judge.html");
			cout<<url<<endl;
		}
		//至此请求行分析完毕，开始分析请求头。
		p_state = state_header;
		//请求数据不完整，因为请求头还没解析
		return no_request;
	}

	//解析头部字段
	http_code parse_header(char* temp)
	{
		//字首是空字符，遇到正确的http请求
		if (temp[0] == '\0')
		{
			//内容不为0，就将主状态机转到分析内容
			if (content_length != 0)
			{
				p_state = state_content;
				return no_request;
			}
			//内容为0，完整的客户请求
			return get_request;
		}
		//字首非空字符，处理host字段
		else if (strncasecmp(temp, "Host:", 5) == 0)
		{
			//跳过host:
			temp += 5;
			//略过可能还存在的空白字符
			temp += strspn(temp, "\t");
			//获取Host后面的内容
			host = temp;
			cout<<"host:"<<host<<endl;

		}
		else if (strncasecmp(temp, "Content-length:", 15) == 0)
		{
			temp += 15;
			temp += strspn(temp, "\t");
			//获取Content-length后面的内容
			//将字符串转化为长整数
			content_length = atol(temp);
		}
		else if (strncasecmp(temp, "Connection:", 11) == 0)
		{
			temp += 11;
			temp += strspn(temp, " ");
			cout<<"temp:"<<temp<<endl;
			if (strcasecmp(temp, "keep-alive") == 0)
			{
				linger = true;
				cout<<"linger:"<<linger<<endl;
			}
		}
		else
		{
			//其他头部字段不处理
			cout << "unknown header:" << temp << endl;
		}
		return no_request;
	}

	http_code parse_content(char *temp)
	{
		if (read_index >= (content_length + check_index))
		{
			temp[content_length] = '\0';
			//post请求中最后为输入的用户名和密码
			login_data = temp;
			return get_request;
		}
		return no_request;
	}


	//对请求作出相应的回应
	http_code do_request()
	{
		cout<<"**********************************"<<endl;
		cout<<"已经解析完毕http请求，开始根据请求制作回应报文"<<endl;
		//复制路径到real_file
		strcpy(real_file, root_path);
		int len = strlen(root_path);
		cout<<"real_file:"<<real_file<<endl;
		//该函数返回 str 中最后一次出现字符 c 的位置。如果未找到该值，则函数返回一个空指针。
		cout<<"url:"<<url<<endl;
		char* p = strrchr(url, '/');
		cout<<"p:"<<p<<endl;
		cout<<"cgi:"<<cgi<<endl;
		if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
		{
			//根据标志判断是登录还是注册
			cout<<"开始判断是登陆还是注册"<<endl;


			/*char flag = url[1];
			//分配200个字符串空间
			char* url_real = (char*)malloc(sizeof(char) * 200);
			strcpy(url_real, "/");
			cout<<"url_real:"<<url_real<<endl;
			cout<<"url:"<<url<<endl;
			strcat(url_real, url + 2);
			cout<<"url_real:"<<url_real<<endl;*/
			/*//把 src 所指向的字符串复制到 dest，最多复制 n 个字符。当 src 的长度小于 n 时，dest 的剩余部分将用空字节填充。
			strncpy(real_file + len, url_real, filename_len - len - 1);
			free(url_real);*/



			//将用户名和密码提取出来
			//user=123&password=123
			char name[100]={'\0'};
			char password[100]={'\0'};
			//取用户名
			char* temp_data=login_data;
			strncasecmp(temp_data, "user=", 5);
			login_data+=5;
			for (int i = 0; temp_data[i] != '&'; i++)
				name[i] = temp_data[i];
			//取密码
			strncasecmp(login_data, "password=", 9);
			login_data+=9;
			
			for (int i=0; login_data[i] != '\0'; ++i)
				password[i] = login_data[i];
			cout<<"name:"<<name<<" "<<"password:"<<password<<endl;
			//同步线程登录校验
			if (*(p + 1) == '3')
			{
				cout<<"开始注册校验"<<endl;
				//注册，先检测数据库中是否有重名
				char* sql_insert = (char*)malloc(sizeof(char) * 200);
				//数据库插入语句：INSERT INTO user(username,passwd) VALUES('name','password')
				strcpy(sql_insert, "INSERT INTO user(username,passwd) VALUES(");
				strcat(sql_insert, "'");
				strcat(sql_insert, name);
				strcat(sql_insert, "','");
				strcat(sql_insert, password);
				strcat(sql_insert, "')");

				//在服务器的数据库中取出user数据，进行比较
				//找到了key为name的map，返回迭代器。没找到就返回end()迭代器位置
				if (users.find(name) == users.end())
				{
					cout<<"数据库中没有同名的用户名,注册成功"<<endl;
					h_lock.lock();
					//将用户名和密码加入服务器数据库
					int res = mysql_query(mysql, sql_insert);
					cout<<"res:"<<res<<endl;
					//将用户名密码插入map中
					users.insert(pair<string, string>(name, password));
					h_lock.unlock();

					if (!res)
					{
						strcpy(url, "/log.html");
						cout<<"url:"<<url<<endl;
					}
					else
					{
						strcpy(url, "/registererro.html");
						cout<<"url:"<<url<<endl;
					}
				}
				else
				{
					cout<<"存在同名的注册用户，注册失败"<<endl;
					strcpy(url, "/registererror.html");
					cout<<"url:"<<url<<endl;
				}
			}
			//登录
			else if (*(p + 1) == '2')
			{
				cout<<"开始登录校验"<<endl;
				//比较用户名和密码即可
				if (users.find(name) != users.end() && users[name] == password)
				{
					strcpy(url, "/welcome.html");
					cout<<"url:"<<url<<endl;
				}
				else
				{
					strcpy(url, "/logerror.html");
					cout<<"url:"<<url<<endl;
				}
			}
		}
		if (*(p + 1) == '0')
		{
			char* url_real = (char*)malloc(sizeof(char) * 200);
			strcpy(url_real, "/register.html");
			strncpy(real_file + len, url_real, strlen(url_real));
			cout<<"new rea_file:"<<real_file<<endl;
			free(url_real);
		}
		else if (*(p + 1) == '1')
		{
			char* url_real = (char*)malloc(sizeof(char) * 200);
			strcpy(url_real, "/log.html");
			strncpy(real_file + len, url_real, strlen(url_real));
			cout<<"new rea_file:"<<real_file<<endl;
			free(url_real);
		}
		else if (*(p + 1) == '5')
		{
			char* url_real = (char*)malloc(sizeof(char) * 200);
			strcpy(url_real, "/picture.html");
			strncpy(real_file + len, url_real, strlen(url_real));
			cout<<"new rea_file:"<<real_file<<endl;
			free(url_real);
		}
		else if (*(p + 1) == '6')
		{
			char* url_real = (char*)malloc(sizeof(char) * 200);
			strcpy(url_real, "/video.html");
			strncpy(real_file + len, url_real, strlen(url_real));
			cout<<"new rea_file:"<<real_file<<endl;
			free(url_real);
		}
		else if (*(p + 1) == '7')
		{
			char* url_real = (char*)malloc(sizeof(char) * 200);
			strcpy(url_real, "/fans.html");
			strncpy(real_file + len, url_real, strlen(url_real));
			cout<<"new rea_file:"<<real_file<<endl;
			free(url_real);
		}
		else
		{
			strncpy(real_file + len, url, filename_len - len - 1);
			cout<<"my_new rea_file:"<<real_file<<endl;
		}

		//stat取得指定文件的文件属性，文件属性存储在结构体file_stat里
		if (stat(real_file, &file_stat) < 0)
		{
			cout<<"空文件属性:"<<errno<<endl;
			return no_resource;
		}
		//宏S_IROTH代表其他人拥有读权限
		if (!(file_stat.st_mode & S_IROTH))
		{
			cout<<"无读权限，禁止访问"<<endl;
			return forbid_request;
		}
		//S_ISDIR是否是目录
		if (S_ISDIR(file_stat.st_mode))
		{
			cout<<"是目录"<<endl;
			return bad_request;
		}

		//是确定要访问的文件，打开文件，返回描述符
		cout<<"访问的文件存在"<<endl;
		int fd = open(real_file, O_RDONLY);
		//MAP_PRIVATE私有映射，PROT_READ页内容可以被读取
		//创建一个文件映射区，返回映射区指针，磁盘文件映射进程的虚拟地址空间
		cout<<"文件映射到共享内存"<<endl;
		//首个参数为0表示由系统选择映射文件内容到的地址
		//返回映射内存的指针
		file_address = (char*)mmap(0, file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		cout<<"**********************************"<<endl;
		cout<<endl;
		return file_request;
	}

	//解析http请求的入口函数
	http_code parse_http()
	{
		cout<<"read_buffer:"<<read_buffer<<endl;
		//c_state用来记录当前行的读取状态
		child_state c_state = line_full;
		//ret记录http请求的处理结果
		http_code ret = no_request;
		char* text = 0;
		//主状态机解析内容且从状态机获得完整的行 || 获得完整的行，再赋值给c_state，然后判断c_state是否为获得完整的行
		while ((p_state == state_content && c_state == line_full) || ((c_state = get_full_line()) == line_full))
		{
			cout<<"p_state:"<<p_state<<endl;
			//start_line是行在buffer中的起始位置
			cout<<"start_line:"<<start_line<<endl;
			text = read_buffer + start_line;
			//记录下一行的起始位置，用于
			start_line = check_index;

			//p_state是主机的当前状态
			switch (p_state)
			{
			case state_requestline:
			{
				//解析请求行
				cout<<"**********************************"<<endl;
				cout<<"解析请求行"<<endl;
				ret = parse_requestline(text);
				//请求数据有误
				if (ret == bad_request)
				{
					return bad_request;
				}
				cout<<"**********************************"<<endl;
				cout<<endl;
				break;
			}
			case state_header:
			{
				//解析请求头
				cout<<"**********************************"<<endl;
				cout<<"解析请求头"<<endl;
				ret = parse_header(text);
				//请求数据有误
				if (ret == bad_request)
				{
					return bad_request;
				}
				//获得完整请求 
				else if (ret == get_request)
				{	
					return do_request();
				}
				cout<<"**********************************"<<endl;
				cout<<endl;
				break;
			}
			case state_content:
			{
				cout<<"**********************************"<<endl;
				cout<<"解析请求内容"<<endl;
				//解析请求内容
				ret = parse_content(text);
				//获得完整请求
				if (ret == get_request)
				{
					return do_request();
				}
				//从机获得行不完整
				c_state = line_unfull;
				cout<<"**********************************"<<endl;
				cout<<endl;
				break;
			}
			default:
			{
				//http请求内部错误
				return internel_error;
			}
			}
		}
		return no_request;
	}

	//将要回应客户端的数据格式化到输出缓冲区中
	bool add_response(const char* format, ...)
	{
		if (write_index >= WRITE_BUFFER_SIZE)
		{
			return false;
		}
		//arg_list是一个指向函数参数的指针
		va_list arg_list;
		//va_start函数初始化arg_list指向format
		va_start(arg_list, format);
		//vsnprintf将可变参数格式化输出到一个字符数组。vsnprintf会自动在写入字符的后面加上停止符\0，占用一个字符位。
		int len = vsnprintf(write_buf + write_index, WRITE_BUFFER_SIZE - 1 - write_index, format, arg_list);
		//写入的字节超过了限制，数组越界
		if (len >= (WRITE_BUFFER_SIZE - 1 - write_index))
		{
			//arg_list只为NULL指针
			va_end(arg_list);
			return false;
		}
		//移动到当前写入位置
		write_index += len;
		va_end(arg_list);
		return true;
	}

	//添加回应行
	bool add_status_line(int status, const char* title)
	{
		return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
	}

	//添加内容长度
	bool add_content_length(int content_len)
	{
		return add_response("Content_Length:%d\r\n", content_len);
	}

	//添加连接状态
	bool add_linger()
	{
		return add_response("Connection:%s\r\n", (linger == true) ? "keep-alive" : "close");
	}

	//添加空行
	bool add_blank_line()
	{
		return add_response("%s", "\r\n");
	}

	//增加回应头
	bool add_headers(int content_len)
	{
		add_content_length(content_len);
		add_linger();
		add_blank_line();
	}

	//添加回应的内容
	bool add_content(const char* content)
	{
		return add_response("%s", content);
	}

	//准备要回应客户端的内容
	bool process_write(http_code h_code)
	{
		cout<<"**********************************"<<endl;
		switch (h_code)
		{
		case internel_error:
		{
			add_status_line(500, error_500_title);
			add_headers(strlen(error_500_form));
			if (!add_content(error_500_form))
			{
				return false;
			}
			break;
		}
		case bad_request:
		{
			add_status_line(404, error_404_title);
			add_headers(strlen(error_404_form));
			if (!add_content(error_404_form))
			{
				return false;
			}
			break;
		}
		case forbid_request:
		{
			add_status_line(403, error_403_title);
			add_headers(strlen(error_403_form));
			if (!add_content(error_403_form))
			{
				return false;
			}
			break;
		}
		case file_request:
		{
			//请求到文件，将成功信息和文件内容准备好发送给客户端
			add_status_line(200, ok_200_title);
			if (file_stat.st_size != 0)
			{
				add_headers(file_stat.st_size);
				/*
				struct iovec定义了一个向量元素。通常，这个结构用作一个多元素的数组。对于每一个传输的元素，指针成员iov_base指向一个缓冲区，这个缓冲区是存放的是readv所接收的数据或是writev将要发送的数据。成员iov_len在各种情况下分别确定了接收的最大长度以及实际写入的长度。
				*/
				//write_buf是发送缓冲区
				cout<<"write_buffer:"<<write_buf<<endl;
				iv[0].iov_base = write_buf;
				iv[0].iov_len = write_index;
				//file_address是文件映射的线性空间起始地址。
				iv[1].iov_base = file_address;
				iv[1].iov_len = file_stat.st_size;
				iv_count = 2;
				//发送的字节数，回应行+回应头+回应内容
				send_byte_num = write_index + file_stat.st_size;
				return true;
			}
			else
			{
				//请求文件为空，返回空白网页
				const char* ok_string = "<html><body></body></html>";
				add_headers(strlen(ok_string));
				if (!add_content(ok_string))
				{
					return false;
				}
			}
		}
		default:
			return false;
		}

		//未请求到文件，将错误码等信息准备好发送到客户端
		iv[0].iov_base = write_buf;
		iv[0].iov_len = write_index;
		iv_count = 1;
		send_byte_num = write_index;
		return true;
	}



	void process()
	{
		cout<<"开始处理http请求"<<endl;
		//解析http请求
		http_code read_ret=parse_http();
		//不完整请求，重置fd标志，重新读取数据
		cout<<"read_ret:"<<read_ret<<endl;
		if (read_ret == no_request)
		{
			reset_oneshot(m_epollfd, m_sockfd, EPOLLIN);
			return;
		}
		//其他请求结果，分类处理
		cout<<"开始处理要发送给客户端的数据"<<endl;
		bool write_ret = process_write(read_ret);
		if (!write_ret)
		{
			close_connect(true);
		}
		cout<<"已准备好要发送给客户端的数据报，重置客户端的epollout事件，向发送缓冲区写入数据报"<<endl;
		reset_oneshot(m_epollfd, m_sockfd, EPOLLOUT);
	}

	//取消文件内存映射
	void unmap()
	{
		if (file_address)
		{
			//取消文件内存映射
			munmap(file_address, file_stat.st_size);
			file_address = 0;
		}
	}

	//将回应客户端的数据写入客户端sockfd,相当于send
	bool write()
	{
		int temp = 0;
		//发送的字节为0，出错，重新注册事件，继续监听
		if (send_byte_num == 0)
		{
			reset_oneshot(m_epollfd, m_sockfd, EPOLLIN);
			para_init();
			return true;
		}

		//循环向客户端发送所有回应数据
		while (1)
		{
			//writev以顺序iov[0]、iov[1]至iov[iovcnt-1]从各缓冲区中聚集输出数据到fd
			temp = writev(m_sockfd, iv, iv_count);
			if (temp < 0)
			{
				//writev返回值小于0且错误为EAGAIN，则说明发送缓冲区已满
				//对于非阻塞io，如果fd读缓冲区为空无数据可读或者写缓冲区已满无法写入数据，调用不会阻塞，会返回EAGAIN错误
				if (errno == EAGAIN)
				{
					//保存剩下的数据，注册EPOLLOUT事件，当此事件触发时，说明发送缓冲区有空位，继续发送剩余的数据
					cout<<"缓冲区已满"<<endl;
					reset_oneshot(m_epollfd, m_sockfd, EPOLLOUT);
					return true;
				}
				//发送出错，取消映射，结束
				else
				{
					unmap();
					return false;
				}
			}

			//已写入
			have_send_num += temp;
			//剩余待写入
			send_byte_num -= temp;
			
			//缓冲区1已全部写入或者缓冲区2也部分写入
			if (have_send_num >= iv[0].iov_len)
			{
				iv[0].iov_len = 0;
				iv[1].iov_base = file_address + (have_send_num - write_index);
				iv[1].iov_len = send_byte_num;
			}
			else
			{
				iv[0].iov_base = write_buf + have_send_num;
				iv[0].iov_len = iv[0].iov_len - have_send_num;
			}
			
			//所有要发送给客户端的数据已全部发送完毕。
			if (send_byte_num <= 0)
			{
				unmap();
				cout<<"所有数据已发送给客户端，再次注册EPOLLIN事件"<<endl;
				reset_oneshot(m_epollfd, m_sockfd, EPOLLIN);

				//
				if (linger)
				{
					//保持连接状态，那么初始化连接参数，等待接收此socket的下一次请求
					cout<<"此次请求已处理完毕，初始化连接参数，等待下次请求的连接"<<endl;
					para_init();
					return true;
				}
				else
				{
					return false;
				}
			}
		}
	}
};


int http::m_epollfd=-1;
int http::m_user_count=0;
