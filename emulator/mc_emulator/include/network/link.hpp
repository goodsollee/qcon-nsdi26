#pragma once

#include <zmq.hpp>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <functional>
#include <string>

// Message structure for communication between nodes
struct Message {
    std::string src;
    std::string dst;
    std::string type;
    std::string payload;
};

/**
 * @brief A helper function to retrieve zero-copy data from a Message payload
 *        into a buffer if needed, or you can pass directly to PdcpSender.
 */
static void copyMessageToBuffer(const Message& msg, unsigned char* buffer, size_t& outLen, size_t maxSize) {
    if (msg.payload.size() > maxSize) {
        outLen = 0;
        return;
    }
    memcpy(buffer, msg.payload.data(), msg.payload.size());
    outLen = msg.payload.size();
}


/**
 * @brief A Link class that has:
 *  - one Pull socket to receive messages
 *  - one Push socket to send messages
 *
 * You register callbacks based on 'type' to handle incoming messages.
 */
class Link {
public:
    using Callback = std::function<void(const Message&)>;

    /**
     * @param linkId         A string ID to label the Link (for logging, etc.)
     * @param recvEndpoint   The ZMQ endpoint for receiving (e.g. "tcp://*:5555")
     * @param sendEndpoint   The ZMQ endpoint for sending (e.g. "tcp://127.0.0.1:6666")
     */
    Link(const std::string& linkId,
         const std::string& recvEndpoint,
         const std::string& sendEndpoint);

    ~Link();

    /**
     * @brief Starts the receiving loop in a background thread.
     */
    void start();

    /**
     * @brief Stops the receiving loop, closes sockets, joins thread.
     */
    void stop();

    /**
     * @brief Sends a multipart message composed of:
     *  [dst][type][payload]
     */
    void sendMessage(const std::string& dst,
                     const std::string& type,
                     const std::string& payload);

    /**
     * @brief Registers a callback for a given message type.
     *        If a message arrives with a matching 'type', the callback is invoked.
     */
    void registerCallback(const std::string& msgType, Callback cb);


private:
    /**
     * @brief The main loop that polls the Pull socket,
     *        receives messages, and calls the appropriate callback.
     */
    void recvLoop();

private:
    std::string linkId_;
    std::string recvEndpoint_;
    std::string sendEndpoint_;

    zmq::context_t context_;
    zmq::socket_t recvSocket_;
    zmq::socket_t sendSocket_;

    std::atomic<bool> running_;
    std::thread recvThread_;

    std::unordered_map<std::string, Callback> callbackMap_;
};