
/*********************************************************
 *
 *  Copyright (C) 2014 by Vitaliy Vitsentiy
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *********************************************************/


#ifndef __ctpl_thread_pool_H__
#define __ctpl_thread_pool_H__

#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <exception>
#include <future>
#include <mutex>
#include <boost/lockfree/queue.hpp>

// thread pool to run user's functors with signature
//      ret func(thread_id id, other_params)
// where id is the index of the thread that runs the functor
// ret is some return type

namespace ctpl {

    using thread_id = std::size_t;

    class thread_pool {
      static constexpr std::size_t defaultQueueLength = 100;

    public:
        thread_pool(std::size_t nThreads,
                    std::size_t queueSize = defaultQueueLength)
                : q(queueSize) {
            this->init();
            this->resize(nThreads);
        }

        // the destructor waits for all the functions in the queue to be finished
        ~thread_pool() {
            this->stop(true);
        }

        // get the number of running threads in the pool
        std::size_t size() { return this->threads.size(); }

        // number of idle threads
        std::size_t n_idle() { return this->nWaiting; }
        std::thread & get_thread(std::size_t i) { return *this->threads[i]; }

        // change the number of threads in the pool
        // should be called from one thread, otherwise be careful to not interleave, also with this->stop()
        // nThreads must be >= 0
        void resize(std::size_t nThreads) {
            if (!this->isStop && !this->isDone) {
                auto oldNThreads = this->threads.size();
                if (oldNThreads <= nThreads) {  // if the number of threads is increased
                    this->threads.resize(nThreads);
                    this->flags.resize(nThreads);

                    for (std::size_t i = oldNThreads; i < nThreads; ++i) {
                        this->flags[i] = std::make_shared<std::atomic_bool>(false);
                        this->set_thread(i);
                    }
                }
                else {  // the number of threads is decreased
                    for (std::size_t i = nThreads; i < oldNThreads; ++i) {
                        *this->flags[i] = true;  // this thread will finish
                        this->threads[i]->detach();
                    }
                    {
                        // stop the detached threads that were waiting
                        std::unique_lock<std::mutex> lock(this->mutex);
                        this->cv.notify_all();
                    }
                    this->threads.resize(nThreads);  // safe to delete because the threads are detached
                    this->flags.resize(nThreads);  // safe to delete because the threads have copies of shared_ptr of the flags, not originals
                }
            }
        }

        // empty the queue
        void clear_queue() {
            std::function<void(thread_id id)> * _f;
            while (this->q.pop(_f))
                delete _f;  // empty the queue
        }

        // pops a functional wraper to the original function
        std::function<void(thread_id)> pop() {
            std::function<void(thread_id id)> * _f = nullptr;
            this->q.pop(_f);
            std::unique_ptr<std::function<void(thread_id id)>> func(_f);  // at return, delete the function even if an exception occurred

            std::function<void(thread_id)> f;
            if (_f)
                f = *_f;
            return f;
        }


        // wait for all computing threads to finish and stop all threads
        // may be called asyncronously to not pause the calling thread while waiting
        // if isWait == true, all the functions in the queue are run, otherwise the queue is cleared without running the functions
        void stop(bool isWait = false) {
            if (!isWait) {
                if (this->isStop)
                    return;
                this->isStop = true;
                for (std::size_t i = 0, n = this->size(); i < n; ++i) {
                    *this->flags[i] = true;  // command the threads to stop
                }
                this->clear_queue();  // empty the queue
            }
            else {
                if (this->isDone || this->isStop)
                    return;
                this->isDone = true;  // give the waiting threads a command to finish
            }
            {
                std::unique_lock<std::mutex> lock(this->mutex);
                this->cv.notify_all();  // stop all waiting threads
            }
            for (std::size_t i = 0; i < this->threads.size(); ++i) {  // wait for the computing threads to finish
                if (this->threads[i]->joinable())
                    this->threads[i]->join();
            }
            // if there were no threads in the pool but some functors in the queue, the functors are not deleted by the threads
            // therefore delete them here
            this->clear_queue();
            this->threads.clear();
            this->flags.clear();
        }

        template<typename F, typename... Rest>
        auto push(F && f, Rest&&... rest) {
            auto bound_fcn = [f = std::forward<F>(f), args = std::forward_as_tuple(rest...)] (thread_id id) {
                return std::apply([f = std::move(f), id](auto &&...rest) {
                  return f(id, std::forward<Rest>(rest)...);
                }, std::move(args));
            };
            using ret_t = std::result_of_t<F(thread_id, Rest...)>;
            auto pck = std::make_shared<std::packaged_task<ret_t(thread_id)>>(
                std::move(bound_fcn)
            );

            auto _f = new std::function<void(thread_id id)>([pck](thread_id id) {
                (*pck)(id);
            });
            this->q.push(_f);

            std::unique_lock<std::mutex> lock(this->mutex);
            this->cv.notify_one();

            return pck->get_future();
        }

        // run the user's function that excepts argument int - id of the running thread. returned value is templatized
        // operator returns std::future, where the user can get the result and rethrow the catched exceptins
        template<typename F>
        auto push(F && f) ->std::future<decltype(f(0))> {
            auto pck = std::make_shared<std::packaged_task<decltype(f(0))(thread_id)>>(std::forward<F>(f));

            auto _f = new std::function<void(thread_id id)>([pck](thread_id id) {
                (*pck)(id);
            });
            this->q.push(_f);

            std::unique_lock<std::mutex> lock(this->mutex);
            this->cv.notify_one();

            return pck->get_future();
        }


    private:

        // deleted
        thread_pool(const thread_pool &);// = delete;
        thread_pool(thread_pool &&);// = delete;
        thread_pool & operator=(const thread_pool &);// = delete;
        thread_pool & operator=(thread_pool &&);// = delete;

        void set_thread(thread_id i) {
            std::shared_ptr<std::atomic<bool>> flag(this->flags[i]);  // a copy of the shared ptr to the flag
            auto f = [this, i, flag/* a copy of the shared ptr to the flag */]() {
                std::atomic<bool> & _flag = *flag;
                std::function<void(thread_id id)> * _f;
                bool isPop = this->q.pop(_f);
                while (true) {
                    while (isPop) {  // if there is anything in the queue
                        std::unique_ptr<std::function<void(thread_id id)>> func(_f);  // at return, delete the function even if an exception occurred
                        (*_f)(i);

                        if (_flag)
                            return;  // the thread is wanted to stop, return even if the queue is not empty yet
                        else
                            isPop = this->q.pop(_f);
                    }

                    // the queue is empty here, wait for the next command
                    std::unique_lock<std::mutex> lock(this->mutex);
                    ++this->nWaiting;
                    this->cv.wait(lock, [this, &_f, &isPop, &_flag](){ isPop = this->q.pop(_f); return isPop || this->isDone || _flag; });
                    --this->nWaiting;

                    if (!isPop)
                        return;  // if the queue is empty and this->isDone == true or *flag then return
                }
            };
            this->threads[i].reset(new std::thread(f));  // compiler may not support std::make_unique()
        }

        void init() { this->nWaiting = 0; this->isStop = false; this->isDone = false; }

        std::vector<std::unique_ptr<std::thread>> threads;
        std::vector<std::shared_ptr<std::atomic<bool>>> flags;
        mutable boost::lockfree::queue<std::function<void(thread_id id)> *> q;

        std::atomic_bool isDone;
        std::atomic_bool isStop;
        std::atomic_size_t nWaiting;  // how many threads are waiting

        std::mutex mutex;
        std::condition_variable cv;
    };

}

#endif // __ctpl_thread_pool_H__

