#pragma once
#include <pthread.h>
#include <list>
#include"../http/http.h"
#include"../mysqlconn/sql_connect_pool.h"
using namespace std;

//线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<class T>
class threadpool
{

private:
	//线程池中的线程数
	int m_thread_number;
	//描述线程池的数组，pthread_t是一个线程标识符
	pthread_t* m_threads;
	//请求队列
	list<T*> m_workqueue;
	//请求队列中允许的最大请求数
	int m_max_requests;
	//保护请求队列的互斥锁
	locker m_queuelocker;
	//是否有任务需要处理,条件变量
	sem m_queuestat;
	//是否结束线程标志
	bool m_stop;
	//数据库连接池对象指针
	sql_connect_pool* m_pool;

public:

	//创建线程组成线程池
	threadpool(sql_connect_pool* s_c_pool,int thread_number=8,int max_request=100):
		m_thread_number(thread_number),
		m_max_requests(max_request),
		m_stop(false),
		m_threads(NULL),
		m_pool(s_c_pool)
	{
		if((thread_number<=0)||(max_request<=0))
		{
			cout<<"pool init fail"<<endl;
		}

		//预先分配好线程标识符数组，数组中的元素作为创建新线程的标识符，实现线程池的功能
		m_threads=new pthread_t[m_thread_number];

		//创建thread_number个线程，并将它们设置为脱离线程
		for(int i=0;i<thread_number;i++)
		{
			cout<<"create thread "<< i <<endl;
			//创建线程
			if(pthread_create(m_threads+i,NULL,worker,this)!=0)
			{
				cout<<"create thread fail"<<endl;
			}
			//分离主子线程，分离成功后，子线程结束自动回收资源
			if(pthread_detach(m_threads[i]))
			{
				cout<<"detach thread fail"<<endl;
			}
		}
	}

	~threadpool()
	{
		delete [] m_threads;
		m_stop=true;
	}

	bool append(T *request)
	{
		cout<<"将任务加入到请求队列"<<endl;
		//操作工作队列时一定要加锁，因为它被所有线程共享
		m_queuelocker.lock();
		//当前请求队列的大小大于请求队列的最大值，则无法向请求队列加入新的请求
		if(m_workqueue.size()>m_max_requests)
		{
			m_queuelocker.unlock();
			return false;
		}
		//将请求加入请求队列
		m_workqueue.push_back(request);
		m_queuelocker.unlock();
		
		//发送有任务可做的条件变量
		cout<<"条件变量发送通知"<<endl;
		m_queuestat.post();
		return true;
	}

private:
	//工作线程运行的函数，他不断的从工作队列中取出任务并执行
	static void* worker(void* arg)
	{
		//由线程传递过来的threadpool的实例化对象arg
		cout<<"线程进入入口函数"<<endl;
		threadpool<http>* pool=(threadpool*)arg;
		pool->run();
		return pool;
	}

	//
	void run()
	{
		//线程结束标志不打开，就一直while循环
		while(!m_stop)
		{
			cout<<"等待信号量"<<endl;
			//等到条件变量就结束阻塞，继续运行。
			m_queuestat.wait();
			cout<<"信号量等到通知"<<endl;
			m_queuelocker.lock();
			if(m_workqueue.empty())
			{	
				cout<<"请求队列为空"<<endl;
				m_queuelocker.unlock();
				continue;
			}
			//从请求队列前面取出一个请求
			T* request=m_workqueue.front();
			cout<<"成功取出请求"<<request<<endl;
			//删除这个请求
			m_workqueue.pop_front();
			m_queuelocker.unlock();
			//请求为空，跳过这次循环
			if(!request)
			{
				continue;
			}
			
			//从数据库连接池中取出一个MYSQL对象，供http任务使用
			request->mysql=m_pool->get_connection();

			//处理http请求
			request->process();

			//使用完再次释放连接池对象
			m_pool->release_connection(request->mysql);
			
		}
	}
};
