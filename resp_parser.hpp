
//##########################################################################################################

// ###### understanding the process:
// #full process 
// 1. A client sends a command to the server (e.g., GET key, SET key value).
// 2. The server reads the raw command from the network socket.
// 3. The server uses the RESP parser to convert the raw command into a Command struct.
// 4. The Command struct is wrapped in a Task struct with client_fd and pushed to the task queue.
// 5. A worker thread pops the Task from the queue, executes the Command against the database, and generates a Response struct.
// 6. The Response struct is converted back to RESP format and sent back to the client via the network socket.

// ###### detailed process explanation client -> network ->(os)(kernel)->server:
//client send data as stream of bytes at network level
//then these are stored at kernel level buffers
//server reads these bytes from socket into user space buffer(we created)
//then we parse these bytes from user space buffer into meaningful commands(we created command struct)
//then we execute these commands against database

// #notation for resp protocol used in comments:
// * :Array:> *3\r\nTells you "Prepare to receive 3 distinct pieces of data."
// $Bulk String :> $3\r\nTells you "The next string is exactly 3 bytes long."
// \r\n : CRLF(End of line) : The "Full Stop" of the protocol. It separates the metadata from the data.

// ###### eg
// eg: we get :
// *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nParth\r\n

//we parse it as:
// First pass: Sees $3\r\nSET\r\n. It reads the 3, then uses buf.substr(pos, 3) to grab "SET".
// Second pass: Sees $4\r\nname\r\n. It reads the 4, grabs "name".
// Third pass: Sees $5\r\nparth\r\n. It reads the 5, grabs "parth".

//but if due to network delay we get partial data like:
// *3\r\n$3\r\nSET\r\n$4\r\nna

//we wait for more data to arrive before parsing further.
// we stor the pointer uptill where we have parsed
//once we wait and data arrives remaning: //me\r\n$5\r\nParth\r\n ,then we append it to existing buffer
// passed pos by reference (size_t& pos) to keep track of how much we've parsed so far.
// continue parsing from that position next time.


// ##### not implemented here but in future we can implement dynammic buffer and resting pos=0 or shifting buffer
// to avoid buffer overflow when buffer is full and we have parsed some data already.
//eg: if buffer size is 1024 and we have parsed 800 bytes already , then we can shift remaining 224 bytes to start of buffer
// and reset pos=0 , so that we have more space to read new data from socket

// ##### Why we did NOT use getline()
// getline() reads until it hits a newline character ('\n'). if network issue inly some data comes,
// getline is a blocking call; it freezes your entire thread while waiting for a client, 
// making it impossible to handle thousands of connections simultaneously.

//Stateful Stream Parsing: We use a Moving Cursor (pos) and a Dynamic Accumulation Buffer to remember exactly where we left off during network delays
//also bytestream data can include images whereas getline only works for text data

//##########################################################################################################


// resp_parser.hpp - Redis Serialization Protocol Parser
#pragma once

#include "common.hpp"
#include <sstream> //==>Inspecting/modifying single characters
#include <cctype> //==> Converting strings to numbers (and vice versa)
#include <vector>
namespace flashkv {


// Functions :The Role of Each Function in RESP Parsing
// parse,The Dispatcher: Acts as the entry point; it looks at the first byte to decide if the data is a formal RESP Array (*) or just a simple text command.
// parseArray,"The Orchestrator: Manages the high-level loop. It asks ""How many parts are in this command?"" and coordinates other functions to collect those parts."
// readBulkString,The Precision Reader: Uses the length provided (after $) to extract exactly that many bytes. This is what allows your parser to handle binary data like images or JSON safely.
// readInteger,The Metadata Decoder: Converts the text digits found in the protocol (like the 3 in $3) into actual C++ numbers that the code can use for logic.
// buildCommand,"The Logic Validator: Takes the list of strings (e.g., SET, key, val) and checks if they make sense together before creating a final Command object."
// parseSimple,"The Human Translator: A fallback that splits data based on spaces and newlines, allowing a human to type directly into a terminal without using the complex RESP format."
// consumeCRLF,The Housekeeper: A small helper that makes sure the cursor (pos) skips over the \r\n markers so the next function starts at clean data.


class RESPParser {


