//Handle 10,000+ concurrent TCP connections on one thread.

//##########################################################################################################

// ##### reading approach for socket
// if we want to read from sockets :
// ###> bad approach: ask each socket if it has data (polling) ==> wastes CPU cycles
// WHILE true:
//       data = read(socket1)  ← Blocks if no data
//       process(data)
//       data = read(socket2)  ← Blocks if no data
//       process(data)

// If socket1 has no data, but socket2 has urgent data, socket2 must wait until socket1 receives a byte or times out. This is Head-of-Line Blocking.
// Scalability: To handle 10,000 clients this way, you would need 10,000 threads (One-Thread-Per-Client).

// ###> good approach: Use an event-driven model with non-blocking I/O and multiplexing (e.g., epoll, kqueue, IOCP).
// WHILE true:
//       events = kqueue_wait()  ← Returns when ANY socket has data
//       FOR each event:
//          IF event is "data ready":
//             data = read(socket)  ← Returns immediately
//             process(data)
   
//    Can handle 10,000 clients simultaneously!

// Inversion of Control: Instead of asking every socket "Do you have data?", you register 10,000 sockets with the Kernel once.
// You tell the Kernel: "Wake me up only when one of these specific sockets is ready."

// Old methods (select, poll) required iterating over the entire list of 10,000 sockets ($O(N)$) to find the active ones.
// kqueue and epoll return a distinct list of only the events that happened. If 5 clients send data, the loop runs exactly 5 times.
// Complexity is $O(1)$ relative to total connections.

// Wake Up: The OS wakes your thread up only when data actually arrives. It hands you a list: "Connection #5 sent a 'GET' command, and Connection #99 sent a 'SET' command."
// Action: You loop through just those specific connections, read their data, and process it.
// Repeat: Go back to sleep.

// ##### mutlipexing:  is the tool that allows us to monitor multiple file descriptors to see if they have data 
// ##### reactor pattern: is the design pattern that uses multiplexing to handle multiple connections in single thread 
//     it helps  inversion of control by letting the OS notify us when data is ready instead of us asking each socket
// ##### kqueue : The specific Operating System function (API) that implements Multiplexing on macOS and FreeBSD.






//##########################################################################################################


#pragma once

#include "common.hpp"
#include "resp_parser.hpp"
#include "thread_pool.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <unordered_map>
#include <memory>
#include <iostream>

namespace flashkv {

// Per-connection state
struct Connection {
    int fd;
    std::string read_buffer;
    size_t parse_pos;
    
    Connection(int f) : fd(f), parse_pos(0) {
        read_buffer.reserve(BUFFER_SIZE);
    }
};

class NetworkServer {

public:

      NetworkServer(uint16_t port, ThreadPool& pool) 
            : port_(port), pool_(pool), running_(false) {
            
            // Create kqueue descriptor
            kqueue_fd_ = kqueue();
            //The kernel allocates a new kqueue data structure in kernel memory
            // The kernel returns a file descriptor (integer index) pointing to this structure.

            if (kqueue_fd_ == -1) {
                  throw std::runtime_error("kqueue() failed: " + 
                  std::string(strerror(errno)));
            }
            
            // Create listening socket
            listen_fd_ = createListenSocket(port);
            
            // Register listen socket with kqueue
            registerEvent(listen_fd_, EVFILT_READ, EV_ADD);
      }

      ~NetworkServer() {
            stop();
            
            if (listen_fd_ != -1) {
                  close(listen_fd_);
            }
            
            if (kqueue_fd_ != -1) {
                  close(kqueue_fd_);
            }
            
            // Close all client connections
            for (auto& [fd, conn] : connections_) {
                  close(fd);
            }
      }

