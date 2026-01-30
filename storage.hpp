//Sharded HashMap with cache-line alignment


//##########################################################################################################

//# using shards to reduce lock contention in multi-threaded environments
//so instead of one big hashmap we have multiple smaller hashmaps (shards) each with its own lock

// ##### alignment ==> alignas(64) 
// we use CACHE_LINE_SIZE to ensure that each shard starts on a new cache line
//this prevents false sharing where multiple threads modify variables that reside on the same cache line
//false sharing can lead to performance degradation due to cache coherence traffic
// CPUs can write in parallel without cache traffic

// ##### simulation for the same 
// Thread A receives: SET user:101 "Parth". ==> we find which shard it belongs to (lets say shard 17)
// Thread B receives: GET user:101 ==> we find which shard it belongs to(lets say shard 17)
// i.e hash(101) ==> 982,451,217 ==>982,451,217 % 32 ==17 

// Time(ns)    ,Thread A (Writer)                  ,Thread B (Reader),                     Shard 17 State
// T=0,        "Hash(""user:101"") -> Shard 17",   "Hash(""user:101"") -> Shard 17",       Free
// T=10,       Acquire Write Lock (Success),       Acquire Read Lock (Wait),               LOCKED (Exclusive)
// T=20,       "Write ""Parth"" to HashMap",       Sleeping...,                            LOCKED (Exclusive)
// T=30,       Release Write Lock,                 Sleeping...,                            Free
// T=31,       Start writing to AOF (Disk),        Acquire Read Lock (Success),            LOCKED (Shared)
// T=40,       ...Writing to disk...,              "Read ""Parth"" from HashMap",          LOCKED (Shared)
// T=50,       ...Writing to disk...,              Release Read Lock,                      Free
// T=1000,     Finish Disk Write,                  (Request Completed long ago),           Free

// Granularity:  didn't lock the whole database, only 1/32th of it.
// Latency Hiding:  let readers read before the slow disk write finished.
// Hardware Sympathy:  aligned memory to match the CPU cache lines.

// ##### latency hiding explained:
// RAM Write: ~50 nanoseconds (FAST)
// Disk Write (fsync): ~1,000,000 nanoseconds = 1 millisecond (SLOW)
// By letting readers access the shard while the disk write is happening, we hide the latency of the slow operation.
//otherwise if we locked the shard for the entire duration of the disk write, readers would have to wait much longer.(for writing in disk to finish)

// ##### Consistency vs. Latency trade-off
// ==>If Thread A updates RAM, releases the lock, and then the server crashes BEFORE the disk write finishes data lost.
// Latency Optimization: In high-performance KV stores, the priority is minimizing response times. Blocking readers for ~1ms during disk I/O is avoided to prevent massive performance degradation.
// Reliability Model: Data loss risk is limited. Since the OS flushes buffers during process crashes, data is only vulnerable during a total hardware power failure.
// Durability Flexibility: While FlashKV defaults to asynchronous logging for speed, the design allows for a sync_on_write flag to enforce strict durability at the cost of latency if the use case requires it.


// ##### locking mechanism used:
//simple mutex and no shards ==>global lock for entire hashmap ==> only 1 read or write at a time
//shared mutex and no shards ==> multiple reads or 1 write at a time for entire hashmap

//shared mutex with shards i.e erader /writer lock ==> multiple reads or 1 write at a time per shard ==> more concurrency
//i.e A Reader-Writer lock differentiates between access types to maximize parallel execution.
// Shared Access (The "Reader"): Multiple threads can acquire the lock simultaneously as long as they are only reading. This allows the system to utilize all CPU cores for GET requests.
// Exclusive Access (The "Writer"): When a SET or DEL request arrives, the lock ensures the writer has total isolation. It waits for current readers to finish and blocks new readers from starting until the update is complete.


// ##### the request handling and locking part:

// ###> When multiple READERS (GET) arrive:
// Everyone "locks" it, but they share the key.Thread A, B, and C all call lock_shared().
// 1 The mutex maintains an internal counter.
// 2 It says: "Currently, there are 3 readers. Anyone else who wants to read can come in. Anyone who wants to write must wait for this counter to hit 0."
// Result: All 3 threads are inside the critical section simultaneously. 

