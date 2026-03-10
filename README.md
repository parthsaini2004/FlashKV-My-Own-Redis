# FlashKV: In-Memory Key-Value Store

High-performance Redis-like key-value store built in C++17. Achieves 50,000+ ops/sec with 10,000 concurrent connections using event-driven networking and lock-striped concurrency.

## What It Is

FlashKV is a complete in-memory database that stores key-value pairs in RAM with disk persistence. Built from scratch in C++, it demonstrates production-grade systems programming: event-driven I/O, concurrent data structures, and optimized networking. Understanding how systems like Redis work internally teaches core concepts (networking, concurrency, memory optimization) essential for backend engineers.

## Why It Matters

Most applications need fast data storage. A naive implementation would either:
- Serialize all requests (slow: 1000 ops/sec)
- Use one thread per client (memory: 10GB for 10K connections)

FlashKV solves both through proven patterns used in production systems (Redis, memcached, databases).

## Features

- Event-driven networking with kqueue (10,000+ connections on single thread)
- Lock-striped storage (32 independent shards) eliminates global lock bottleneck
- Reader-writer locks (multiple readers, exclusive writers)
- Append-Only File persistence (durability on every write)
- Thread pool with 8 workers and bounded queue (backpressure, prevents memory explosion)
- Sub-millisecond latency for reads (0.1-0.5ms)

## Architecture

Request Flow:
TCP Client -> kqueue (event loop) -> RESP Parser -> Task Queue -> Thread Pool -> Storage Engine -> AOF Log

1. Network Layer (Reactor Pattern with kqueue)
   - Single thread monitors all connections
   - Non-blocking I/O on all sockets
   - Wakes only when data available
   - Batch processes up to 64 events per call

2. Thread Pool (Producer-Consumer Pattern)
   - 8 worker threads process tasks
   - Bounded queue (10K max items) for backpressure
   - Main thread produces, workers consume
   - Condition variables for efficient signaling

3. Storage Engine (Lock Striping)
   - 32 shards, each with hash map and reader-writer lock
   - hash(key) % 32 determines shard
   - Multiple readers OR one exclusive writer per shard
   - Cache-aligned (64-byte boundaries) prevents false sharing

4. Persistence (AOF Logger)
   - Every write logged to disk immediately
   - fsync() ensures durability
   - Recovery on startup rebuilds state

5. Protocol (RESP Parser)
   - Redis Serialization Protocol compatible
   - Handles partial data from network delays
   - Streaming parser with position tracking

## Core Concepts

Reactor Pattern
What: Single thread monitors all connections via event multiplexer instead of thread-per-connection
Why: 10K threads = 10GB memory, constant context switching overhead
How: kqueue returns only active connections, main thread dispatches to worker pool
Benefit: Scalable to 10K+ connections, minimal memory, high throughput

Lock Striping
What: Database split into 32 independent shards, each with own lock
Why: Single lock serializes all operations, throughput capped at 1K ops/sec
How: hash(key) % 32 determines shard, different shards accessed in parallel
Benefit: 50K+ ops/sec on multi-core, true parallelism
Trade-off: 3% collision rate (two keys same shard) vs 100% contention

Cache Alignment
What: Each shard padded to 64 bytes (CPU cache line) using alignas(CACHE_LINE_SIZE)
Why: Two shards in same cache line cause "false sharing" - cache coherency traffic when modified
How: Alignment ensures each shard occupies separate cache line
Benefit: Eliminates hidden contention between cores

Reader-Writer Locks
What: Multiple threads read simultaneously, but only one writes
Why: Most workloads are read-heavy, can leverage parallelism
How: GET uses shared_lock (many concurrent), SET/DEL use unique_lock (exclusive)
Benefit: Saturates CPU cores during read workloads

Backpressure
What: Task queue limited to 10K items
Why: Prevents memory explosion under extreme load
How: When queue full, reject new requests with "server overloaded"
Benefit: Fail fast, let load balancer retry, maintain stable latency

## How It Works: Request Journey

Client sends "SET user:100 Alice":

1. Kernel receives TCP bytes, stores in socket buffer
2. kqueue wakes main thread: "Connection 5 has data"
3. Main thread reads 4KB into per-connection buffer
4. RESP parser converts bytes to Command: {SET, "user:100", "Alice"}
5. Command wrapped in Task, pushed to task queue
6. Condition variable wakes idle worker thread
7. Worker computes: hash("user:100") % 32 = 17
8. Worker acquires unique_lock on shard[17].mutex
9. Updates hash map: shards[17].data["user:100"] = "Alice"
10. Releases lock immediately (minimizes critical section)
11. Logs to AOF: "SET user:100 Alice\n" then fsync()
12. Serializes response to RESP: "+OK\r\n"
13. Sends via send() syscall
14. Worker loops back to wait for next task

Meanwhile, other workers process other clients in parallel.

## Setup

Prerequisites: macOS, CMake 3.15+, C++17 compiler

Build the project:

    cd FlashKV-My-Own-Redis
    rm -rf build
    mkdir build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make

Start the server in one terminal:

    ./flashkv

The server starts on port 7379 and prints configuration and statistics.

Running Tests

Open a new terminal and run the automated test suite:

    cd FlashKV-My-Own-Redis
    cd tests
    python3 test_client.py

This runs correctness tests and performance benchmarks showing throughput and latency.

Manual Testing

For manual testing with clean output, run the test script:

    cd FlashKV-My-Own-Redis
    cd tests
    python3 manual_test.py



## Operations

SET key value - Store/update in shard. Logged to AOF. Latency: 1-2ms
GET key - Retrieve value. Parallel on same shard. Latency: 0.1-0.5ms
DEL key - Remove key. Logged to AOF. Latency: 1-2ms

## Performance

Throughput: 15K (single) -> 85K (10 clients) -> 120K (50 clients) ops/sec
Latency: p50=0.5ms, p99=2.0ms, max=5ms
Memory: 8KB per connection, ~200 bytes per key, 100K keys = 20-30MB
Bottleneck: AOF fsync limits writes to ~1000/sec

## Code Structure

include/
- common.hpp: Constants, Command/Task/Response structs
- network.hpp: NetworkServer, kqueue event loop
- storage.hpp: StorageEngine, Shard struct, lock-striped hash map
- thread_pool.hpp: ThreadPool, worker threads, task queue
- resp_parser.hpp: RESPParser, protocol parsing

src/main.cpp: Initialization, signal handling, monitoring

tests/test_client.py: Python client, tests, benchmarks
