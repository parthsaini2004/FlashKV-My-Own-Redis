// purpose :Execute tasks without creating threads per request.


// ##########################################################################################

// ##### why not create a Thread-per-request rather a Thread Pool :
// Client connects → spawn thread → handle request → kill thread
// Cost: 50-100μs per thread creation/destruction
// 10K requests → 500-1000ms just for thread management,i.e:

// Context Switching: If you spawn 10,000 threads for 10,000 requests, 
// the OS spends more time switching between threads (context switching) than actually doing work. 
// A pool limits active threads to the number of CPU cores, maximizing CPU throughput.

// ##### thread pool creation:
// Amortization: Thread creation is expensive (kernel calls, stack allocation ~1MB, OS scheduler overhead).
// By creating them once, we pay this cost at startup ($T=0$), 
// making runtime request handling nearly "free" in terms of resource allocation.

// ##### Producer-Consumer Pattern:
// The Producer (Main Thread): Its only job is to accept incoming connections (or data packets), wrap them into a Task object,
// and push them into a safe place. It wants to offload work ASAP so it can go back to listening for new clients.
// The Buffer (Task Queue): This is the "safe place." It decouples the rate of arrival (requests coming in) from the rate of processing (workers executing tasks).
// The Consumers (Worker Threads): These are the heavy lifters. They monitor the buffer. When work appears, they grab it, execute it, and go back to sleep.

// ##### Synchronization Primitives:
// Mutex (std::mutex): A lock that ensures only one thread touches the queue at a time.
// Condition Variable (std::condition_variable): The communication channel.
// Workers go to sleep at the kernel level. They use zero CPU. 
// When the Main thread pushes a task, it "signals" (rings a bell), and the OS wakes up one worker.

// ##### Proces_flow thread_pool:
// Phase 1: Submission (The Main Thread)
// Acquire Lock: The main thread locks the mutex.
// Check Capacity (Backpressure): It checks queue.size(). If the queue is full (e.g., >10,000 pending tasks), it rejects the request immediately.
// Why? To prevent "Out of Memory" (OOM) crashes. Better to drop a request than crash the server.
// Push: If space exists, it pushes the Task struct (Client FD + Command) into the queue.
// Signal: It calls notify_one() on the condition variable.
// Release: It unlocks the mutex.

// Phase 2: The Wake Up (The Worker Thread)
// Wait: The worker is currently sleeping inside cv.wait().
// Wake: The signal arrives. The worker wakes up and automatically attempts to re-acquire the mutex.
// Check (Spurious Wakeup Protection): It checks: Is the queue actually non-empty? Or did I get woken up by accident? (This happens in OS scheduling). It loops until there is real work.
// Pop: It takes the task from the front of the queue.
// Unlock: It releases the mutex immediately.
// Critical Optimization: We only hold the lock while taking the work, not while doing the work. This allows other workers to grab other tasks while this one processes.

// Phase 3: Execution
// Process: The worker parses the command (using your Module 2 Parser) and calls the Storage Engine (Module 3).
// Respond: The worker writes the result back to the Client's socket (send()).
// Loop: The worker goes back to step 1 (Wait).

// A.)
// ### > Spurious Wakeups :never write : if (queue.notEmpty) wait(). 
//  always write:
//     while (queue.empty() && !stop_flag) {
//         cv.wait(lock);
//     }
//  a while loop because POSIX condition variables can suffer from Spurious Wakeups, 
//  where a thread wakes up without a signal due to OS internal signal handling optimization. 
//  The loop re-verifies the condition to ensure correctness.

// B.) 
// ###> Granularity of LockingBad Design: 
// Holding the lock while processing the request (Parsing/Storage). This turns your multi-threaded server into a single-threaded one effectively.
// Good Design: Hold lock -> Pop Task -> Release lock -> Process Task.
// minimized the Critical Section. The lock is held only for queue manipulation (nanoseconds). 
// The heavy lifting (IO, Storage) happens outside the lock, allowing true parallelism.

// C.)
// ###> Backpressure (Bounded Queue)
// implemented a bounded queue to enforce Backpressure. If the system is overloaded, 
// prefer to fail fast and let the load balancer/client retry, rather than queuing infinite tasks 
// which leads to memory exhaustion and massive latency spikes (Little's Law).


// ##### Conditional Variable Explanation:
// 1. WITHOUT Condition Variable (Busy Waiting / Polling)
// You are forced to choose between two evils: burning CPU or high latency.

// // The "Bad" Loop
// void workerLoop() {
//     while (true) {
//         mutex.lock();
//         if (!queue.empty()) {
//            // take task
//         }
//         mutex.unlock();
        
