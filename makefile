server: main.cpp ./threadpool/threadpool.h ./http/http.h ./lock/locker.h ./mysqlconn/sql_connect_pool.h 
	g++ -g -std=c++11 -o server main.cpp ./threadpool/threadpool.h ./http/http.h ./lock/locker.h ./mysqlconn/sql_connect_pool.h  -lpthread -lmysqlclient


clean:
	rm  -r server