// ###> When one WRITER (SET/DEL) arrives:
// Only the writer locks it; everyone else is locked out.
// Thread D calls lock().The mutex checks its counter. If the counter is >0 (readers are inside) or
// if another writer is inside, Thread D is blocked.Once it's Thread D's turn, it takes exclusive ownership.
// Result: Thread D is the only thread in the critical section. The "Shared" entry is closed.


// ##### critical scenarios explained:
// Same Key, Different Threads (Race Condition): Since both threads target the same shard, the lock forces them to run one after another (serializing), ensuring the "Last Writer Wins" and data stays consistent.
// Different Keys, Same Shard (Collision): Even though keys are different, they map to the same bucket (Shard ID), so they must wait for the same lock, causing "false contention."
// Different Keys, Different Shards (Ideal Parallelism): The threads target completely independent memory addresses (locks), allowing the CPU to execute them simultaneously with zero waiting.
// AOF Corruption (Crash Recovery): If a power failure leaves a half-written log line, the recovery process detects the mismatch (via checksum or length check) and safely discards the corrupted tail.

// Why not have 1 lock per key? Too much memory overhead (millions of mutexes).
// Why 32 shards? It lowers the probability of this collision happening. With 32 shards and random keys, there is only a ~3% chance two random requests will collide.

//##### :False Contention :It happens when two threads want completely different data, but are forced to share the same lock because of how we grouped the data.
//##### :Lock Striping : is a concurrency pattern where a single data structure is partitioned into several independent sections (stripes), each protected by its own lock.
// This reduces lock contention by decreasing the probability that two threads will require the same lock simultaneously."


//##### Concurrency:  have 1000 clients connected.  Thread Pool manages these 1000 connections using just 4 threads. It switches context between them so no client feels ignored.
//##### Parallelism: Because  used Lock Striping (storage.hpp), when Thread 1 runs on Core A and Thread 2 runs on Core B, they don't block each other. They run in parallel.


//##### lock contention: and how using sharding + alignment we decreased it
// Lock Contention is what happens when multiple CPU cores (threads) try to grab the same lock at the same time.
// sharding: You have effectively cut the "chance" of a traffic jam by 32x. by sharding
// alignment :
// When Core 1 locks and updates Shard 0, the CPU hardware thinks the entire 64-byte line is "dirty." It forces Core 2 (which is working on Shard 1) to throw away its cache and reload from RAM.
// The Result: Even though your code says they are different shards, the hardware makes them wait for each other. This is "Hidden Contention."

//##########################################################################################################


// storage.hpp - Lock-striped, cache-aligned storage engine
#pragma once

#include "common.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <fstream>
#include <functional>
#include <sstream>


namespace flashkv {

    // Hash function for key distribution
    inline size_t hash_key(const std::string& key) {
        return std::hash<std::string>{}(key);
    }
    //==> turns a userd_id into a fixed-size integer.
    // inline tells the compiler to take the function's body and "paste" it into every place where that function is called.

    // Single shard of the hash map
    // CRITICAL: alignas(64) prevents false sharing between CPU cores 
    // lets say shard 0 updates cache0 , shard 1 updates cache1 and they are on same cache line ,
    // then when shard 0 updates cache0 ,cpu thinks shard 1 may also need to update cache1 (even though it doesn't)
    // so it invalidates cache1 from other cores and this causes performance degradation due to cache coherence traffic
    // by aligning each shard to start at a new cache line , we prevent this issue


    struct alignas(CACHE_LINE_SIZE) Shard {
        std::unordered_map<std::string, std::string> data;
        mutable std::shared_mutex mutex;  // Reader-writer lock
        
        Shard() = default;
        
        // Prevent copying (mutexes are not copyable)
        Shard(const Shard&) = delete;
        Shard& operator=(const Shard&) = delete;
        // The Logic: You cannot "copy" a Shard. If you tried to copy it, you’d have two different maps protected by two different locks, which would lead to data corruption.
        // The Action: By "deleting" the copy constructor, if you accidentally try to write Shard s2 = s1;, the compiler will stop you immediately with an error.
        