//         // EVIL CHOICE:
//         std::this_thread::sleep_for(std::chrono::milliseconds(1)); 
//     }
// }
// The Latency Trap: If you sleep for 1ms, and a request arrives at 0.1ms, that request sits in the queue rotting for 0.9ms until you wake up. In High-Frequency Trading or real-time systems, 0.9ms is an eternity.
// The CPU Trap: To fix latency, you might remove the sleep. Now the loop runs millions of times per second. Your CPU core hits 100% usage doing nothing but checking an empty queue. This heats up the server and slows down other useful threads.
// The Lock Contention: Your worker is constantly locking and unlocking the mutex. This "pollutes" the lock. When the Main Thread tries to acquire the lock to actually push a task, it might have to wait because your worker locked it just to check if it was empty.

// 2. WITH Condition Variable (Signaling)
// This approach uses the Operating System's kernel to manage the thread state.

// // The "Good" Flow
// void workerLoop() {
//     std::unique_lock<std::mutex> lock(mutex);
    
//     while (queue.empty()) {
//         // MAGIC HAPPENS HERE
//         cv.wait(lock); 
//     }
    
//     // We only reach here if queue is NOT empty
//     Task t = queue.front();
// }
// What happens inside cv.wait(lock)? This is a complex atomic operation.
// Atomic Unlock: It releases the mutex automatically. (Crucial! If it slept holding the lock, the producer could never push a task because the queue would be locked forever. Deadlock.).
// Deschedule: It tells the OS Scheduler: "Remove me from the Runnable List." The OS puts the thread into a Blocked/Waiting Queue.
// Sleep: The thread consumes 0 CPU cycles. It is not running. It is effectively "frozen."
// What happens inside cv.notify_one() (The Producer)?
// The Main Thread pushes a task.
// It calls notify.
// The OS Scheduler moves one worker thread from the Waiting Queue back to the Runnable Queue.
// The Worker wakes up, automatically re-locks the mutex, and continues execution.


// ##### interconnection of conditional variable with mutex and task queue:

// Mutex (std::mutex): The Gate.
// It is the actual physical barrier. It ensures that only one thread can touch the shared data (the queue) at a time.

// Lock (std::unique_lock): The Key (or the act of holding the key).
// In C++, lock is a wrapper object that manages the Mutex. When you create the lock, you "acquire" the Mutex. When the lock is destroyed (or you call .unlock()), you release the Mutex.

// Condition Variable (cv): The Waiting Room / Intercom.
// It is a mechanism used strictly for waiting and signaling. It doesn't protect data; it just coordinates when threads should wake up.
// 2. The Timeline: What happens to "Thios" (The Thread)?
// Let's trace the life of the Worker Thread and the Mutex step-by-step.

// Phase 1: BEFORE cv.wait() (The Setup)
// Action: The Worker Thread enters the loop and creates std::unique_lock<std::mutex> lock(mutex);
// State: The Worker holds the Mutex.
// Consequence: No other thread (including the Main Thread) can touch the queue right now. The gate is closed.

// Phase 2: ENTERING cv.wait() (The Handover)
// Action: The Worker sees the queue is empty and calls cv.wait(lock);.
// Crucial Magic: Inside this function, the thread atomically releases the Mutex.
// Why? If it went to sleep holding the Mutex, the Main Thread would never be able to add anything to the queue (deadlock).
// State:
// The Worker is now Asleep (blocked).
// The Mutex is now Free (unlocked).

// Phase 3: THE NOTIFY (The Main Thread acts)
// Action: Since the Mutex is free, the Main Thread (Producer) acquires the Mutex.
// Action: It pushes a task into the queue.
// Action: It calls cv.notify_one().
// State:
// The Main Thread still holds the Mutex.
// The Worker Thread receives the signal and moves from "Deep Sleep" to "Ready to Run."
// CRITICAL: The Worker cannot proceed yet because the Main Thread is still holding the Mutex!

// Phase 4: THE WAKE UP (The Fight for the Lock)
// Action: The Main Thread finishes its work and releases the Mutex (unlocks).
// Action: Now that the Mutex is free, the Worker Thread (inside cv.wait) attempts to re-lock the Mutex.
// cv.wait() does not return until it successfully grabs the Mutex again.
// State:
// The Worker now holds the Mutex again.
// The function cv.wait() finally returns.

// Phase 5: RESUMING (The Check)
// Action: The code loops back to while (queue.empty()).
// Result: The queue is not empty. The loop finishes.
// Action: The Worker processes the task and eventually releases the Mutex.

// ##########################################################################################


// thread_pool.hpp - Lock-free-ish task queue with condition variables
#pragma once

#include "common.hpp"
#include "storage.hpp"
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic> //-->std::atomic forces the CPU to propagate the change immediately to all cores
#include <sys/socket.h>
#include <unistd.h>
//  --> sys/socket.h
// Gives you the socket() function to create an endpoint (get a File Descriptor).
// Gives you bind() to tell the OS "I own port 8080".
// Gives you listen() and accept() to handle connections.
// Gives you send() and recv() to push bytes onto the wire.

namespace flashkv {

class ThreadPool {
public:

