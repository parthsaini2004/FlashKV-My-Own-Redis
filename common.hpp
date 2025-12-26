
#include <string>

// we define the paramters here that are reqd globally (accessed everywhere)

#pragma once 
// ==> ensure that a specific header file is included only once during a single compilation,
//     even if it is referenced multiple times in your project.


namespace flashkv {

// #namespace flashkv 
// namespace flashkv: used for Logical Isolation and Symbol Conflict Prevention.
// By wrapping your code in namespace flashkv, your function becomes flashkv::init(), making it unique.

// #constexpr
// constexpr stands for constant expression.
// This value (or the result of this function) can be calculated at compile-time rather than while the program is running.


// 1)

constexpr size_t NUM_SHARDS =32;
// NUM_SHARDS==> no of shards we are breaking our in memory storage into 
// to prevent threads from fighting over a single "lock"
// Logic: When data comes in, the program calculates a hash. Instead of hash % 32,it uses hash & (32 - 1).
// Benefit: Computers are much faster at bitwise AND (&) than division (%). Using a power of 2 allows this shortcut.
// it gives a value between 0 and 31, which corresponds to one of the 32 shards.
//we need to keep the number of shards as a power of 2 to use this optimization.

//2)
constexpr size_t CACHE_LINE_SIZE = 64; 
// it says that two variable will not use the same line/space/cache(for each thread/core_used a cache is created) ,
// rather will create a 64 byte cache seperately for them 
//otherwise if not done, two variables may end up on the same cache/line and work as single core 

// 3)
constexpr size_t THREAD_POOL_SIZE = 8;
// determines the fixed number of execution units (threads) allocated to a process for task processing.
// If Tasks < Pool Size: All tasks are executed immediately in parallel. across the 8 cores
// If Tasks > Pool Size: Excess tasks are queued and executed as threads become available.

// 4)
constexpr size_t MAX_CONNECTIONS = 10000;
// a connection is a persistent TCP session handle.Technically, 
// it is an established communication channel between two unique IP:Port pairs
// that stays open until one side explicitly closes it.
// specifying how many of the following "pipes" can exist simultaneously:ie 10000
// otherwise new connection requests will be refused or ignored until existing connections are closed. 

// 5)
constexpr size_t BUFFER_SIZE = 4096;
// A Buffer is a contiguous block of memory (usually an array of char or uint8_t)
// where data is placed after being read from a socket but before being parsed by your KV logic.
// 4096 bytes is the size of one Virtual Memory Page.
// If your buffer was 4097 bytes, every time you cleared the buffer, the CPU might have to touch two different memory pages instead of one.
//  This causes a tiny performance hit that adds up across millions of operations.


// #############################......... The PROCESS ......#######################################################

// 1. The Kernel (The Arrival)
// Data arrives from the network and sits in the Kernel Socket Buffer. It stays there. The kernel will not move it into your program's memory until you explicitly ask.

// 2. The Notification (The Alarm)
// The Kqueue (on your Mac) or Epoll (on Linux) notices that data is sitting in the Kernel. It sends a "Ready" signal to your program.

// 3. The Thread (The Fetcher)
// A Thread from your pool wakes up. This is the moment of action. The Thread performs a System Call (like read() or recv()).

// The Thread is the one that actually "pulls" the data.

// It tells the Kernel: "Take the data for Connection #10 and put it into Buffer #10."

// 4. The Buffer (The Storage)
// Only now does the data enter your Buffer. The Kernel copies the bytes from its hidden memory into your 4096-byte array.

// 5. The Processing (The Brain)
// Now that the data is in the Buffer, the Thread doesn't have to talk to the Kernel anymore. It stays in "User Space" and reads the buffer line-by-line to find your SET or GET commands.\

// ##
// why not use directly use connection kernel storage but buffer :
// If you read data "directly" (one byte at a time), the CPU has to stop your code and switch to "Kernel Mode" for every single byte.
// With a Buffer: You do one switch, grab 4096 bytes (a "gulp"), and then your Thread works at 100% speed in its own memory.
// By copying it to a Buffer, the Thread can look at the same data multiple times (to calculate a hash, then to check a key, then to log the request) without losing it.

// The Connection is the Pipe, the Kernel is the Reservoir, the Buffer is the Bucket, and the Thread is the Worker. 
// You can't drink directly from a high-pressure reservoir; 
// you need the bucket to stabilize the water so you can use it.

// #############################......... The PROCESS ......#######################################################

// Command types for the KV store
// Scope (The "Namespacing" problem): If you just had enum Color { RED };
// you couldn't use the word RED anywhere else in your program. By using enum class CommandType, 
// the name SET is "hidden" inside CommandType. You must type CommandType::SET to use it.
enum class CommandType {
    //enum class (also called a strongly typed enum) serves as a way to create a fixed set of named constants that are safe, organized, and efficient.
    SET,
    GET,
    DEL,
    UNKNOWN
    //An enum is an "OR" type (Sum Type): It represents a choice between mutually exclusive options.
    //Example: A CommandType is either a SET OR a GET OR a DEL. It can never be two at the same time.

};

// Represents a parsed client command
struct Command {
    CommandType type;
    std::string key;
    std::string value;  // Empty for GET/DEL
    
    Command() : type(CommandType::UNKNOWN) {}
    Command(CommandType t, std::string k, std::string v = "")
        : type(t), key(std::move(k)), value(std::move(v)) {}
};

struct Response{
    bool success;
    std::string data;

    Response(bool s, std::string d="") : success(s), data(std::move(d)) {}

    std:: string toRESP() const{
        if (!success) {
            return "-ERR " + data + "\r\n";
        }
        if (data.empty()) {
            return "$-1\r\n";  // Null bulk string
        }
        return "$" + std::to_string(data.size()) + "\r\n" + data + "\r\n";
        //use \r\n as the official way to signal the end of a line.
    }
};

struct Task{

    int client_fd; // client file descriptor
    Command cmd;
    Task(): client_fd(-1), cmd() {};
    Task(int fd, Command c) : client_fd(fd), cmd(std::move(c)) {}

};

// High-precision timing for latency measurements
using TimePoint = std::chrono::steady_clock::time_point;
//steady_clock:It is the only reliable way to measure Duration (how long something took),dosent change/adjust
// It allows you to measure exactly how long it took to process a command from the moment it arrived until the moment the response was sent.


inline TimePoint now() {
    return std::chrono::steady_clock::now();
    //Instead of typing the long-winded std::chrono::steady_clock::now() 
    //every time you want to check the time, you can now just type now().
    //not human readable ,only diiference bw strat and end is useful
}

inline int64_t elapsed_micros(TimePoint start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        now() - start).count();
}



}