      void run() {
        running_ = true;
        
        std::cout << "[FlashKV] Server listening on port " << port_ << "\n";
        std::cout << "[FlashKV] kqueue FD: " << kqueue_fd_ << "\n";
        std::cout << "[FlashKV] Waiting for connections...\n";
        
        struct kevent events[64];  // Batch event processing
        //we ask for up to 64 events at once. This reduces the number of expensive System Calls
        // (switching between User Mode and Kernel Mode).

        //The Problem: The OS knows that Socket #5 has a "GET" command waiting. But your C++ code (User Space) cannot read the OS's internal memory variables. It is forbidden.
        //The Solution (events array): You create this array in your memory (events[64]). When you call kevent(), you pass the address of this array to the OS.
        //The Action: The OS copies the details ("Socket #5 has data") into your events array.
        
        while (running_) {
            // Wait for events (timeout: 1 second)
            struct timespec timeout{1, 0};
            int nev = kevent(kqueue_fd_, nullptr, 0, events, 64, &timeout);
            //If nev is 5, it means the first 5 slots in your events array now contain real data Slots 5-63 are still garbage/empty.
            
            if (nev < 0) {
                if (errno == EINTR) continue;  // Interrupted by signal
                std::cerr << "[ERROR] kevent() failed: " << strerror(errno) << "\n";
                break;
            }
            
            // Process all ready events
            for (int i = 0; i < nev; i++) {
                handleEvent(events[i]);
            }
        }
    }
    
    void stop() {
        running_ = false;
    }


private:
    uint16_t port_; //This port is used to check if :This packet belongs to  server
    ThreadPool& pool_; //passed buy refernce to avoid copying:Thread pools cannot be copied (and shouldn't be).
    bool running_;
    
    int kqueue_fd_;//The OS gives you an integer ID (File Descriptor) that represents "The Socket hooked up to Port 6379".
    //it refers to the internal pointer to the queue as kqueue lives in Kernel Space and cant be touched directly by user programs.
    int listen_fd_;// The specific socket waiting for new connections (TCP Handshake).
    //you told the OS: "Put listen_fd_ inside the list of things kqueue_fd_ is watching

//     Packet arrives at Port 6379 -> Matched because of port_
//     OS sees it's a new connection -> Queues it on listen_fd_.
//     kqueue_fd_  sees activity on listen_fd_ -> Wakes up your program so you can call accept().

    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    //Map of active connections: Key = socket FD, Value = Connection state

    void handleEvent(const struct kevent& ev) {
        int fd = static_cast<int>(ev.ident);
        
        // New connection
        if (fd == listen_fd_) {
            //If the ID matches your listen_fd_ (the one you bound to port 6379),
            // it means a New Client is trying to connect (TCP Handshake complete)
            
            acceptConnection();
            
        } else {
            // Data ready to read
            if (ev.filter == EVFILT_READ) {//Is there data?
                handleRead(fd);
            }
            
            // Connection closed or error
            if (ev.flags & (EV_EOF | EV_ERROR)) {//Did they disconnect?
                closeConnection(fd);
            }
        }
    }

    void acceptConnection() {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, 
                               &addr_len);

        // OS Action:
        // The OS accepts the connection.
        // The OS looks at the client's details (e.g., IP: 203.0.113.5, Port: 54321).
        // The OS writes those details into your client_addr variable.
        // The OS updates addr_len (if necessary) to say how many bytes it actually wrote.
        
        if (client_fd == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "[ERROR] accept() failed: " 
                          << strerror(errno) << "\n";
            }
            return;
        }
        
        // Set non-blocking
        setNonBlocking(client_fd);
        
        // Register with kqueue
        registerEvent(client_fd, EVFILT_READ, EV_ADD);
        
        // Create connection state
        connections_[client_fd] = std::make_unique<Connection>(client_fd);
        
