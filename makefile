server: main.cpp ./threadpool/threadpool.h ./request/request_process.cpp ./request/request_process.h ./locker/locker.h ./sqlconnpool/sql_connection_pool.cpp ./sqlconnpool/sql_connection_pool.h ./log/log.cpp ./log/log.h ./log/block_queue.h ./config/config.h ./config/config.cpp
	g++ -o server main.cpp ./threadpool/threadpool.h ./request/request_process.cpp ./request/request_process.h ./locker/locker.h ./sqlconnpool/sql_connection_pool.cpp ./sqlconnpool/sql_connection_pool.h ./log/log.cpp ./log/log.h ./log/block_queue.h ./config/config.h ./config/config.cpp -L/usr/lib64/mysql -lmysqlclient -lpthread -std=c++11


clean:
	rm  -r server