        // Allow moving
        Shard(Shard&& other) noexcept {
            std::unique_lock lock(other.mutex);
            data = std::move(other.data);
        }
        // we might want to move it (e.g., when initializing the system).
        // This "Move Constructor" takes the data from a temporary Shard (other), steals its contents using std::move, and leaves the old one empty.
    };

    // Client -> Network -> Server -> Task Queue -> Worker Thread -> StorageEngine



    class  StorageEngine {
        //storage engine handles data storage and retrieval
        // It is the only part of the code that has access to the actual data (the Shards).
        // The Server doesn't know how to write to the flashkv.aof file. It calls the StorageEngine to ensure the data is saved to the disk.


        //When the program starts, the StorageEngine constructor reads that file and rebuilds the database so no data is lost.
        // Technically, it is an Array of Shards.
        // Inside each shard is a std::unordered_map<std::string, std::string>.

        public:
        StorageEngine(const std::string& aof_path = "flashkv.aof") 
        : aof_path_(aof_path) {
        
            // Open AOF in append mode
            aof_stream_.open(aof_path_, std::ios::app | std::ios::binary);
            if (!aof_stream_.is_open()) {
                throw std::runtime_error("Failed to open AOF file: " + aof_path_);
            }
            
            // Recover from AOF if exists
            recoverFromAOF();
        }
        ~StorageEngine() {
            if (aof_stream_.is_open()) {
                aof_stream_.flush();
                aof_stream_.close();
            }

        };

        // GET operation - uses shared_lock for concurrent reads
        Response get(const std::string& key) {
            size_t shard_idx = hash_key(key) % NUM_SHARDS;
            Shard& shard = shards_[shard_idx];

            //shard.mutex : Lives inside the Shard:
            // variable stored in RAM that acts as a counter and a flag. 
            // It keeps track of how many threads are currently "reading" and whether anyone is "writing."

            // shared_lock: 
            // Lives inside the function. It is the Permission. It is temporary.
            // If no one is writing, the shared_lock tells the mutex to increment its internal "Reader Counter."

            // Multiple readers can hold this lock simultaneously

            std::shared_lock<std::shared_mutex> lock(shard.mutex);
            
            auto it = shard.data.find(key);
            if (it != shard.data.end()) {
                return Response(true, it->second);
            }
            return Response(false, "Key not found");
            // LOCK RELEASES HERE (at the end of the function)
        }

        // SET operation - uses unique_lock for exclusive write access
        Response set(const std::string& key, const std::string& value) {
            size_t shard_idx = hash_key(key) % NUM_SHARDS;
            Shard& shard = shards_[shard_idx];
            
            {   // <--- Scope Starts

                // Write lock - blocks all readers and writers
                std::unique_lock<std::shared_mutex> lock(shard.mutex);
                shard.data[key] = value;

                // <--- Scope Ends (LOCK RELEASES RIGHT HERE!)
            }
            
            // Log to AOF AFTER releasing lock (reduce critical section)
            logAOF("SET", key, value);
            
            return Response(true, "OK");
        }

        // ###################################################################################################

        // The Scenario
        // Request 1 & 2: GET user:1 (Read)
        // Request 3: SET user:1 "Alice" (Write)
        // Request 4: GET user:1 (Read)

        // Phase 1: High Concurrency (Requests 1 & 2)
        // Requests 1 and 2 hit the get() function at the exact same millisecond.
        // Both execute std::shared_lock lock(shard.mutex);.
        // The Shared Mutex sees two "Reader" requests. Since no one is writing, it allows both to proceed.
        // Both threads search the unordered_map at the same time on different CPU cores.
        // Result: Total parallel execution. No waiting.

        // Phase 2: The Block (Request 3 arrives)
        // While 1 and 2 are still finishing, Request 3 (SET) hits the code.
        // It reaches std::unique_lock lock(shard.mutex);.
        // The Mutex sees that Readers (1 and 2) are still active.
        // Request 3 must STOP and wait. It cannot enter the { } block until the Reader Count hits zero.
        // Once Requests 1 and 2 finish their functions and their shared_lock variables "die," the count hits zero.

