#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>

class ThreadPool {
public:
    ThreadPool(size_t);     // 构造函数
    template<class F, class... Args>        
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type>;      // 向任务队列添加任务
    ~ThreadPool();      // 析构函数
private:
    // need to keep track of threads so we can join them
    std::vector< std::thread > workers;     // 工作线程队列
    // the task queue
    std::queue< std::function<void()> > tasks;      // 任务队列
    
    // synchronization
    std::mutex queue_mutex;     // 互斥锁
    std::condition_variable condition;      // 条件变量 配合互斥锁 实现睡眠锁的效果
    bool stop;      // 线程池关闭的标志，stop为真后每个线程都会结束运行
};
 
// the constructor just launches some amount of workers
// 构造函数
inline ThreadPool::ThreadPool(size_t threads)
    :   stop(false)
{
    // 创建指定数量的线程
    for(size_t i = 0;i<threads;++i)
        workers.emplace_back(
            [this]
            {
                for(;;)
                {
                    std::function<void()> task;

                    {   // 这个多余大括号是为了构造一个作用域，unique_lock离开作用域后自动销毁，通过多余大括号可以调整锁的粒度
                        // 获取任务队列锁，unique_lock会在离开作用域后自动销毁
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        // 条件变量配合lock实现睡眠锁的效果
                        this->condition.wait(lock,
                            [this]{ return this->stop || !this->tasks.empty(); });
                        // 这里可以控制线程结束运行
                        if(this->stop && this->tasks.empty())
                            return;
                        // 获取一个任务
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }

                    // 执行任务
                    task();
                }
            }
        );
}

// add new work item to the pool
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)     // 可变参数模板
    -> std::future<typename std::result_of<F(Args...)>::type>   // 异步运行的结果可以通过future获取
{
    // 该任务的返回类型
    using return_type = typename std::result_of<F(Args...)>::type;

    // packaged_task封装，异步调用，任务运行结果可以通过与该packaged_task对应的future获得
    auto task = std::make_shared< std::packaged_task<return_type()> >(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
    
    // packaged_task对应的future
    std::future<return_type> res = task->get_future();
    {   // 多余的大括号用以指定unique_lock的作用范围
        std::unique_lock<std::mutex> lock(queue_mutex);

        // don't allow enqueueing after stopping the pool
        if(stop)
            throw std::runtime_error("enqueue on stopped ThreadPool");

        tasks.emplace([task](){ (*task)(); });
    }
    // 唤醒一个工作线程
    condition.notify_one();
    return res;
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool()
{
    {   // 多余的大括号用以指定unique_lock的作用范围
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    // 唤醒所有工作线程
    condition.notify_all();
    // 等待每个工作线程结束
    for(std::thread &worker: workers)
        worker.join();
}

#endif