    ThreadPool(size_t num_threads, StorageEngine& storage)
        : storage_(storage), stop_(false) {
        // -> This is Dependency Injection. You are binding the global StorageEngine (the "Database") to the ThreadPool.
        workers_.reserve(num_threads);
        
        for (size_t i = 0; i < num_threads; i++) {
            workers_.emplace_back([this, i] {
                workerLoop(i);
            });

            //empace back:it automatically creates a thread and adds it to the workers_ vector.
            // othersise we would have to:
            // You have to CREATE the thread first
            // std::thread t(workerLoop, 0); 

            // // Then you PUSH it into the vector (Move it)
            // workers_.push_back(std::move(t));
        }
        //The Problem: The new thread needs to touch queue_mutex_ and task_queue_. But those variables belong to the ThreadPool class instance.
        // The Packing: By putting this in the bracket [this], you are giving the thread a pointer to the ThreadPool object.
    }
    
    ~ThreadPool() {
        shutdown();
    }

    // Submit task to the pool
    // Returns false if queue is full (backpressure)
    bool submit(Task task) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            
            // Bounded queue - prevent memory exhaustion
            if (task_queue_.size() >= 10000) {
                return false;  // Queue full - client should retry
            }
            
            task_queue_.push(std::move(task));
        }
        
        // Wake up one waiting worker
        queue_cv_.notify_one();
        return true;
    }
    
    // Get queue depth for monitoring
    size_t queueDepth() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return task_queue_.size();
    }

    //gracefull shutdown
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        
        queue_cv_.notify_all();
        //why all rather than one:, we need every single thread to wake up, see that stop_ is true, and exit their loop.
        
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
            //join() blocks the main thread until the worker thread actually finishes its function execution.
            //Action: This tells the Main Thread: "PAUSE HERE. Do not move to the next line of code until this specific worker has finished."
        }
    }


    private:
    StorageEngine& storage_;
    std::vector<std::thread> workers_;

    // Task queue with synchronization
    std::queue<Task> task_queue_;
    mutable std::mutex queue_mutex_;
    //mutable because even in const functions we may need to lock the mutex to access the queue
    // so Allow this specific variable (queue_mutex_) to be modified even inside const functions.
    std::condition_variable queue_cv_;

    std::atomic<bool> stop_;
    //atomic because multiple threads will read/write this variable
    // it forces the CPU to propagate the change immediately to all cores ,so cant use mutex for same reason 
    //i.e Do not cache this variable permanently. Any time someone writes to it, force that change to be visible to all other cores immediately.


    void workerLoop(size_t worker_id) {
        while (true) {
            Task task;
            
            {
                std::unique_lock<std::mutex> lock(queue_mutex_); //It acquires the queue_mutex_
                
                // Wait for task or shutdown signal
                queue_cv_.wait(lock, [this] {
                    return stop_ || !task_queue_.empty();
                });
                //Go to sleep until there is work to do OR we are shutting down.
                //Wake Up: When notify_one() is called, the OS wakes the thread.
                // Re-Lock: The thread Re-locks the Mutex before returning from wait.
                // Re-Check: It checks the lambda again to be sure.
                
                if (stop_ && task_queue_.empty()) {
                    break;  // Exit if shutting down and no more tasks
                }
                
                if (!task_queue_.empty()) {
                    task = std::move(task_queue_.front());
                    task_queue_.pop();
                }
            }
            //{} ends:Since we don't hold the lock, other threads can push/pop from the queue while this thread is busy processing processTask.
            
            if (task.client_fd != -1) {
                processTask(task);
            }
        }
    }

    void processTask(const Task& task) {
        Response resp(false, "Unknown command");
        
        // Execute command on storage engine
        switch (task.cmd.type) {
            case CommandType::SET:
                resp = storage_.set(task.cmd.key, task.cmd.value);
                break;
                
            case CommandType::GET:
                resp = storage_.get(task.cmd.key);
                break;
                
            case CommandType::DEL:
                resp = storage_.del(task.cmd.key);
                break;
                
            case CommandType::UNKNOWN:
                resp = Response(false, "Invalid command");
                break;
        }
        
        // Send response back to client
        sendResponse(task.client_fd, resp);
    }
    void sendResponse(int client_fd, const Response& resp) {
        std::string resp_str = resp.toRESP();
        
        ssize_t sent = 0;
        ssize_t total = resp_str.size();
        const char* buf = resp_str.c_str();
        
        // Handle partial sends
        while (sent < total) {
            ssize_t n = ::send(client_fd, buf + sent, total - sent, 0);
            if (n <= 0) {
                // Client disconnected or error
                break;
            }
            sent += n;
            // We call: send(fd, buf + 0, 11 - 0, ...) -> "Try to send 11 bytes starting from index 0".
            // Network: "I can only take 5."
            // Returns: n = 5.
            // We update: sent += 5. (Now sent is 5).
        }
    }

};

}// namespace flashkv