        std::cout << "[CONN] New client: FD=" << client_fd << "\n";
    }

    void setNonBlocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1) {
            throw std::runtime_error("fcntl(F_GETFL) failed");
        }
        
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::runtime_error("fcntl(F_SETFL) failed");
        }
    }

    void registerEvent(int fd, int16_t filter, uint16_t flags) {
        struct kevent ev;
        EV_SET(&ev, fd, filter, flags, 0, 0, nullptr);
        
        if (kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr) == -1) {
            std::cerr << "[ERROR] kevent(EV_SET) failed: " 
                      << strerror(errno) << "\n";
        }
    }

    void handleRead(int client_fd) {
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) return;
        
        Connection& conn = *it->second;
        //We get a reference to their personal Connection object (conn). This object holds their private memory buffer.
        
        // Read data into buffer
        char temp_buf[BUFFER_SIZE];
        ssize_t n = recv(client_fd, temp_buf, sizeof(temp_buf), 0);
        
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                closeConnection(client_fd);
            }
            return;
        }
        
        conn.read_buffer.append(temp_buf, n);
        //We simply glue the new data onto the end of whatever data this client sent previously.
        
        // Try to parse commands from buffer
        while (conn.parse_pos < conn.read_buffer.size()) {
            Command cmd;
            size_t old_pos = conn.parse_pos;
            
            if (RESPParser::parse(conn.read_buffer, conn.parse_pos, cmd)) {
                // Complete command parsed - submit to thread pool
                Task task(client_fd, std::move(cmd));
                
                if (!pool_.submit(std::move(task))) {
                    // Queue full - send error
                    std::string err = "-ERR Server overloaded\r\n";
                    send(client_fd, err.c_str(), err.size(), 0);
                }
            } else {
                // Incomplete command - need more data
                conn.parse_pos = old_pos;
                break;
            }
        }
        
        // Compact buffer if we've consumed data
        if (conn.parse_pos > 0) {
            conn.read_buffer.erase(0, conn.parse_pos);
            conn.parse_pos = 0;
        }
        //Once we successfully parse a command (e.g., the first 20 bytes), 
        //we delete those 20 bytes from memory. The buffer now only contains unprocessed data.
        
        // Prevent buffer growth attack
        if (conn.read_buffer.size() > 1024 * 1024) {  // 1MB limit
            std::cerr << "[WARN] Client buffer too large, closing\n";
            closeConnection(client_fd);
        }
        //If a single incomplete command grows larger than 1MB, we assume it's an attack or a bug. We disconnect them immediately to save RAM.
    }
     void closeConnection(int client_fd) {
        std::cout << "[CONN] Closing FD=" << client_fd << "\n";
        
        // Remove from kqueue
        registerEvent(client_fd, EVFILT_READ, EV_DELETE);
        
        // Close socket
        close(client_fd);
        
        // Remove connection state
        connections_.erase(client_fd);
    }

      int createListenSocket(uint16_t port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == -1) {
            throw std::runtime_error("socket() failed: " + 
                std::string(strerror(errno)));
        }
        
        // Set socket options
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        
        // Make non-blocking
        setNonBlocking(sock);
        
        // Bind to port
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(sock);
            throw std::runtime_error("bind() failed: " + 
                std::string(strerror(errno)));
        }
        
        // Start listening
        if (listen(sock, 128) == -1) {
            close(sock);
            throw std::runtime_error("listen() failed: " + 
                std::string(strerror(errno)));
        }
        
        return sock;
    }

      // Claims the Address: It tells the Operating System, "I reserve Port 6379. Send any data arriving there to me."
      // Creates the Queue: It sets up a "waiting room" (the backlog) in the kernel where incoming users wait until you are ready to talk to them.
      // Configures Performance: It tweaks the settings (Non-blocking, Reuse Address) so your server can handle thousands of users without crashing or getting stuck.

      //###>Feature
      // temp_buf,                                      conn.read_buffer
      // Size,Fixed (4096 bytes),                       Dynamic (Grows as needed)
      // Memory Type,"Stack (Very fast, temporary)",    "Heap (Slower, persistent)"
      // Purpose,To transfer data from Kernel to User,  To store data until a full command is formed
      // Lifetime,Dies when handleRead returns,         Lives as long as the Client is connected



};
} // namespace flashkv