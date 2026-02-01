// main.cpp - Server entry point with monitoring
#include "common.hpp"
#include "storage.hpp"
#include "thread_pool.hpp"
#include "network.hpp"
#include <iostream>
#include <csignal>
#include <atomic>

using namespace flashkv;

// Global state for signal handling
std::atomic<bool> g_shutdown{false};
NetworkServer* g_server = nullptr;

void signalHandler(int signum) {
    std::cout << "\n[SIGNAL] Received signal " << signum << ", shutting down...\n";
    g_shutdown = true;
    
    if (g_server) {
        g_server->stop();
    }
}



void printStats(const StorageEngine& storage, const ThreadPool& pool) {
    auto stats = storage.getStats();
    
    std::cout << "\n=== FlashKV Stats ===\n";
    std::cout << "Total Keys:        " << stats.total_keys << "\n";
    std::cout << "Max Shard Size:    " << stats.max_shard_size << "\n";
    std::cout << "Avg Shard Size:    " << stats.avg_shard_size << "\n";
    std::cout << "Load Imbalance:    " << stats.load_imbalance << "x\n";
    std::cout << "Thread Pool Queue: " << pool.queueDepth() << "\n";
    std::cout << "====================\n\n";
}

int main(int argc, char* argv[]) {
    std::cout << R"(
    ███████╗██╗      █████╗ ███████╗██╗  ██╗██╗  ██╗██╗   ██╗
    ██╔════╝██║     ██╔══██╗██╔════╝██║  ██║██║ ██╔╝██║   ██║
    █████╗  ██║     ███████║███████╗███████║█████╔╝ ██║   ██║
    ██╔══╝  ██║     ██╔══██║╚════██║██╔══██║██╔═██╗ ╚██╗ ██╔╝
    ██║     ███████╗██║  ██║███████║██║  ██║██║  ██╗ ╚████╔╝ 
    ╚═╝     ╚══════╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝  ╚═══╝  
    
    High-Performance In-Memory Key-Value Store
    Architecture: kqueue Reactor + Sharded Storage
    )" << "\n";
    
    // Parse command line
    uint16_t port = 7379;  // Default port (Redis-like)
    std::string aof_path = "flashkv.aof";
    size_t num_threads = THREAD_POOL_SIZE;
    
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    
    std::cout << "[CONFIG] Port:          " << port << "\n";
    std::cout << "[CONFIG] Thread Pool:   " << num_threads << " workers\n";
    std::cout << "[CONFIG] Shards:        " << NUM_SHARDS << "\n";
    std::cout << "[CONFIG] AOF Path:      " << aof_path << "\n";
    std::cout << "[CONFIG] Cache Line:    " << CACHE_LINE_SIZE << " bytes\n";
    std::cout << "\n";
    
    // Install signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    //telling the OS: "When you see Ctrl+C, don't kill me. Run this function instead."
    
    try {
        // Initialize components
        StorageEngine storage(aof_path);
        ThreadPool pool(num_threads, storage);
        NetworkServer server(port, pool);
        
        g_server = &server;
        
        // Print initial stats
        printStats(storage, pool);
        
        // Monitoring thread (prints stats every 10s)
        std::thread monitor_thread([&]() {
            while (!g_shutdown) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                if (!g_shutdown) {
                    printStats(storage, pool);
                }
            }
        });
        
        // Run server (blocks until shutdown)
        server.run();
        
        // Cleanup
        std::cout << "[SHUTDOWN] Stopping server...\n";
        monitor_thread.join();
        
        std::cout << "[SHUTDOWN] Final stats:\n";
        printStats(storage, pool);
        
        std::cout << "[SHUTDOWN] Complete. Goodbye!\n";
        
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}