#ifndef THREADPOOL_H
#define THREADPOOL_H


#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include<thread>
#include <iostream>
#include <unordered_map>
#include <future>

const int TASK_MAX_THRESHHOLD = 4;   //任务队列任务个数上限
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;  //线程最大空闲时间，单位秒

enum class PoolMode
{
	MODE_FIXED,     //固定模式，
	MODE_CACHED,    //可增长模式
};


//线程类型
class Thread
{
public:
	using ThreadFunc = std::function<void(int)>;

	Thread(ThreadFunc func)
		:func_(func)    //获取bind返回的函数对象，即threadpool中定义的线程函数
		, threadId_(generateId_++)
	{}

	~Thread() = default;


	void start() 
	{
		std::thread t(func_, threadId_);   //线程对象t 和线程函数func_
		std::cout << "线程id:" << t.get_id() << "线程已创建" << std::endl;
		t.detach();     //设置分离线程
	}//创建一个std::thread


	int getThreadId() const
	{
		return threadId_;
	}//获取线程id//获取线程id

private:
	static int generateId_;
	ThreadFunc func_;  //函数对象类型
	int threadId_;
};
int Thread::generateId_ = 0;   //类外初始化静态变量



//线程池类型
class ThreadPool
{
public:

	ThreadPool()
		:initThreadSize_(0)
		, taskSize_(0)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, idleThreadSize_(0)
		, isPoolRunning_(false)
		, curThreadSize_(0)
		, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
	{}//线程池构造函数

	//线程池析构函数
	~ThreadPool()
	{
		isPoolRunning_ = false;     //设置线程状态
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();     //唤醒所有等待线程
		poolExit_.wait(lock, [&]()->bool {return threads_.size() == 0; });    //等待线程全部回收
	}

	void  setMode(PoolMode mode)
	{
		if (checkRunningState())    //如果处于运行状态则不可以修改
			return;
		poolMode_ = mode;
	};   //设置线程池模式



	
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())    //如果处于运行状态则不可以修改
			return;
		taskQueMaxThreshHold_ = threshhold;
	};//设置任务队列阈值


	void setThreadSizeMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreshHold_ = threshhold;
		}
	}//设置chached模式下最大线程数量



	//可变参模板,接收任意参数类型个数 
	template<typename Func, typename...Args>
	auto submitTask(Func&& func, Args&&... args)->std::future<decltype(func(args...))>  //T&&万能引用类型
	{
		using RType = decltype(func(args...));
		//bind消除参数
		auto task = std::make_shared<std::packaged_task<RType()>>
			(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));  //参数展开，不是折叠表达式！！！
		std::future<RType> result = task->get_future();
                
		//获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		//等待任务队列右空余,超过一秒钟返回失败
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskSize_ < taskQueMaxThreshHold_; }))
		{
			std::cerr << "task queue if full , submit is fail" << std::endl;
			//return std::future<Rtype>();
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType { return RType(); });
			(*task)();
			return task->get_future();
		};
		//提交任务
		taskQue_.emplace([task]() {(*task)(); });  //通过添加一个中间层,消除返回值类型
		taskSize_++;
		notEmpty_.notify_all();

		//根据空闲线程数量和任务数量，判断是否需要添加新的线程
		if (poolMode_ == PoolMode::MODE_CACHED &&
			taskSize_ > idleThreadSize_ &&
			curThreadSize_ < threadSizeThreshHold_)
		{
			//创建新的线程
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadid = ptr->getThreadId();         //获取id
			threads_.emplace(threadid, std::move(ptr));

			threads_[threadid]->start();   //启动线程
			curThreadSize_++;           //当前以及空闲线程加一
			idleThreadSize_++; 
		}
		return result;  //返回result对象
	};   //用户提交任务接口


	void start(int initThreadSize)
	{
		//设置线程池状态
		isPoolRunning_ = true;
		//初始化线程数量
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;
		//创建线程
		for (int i = 0; i < initThreadSize; i++)
		{
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			threads_.emplace(ptr->getThreadId(), std::move(ptr));
		}

		//启动线程
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();
			idleThreadSize_++;
		}
	};//开启线程池
	//关闭拷贝构造函数


	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;
private:
	//定义线程函数
	//////////////////////////////////////////////////////定义线程函数
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();
		while (true)
		{
			
			Task task;    //std::queue<std::function<void()>>  taskQue_; //任务队列
			{
				//获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);     //尝试获取锁
				std::cout << std::this_thread::get_id() << "尝试获取任务" << std::endl;


				//每秒返回一次     区分超时返回，还是有任务待执行返回
				while (taskQue_.size() == 0)     //有任务直接取执行任务
				{
					if (!isPoolRunning_)     //没任务时，判断线程池是否结束，结束直接退出函数，结束线程
					{
						curThreadSize_--;
						threads_.erase(threadid);
						poolExit_.notify_all();
						std::cout << "threadid:" << std::this_thread::get_id() << "已回收" << std::endl;
						return;
					}

					//线程池未结束，进入等待状态
					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						//cached模式下，空闲时间超过60s的线程,进行回收
						//当前时间-last上次线程执行完成时间
						if (std::cv_status::timeout == 
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))   //超时退出
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto cur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);    //计算停止时间
							if (cur.count() >= THREAD_MAX_IDLE_TIME &&
								curThreadSize_ > initThreadSize_)
							{
								//超时回收线程
								threads_.erase(threadid);
								curThreadSize_--;
								idleThreadSize_--;
								std::cout << "threadid:" << std::this_thread::get_id() <<
									"已回收" << std::endl;
								poolExit_.notify_all();
								return;
							}
						}
					}
					else
					{
						//等待notempty,fixed模式
						notEmpty_.wait(lock);
					}
				}
				idleThreadSize_--;
				//从队列中取一个任务
				task = taskQue_.front();
				if (taskQue_.empty())std::cout << "error!!!!!!!!!!!!!!!!" << std::endl;
				taskQue_.pop();
				taskSize_--;

				//通知其他线程
				if (taskQue_.size() > 0)notEmpty_.notify_all();

				//std::cout << std::this_thread::get_id() << "获取任务成功" << std::endl;
				notFull_.notify_all();
				//执行任务
			}	

			if (task != nullptr)
			{
				task();
			}

			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now();
		}
	}

	bool checkRunningState() const
	{
		return isPoolRunning_;   //未来可能有更多的判断
	}

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;  //线程列表
	int initThreadSize_;  //初始线程数量
	std::atomic_int idleThreadSize_; //空闲线程数量
	int threadSizeThreshHold_;     //线程数量的上限
	std::atomic_int curThreadSize_;  //当前线程数量

	using Task = std::function<void()>;
	std::queue<Task>  taskQue_; //任务队列
	std::atomic_int taskSize_;   //任务数量
	int taskQueMaxThreshHold_;   //最大任务数量

	std::mutex taskQueMtx_;  //任务队列互斥锁
	std::condition_variable notFull_;  //表示任务队列不满，用户可以添加任务
	std::condition_variable notEmpty_; //表示任务队列不空，线程可以消耗任务

	PoolMode poolMode_;   //当前线程池的模式
	std::atomic_bool isPoolRunning_;//当前线程池状态
	std::condition_variable poolExit_;
};
#endif

