#include <time.h>
#include<netinet/in.h>
#define sock_buffer_size 64
#include<iostream>
using namespace std;

//定时器类前向申明
class util_timer;


//用户数据结构
class client_data
{
public:
	//用户地址
	sockaddr_in address;
	//sokcet描述符
	int sockfd;
	//定时器
	util_timer* timer;
};

//定时器类
class util_timer
{
public:
	util_timer():prev(NULL),next(NULL){}

public:
	//任务的超时时间
	time_t expire;
	//任务回调函数指针
	void (*cb_func)(client_data*);
	//定时器关联的用户数据,用于回调函数处理
	client_data* user_data;
	//定时器指针
	util_timer* prev;
	util_timer* next;
};


//升序定时器链表类,按超时时间大小排序，最先要到期的任务排在最前面
class sort_timer_lst
{
private:
	util_timer* head;
	util_timer* tail;
	
public:
	sort_timer_lst():head(NULL),tail(NULL){}
	~sort_timer_lst()
	{
		util_timer* tmp=head;
		while(tmp)
		{
			head=tmp->next;
			delete tmp;
			tmp=head;
		}
	}


	//将定时器加入链表
	void add_timer(util_timer* timer)
	{
		if(!timer)
		{
			return;
		}
		//首次插入节点
		if(!head)
		{
			head=tail=timer;
		}
		//插入头结点之前
		if(timer->expire<head->expire)
		{
			timer->next=head;
			head->prev=timer;
			head=timer;
			return;
		}
		//重载函数，加入合适的位置
		add_timer(timer,head);
		cout<<"定时器已加入链表"<<timer<<endl;
	}


	//定时器到期时间延长，调整定时器的位置，向后移动
	void adjust_timer(util_timer* timer)
	{
		if(!timer)
		{
			return;
		}
		//temp为调整节点的下一结点
		util_timer* temp=timer->next;
		//为尾结点或者时间没有超过下一个结点，则不需要移动
		if(!temp || timer->expire<temp->expire)
		{
			return;
		}
		//调整节点为头结点，拿出来调整
		if(timer==head)
		{
			head=timer->next;
			head->prev=NULL;
			timer->next=NULL;
			add_timer(timer,head);
		}
		//调整节点不是头结点，取出调整
		else
		{
			timer->prev->next=timer->next;
			timer->next->prev=timer->prev;
			add_timer(timer,head);
		}
	}


	//删除定时器
	void del_timer(util_timer* timer)
	{
		if(!timer)
		{
			return;
		}
		//只有一个结点
		if(timer==head && timer==tail)
		{
			delete timer;
			head=tail=NULL;
			return;
		}
		//许多结点，删除头结点
		if(timer==head)
		{
			head=head->next;
			head->prev=NULL;
			delete timer;
			return;
		}
		//许多结点，删除尾结点
		else if(timer==tail)
		{
			tail=tail->prev;
			tail->next=NULL;
			delete timer;
			return;
		}
		//许多结点，删除中间结点
		else
		{
			timer->prev->next=timer->next;
			timer->next->prev=timer->prev;
			delete timer;
		}
	}


	//处理链表上的定时器到期任务
	void tick()
	{
		if(!head)
		{
			return;
		}
		//获取系统当前时间
		time_t cur_time=time(NULL);
		//从头结点开始处理定时器，直到遇到一个还未到期的定时器,或者遍历完链表
		util_timer* temp=head;
		while(temp)
		{
			//定时器还没到期
			if(cur_time<temp->expire)
			{
				break;
			}
			//定时器到期了,执行定时器的回调函数处理具体事务
			cout<<"定时器"<<temp<<"到期了"<<endl;
			temp->cb_func(temp->user_data);
			//执行完任务后，删除此定时器
			//因为一定是从头结点开始删除，所以删除定时器要先换头结点
			head=temp->next;
			if(head)
			{
				head->prev=NULL;
			}
			delete temp;
			temp=head;
		}
	}

private:
	//add_timer重载函数，供公有函数调用
	void add_timer(util_timer* timer,util_timer* head)
	{
		util_timer* temp=head->next;
		//遍历head之后的链表，直到找到一个超时时间大于当前定时器的结点
		while(temp)
		{
			if(timer->expire<temp->expire)
			{
				temp->prev->next=timer;
				timer->prev=temp->prev;
				timer->next=temp;
				temp->prev=timer;
				break;
			}
			temp=temp->next;
		}

		//遍历到链表尾部，仍没有找到合适的插入位置，则直接插入尾部
		if(!temp)
		{
			tail->next=timer;
			timer->prev=tail;
			timer->next=NULL;
			tail=timer;
		}
	}
};
