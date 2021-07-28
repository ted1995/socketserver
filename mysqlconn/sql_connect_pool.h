#pragma once
#include<mysql/mysql.h>
#include"../lock/locker.h"
#include<list>
#include<string>
#include<iostream>
using namespace std;


//数据库连接池实例
class sql_connect_pool
{
private:
	//数据库连接池
	list<MYSQL*> sql_pool;
	//信号量
	sem reverse;
	//互斥锁
	static locker lock;

	static sql_connect_pool* scp;
	//已用连接
	int use_connect;
	//未用连接
	int free_connect;
	//最大连接
	int max_connect;

	string url;					//主机
	string port;				//端口
	string user;				//用户名
	string password;			//密码
	string databasename;		//数据库名

	sql_connect_pool()
	{
		max_connect=8;
	}

public:
	//释放所有数据库连接
	~sql_connect_pool()
	{
		lock.lock();
		if(sql_pool.size()>0)
		{
			for(list<MYSQL*>::iterator it=sql_pool.begin();it!=sql_pool.end();it++)
			{
				MYSQL* temp=*it;
				mysql_close(temp);
			}
			free_connect=0;
			use_connect=0;
			sql_pool.clear();
		}
		lock.unlock();
	}

	//创建数据库连接
	void init(string url, string user, string password, string databasename, int port)
	{
		this->url = url;
		this->user = user;
		this->password = password;
		this->databasename = databasename;
		this->port = port;
	
		lock.lock();
		for(int i=0;i<max_connect;i++)
		{
			MYSQL* mysql=NULL;
			mysql=mysql_init(mysql);
			mysql=mysql_real_connect(mysql,url.c_str(),user.c_str(),password.c_str(),databasename.c_str(),port,NULL,0);
			if(mysql==NULL)
			{
				cout<<mysql_error(mysql)<<endl;
				return;
			}
			cout<<"sql connect success"<<mysql<<endl;
			sql_pool.push_back(mysql);
			free_connect++;
		}
		//初始化信号量
		reverse=sem(free_connect);

		lock.unlock();
	}


	//单例懒汉模式，获取实例
	static sql_connect_pool* get_sql_pool()
	{
		//如果此连接池实例为空，创建一个，如果不为空，直接返回此实例
	    if(scp==nullptr)
	    {
	        lock.lock();
	        if(scp==nullptr)
	        {
	            scp=new sql_connect_pool();
	        }
	        lock.unlock();
	    }
	    return scp;
	}




	//获取一个MYSQL
	MYSQL* get_connection()
	{
		MYSQL* mysql = NULL;
		//信号量
		reverse.wait();
		//加锁
		lock.lock();
		mysql = sql_pool.front();
		sql_pool.pop_front();
		free_connect--;
		use_connect++;
		lock.unlock();
		return mysql;
	}


	//释放一个MYSQL
	bool release_connection(MYSQL* mysql)
	{
		if(mysql==NULL)
		{
			return false;
		}
		lock.lock();
		sql_pool.push_back(mysql);
		free_connect++;
		use_connect--;
		lock.unlock();
		reverse.post();
		return true;
	}	

};

locker sql_connect_pool::lock;
sql_connect_pool* sql_connect_pool::scp;