        // Phase 3: The Critical Section (Request 3 executes)
        // Request 3 now enters the { } block and grabs Exclusive Access.
        // While Request 3 is inside those braces, Request 4 (GET) arrives.
        // Request 4 tries to grab a shared_lock, but the Mutex is now in "Unique Mode." Request 4 must STOP and wait.
        // Request 3 updates the memory: shard.data["user:1"] = "Alice".
        // The Release: Request 3 hits the closing brace }. Its unique_lock is destroyed immediately.

        // Phase 4: Early Release & Parallelism (The "Pro" Move)
        // This is where your code's specific design shines:
        // The moment Request 3 leaves the { } block, the lock is gone.
        // Request 4 can now immediately finish its GET and see the new value ("Alice").
        // Meanwhile, Request 3 is still running! It is now executing logAOF(...).
        // Because logAOF is outside the braces, it is writing to the disk while other threads are already back to reading and writing in memory.

        // ###################################################################################################

        // DEL operation
        Response del(const std::string& key) {
            size_t shard_idx = hash_key(key) % NUM_SHARDS;
            Shard& shard = shards_[shard_idx];
            
            bool found = false;
            {
                std::unique_lock<std::shared_mutex> lock(shard.mutex);
                found = shard.data.erase(key) > 0;
            }
            
            if (found) {
                logAOF("DEL", key, "");
                return Response(true, "1");
            }
            return Response(true, "0");
        }

        // Stats for monitoring
        struct Stats {
            size_t total_keys;
            size_t max_shard_size;
            double avg_shard_size;
            double load_imbalance;  // max/avg ratio
        };

        Stats getStats() const {
            Stats stats{};
            size_t max_size = 0;
            size_t total = 0;
            
            for (const auto& shard : shards_) {
                std::shared_lock<std::shared_mutex> lock(shard.mutex);
                size_t size = shard.data.size();
                total += size;
                max_size = std::max(max_size, size);
            }
            
            stats.total_keys = total;
            stats.max_shard_size = max_size;
            stats.avg_shard_size = static_cast<double>(total) / NUM_SHARDS;
            stats.load_imbalance = stats.avg_shard_size > 0 
                ? max_size / stats.avg_shard_size 
                : 0.0;
            
            return stats;
        }


        private:

        Shard shards_[NUM_SHARDS];

        // AOF persistence
        std::string aof_path_;
        std::ofstream aof_stream_;
        std::mutex aof_mutex_;  // Separate lock for file I/O


        void logAOF(const std::string& cmd, const std::string& key, const std::string& value) {
                std::lock_guard<std::mutex> lock(aof_mutex_);
                
                // Simple text format: CMD key value\n
                aof_stream_ << cmd << " " << key;
                if (!value.empty()) {
                    aof_stream_ << " " << value;
                }
                aof_stream_ << "\n";
                aof_stream_.flush();  // Ensure durability

                // The Secret: When you write data in C++, it doesn't actually go to the disk immediately. The Operating System (OS) puts it in a "Buffer" (a temporary waiting area in RAM) to save energy.
                // The Problem: If the computer crashes while the data is still in the "Buffer," you lose that data, even though your code said "write."
                // The Fix: .flush() forces the OS to empty that buffer and push the data onto the physical disk right now. This is what makes your database Durable (The 'D' in ACID).
        }

        void recoverFromAOF() {
            //recoverFromAOF is the database's "Memory Reconstruction" phase.
            std::ifstream recovery_stream(aof_path_);
            if (!recovery_stream.is_open()) return;
            
            std::string line;
            size_t count = 0;
            
            while (std::getline(recovery_stream, line)) {
                //It reads the file line-by-line. Each line represents one command that was successfully executed in the past (e.g., SET name Parth).
                
                std::istringstream iss(line);
                std::string cmd, key, value;
                
                
                iss >> cmd >> key;
                // If the line is SET user_1 admin, it puts "SET" into cmd and "user_1" into key.

                if (cmd == "SET") {
                    std::getline(iss, value);
                    if (!value.empty() && value[0] == ' ') {
                        value = value.substr(1);
                    }
                    set(key, value);
                    count++;
                } else if (cmd == "DEL") {
                    del(key);
                    count++;
                }
            }
            
            // Clear AOF stream position after recovery
            aof_stream_.clear();
    }


    };
}
// namespace flashkv