    public :
    // Parse RESP command from buffer
    // Returns true if complete command parsed, false if need more data

    static bool parse(const std::string &buffer , size_t &pos , Command &cmd) {
        
        if (pos >= buffer.size()) return false;

        // Skip whitespace
        while (pos < buffer.size() && std::isspace(buffer[pos])) {
            pos++;
        }

        // RESP format starts with '*' for arrays
        if (buffer[pos] == '*') {
            return parseArray(buffer, pos, cmd);
        }
        
        // Fallback: simple text protocol for testing
        // "SET key value\n" or "GET key\n"
        return parseSimple(buffer, pos, cmd);


    }

    private: 
    static bool parseArray(const std::string& buf, size_t& pos, Command& cmd) {
        
        // *3\r\n -> array of 3 elements
        pos++;  // Skip '*'

        size_t count = 0;
        if (!readInteger(buf, pos, count)) return false;
        if (!consumeCRLF(buf, pos)) return false;



        if (count < 1 || count > 3) return false;
        std::vector<std::string> args;
         for (size_t i = 0; i < count; i++) {
            std::string arg;
            if (!readBulkString(buf, pos, arg)) return false;
            args.push_back(std::move(arg));
        }

        return buildCommand(args, cmd);
    }

    //testing purpose
    static bool parseSimple(const std::string& buf, size_t& pos, Command& cmd) {
        std::vector<std::string> args;
        std::string token;
        
        while (pos < buf.size()) {
            char c = buf[pos++];
            if (c == '\n' || c == '\r') {
                if (!token.empty()) {
                    args.push_back(token);
                    token.clear();
                }
                if (c == '\n') break;
            } else if (std::isspace(c)) {
                if (!token.empty()) {
                    args.push_back(token);
                    token.clear();
                }
            } else {
                token += c;
            }
        }
        
        if (!token.empty()) args.push_back(token);
        
        return !args.empty() && buildCommand(args, cmd);
    }

    static bool readBulkString(const std::string& buf, size_t& pos, std::string& out) {
        if (pos >= buf.size() || buf[pos] != '$') return false;
        pos++;  // Skip '$'
        
        size_t len = 0;
        if (!readInteger(buf, pos, len)) return false;
        if (!consumeCRLF(buf, pos)) return false;
        
        if (pos + len > buf.size()) return false;
        
        out = buf.substr(pos, len);
        pos += len;
        
        return consumeCRLF(buf, pos);
    }
    
    static bool readInteger(const std::string& buf, size_t& pos, size_t& out) {
        size_t start = pos;
        while (pos < buf.size() && std::isdigit(buf[pos])) {
            pos++;
        }
        if (pos == start) return false;
        
        out = std::stoull(buf.substr(start, pos - start));
        return true;
    }

    static bool consumeCRLF(const std::string& buf, size_t& pos) {

        if (pos + 1 >= buf.size()) return false;
        if (buf[pos] == '\r' && buf[pos + 1] == '\n') {
            pos += 2;
            return true;
        }
        // Tolerate just \n for simple protocol
        if (buf[pos] == '\n') {
            pos++;
            return true;
        }
        return false;
    }

    static bool buildCommand(const std::vector<std::string>& args, Command& cmd) {
        if (args.empty()) return false;
        
        std::string cmdStr = args[0];
        // Convert to uppercase
        for (char& c : cmdStr) c = std::toupper(c);
        
        if (cmdStr == "SET") {
            if (args.size() != 3) return false;
            cmd = Command(CommandType::SET, args[1], args[2]);
            return true;
        } else if (cmdStr == "GET") {
            if (args.size() != 2) return false;
            cmd = Command(CommandType::GET, args[1]);
            return true;
        } else if (cmdStr == "DEL") {
            if (args.size() != 2) return false;
            cmd = Command(CommandType::DEL, args[1]);
            return true;
        }
        
        return false;
    }
};

} // namespace flashkv