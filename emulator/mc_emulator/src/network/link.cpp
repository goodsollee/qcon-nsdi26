#include "network/link.hpp"
#include "log.hpp"   // Or whichever logging header you use

#include <iostream>

Link::Link(const std::string& linkId,
           const std::string& recvEndpoint,
           const std::string& sendEndpoint)
    : linkId_(linkId),
      recvEndpoint_(recvEndpoint),
      sendEndpoint_(sendEndpoint),
      context_(1),
      recvSocket_(context_, zmq::socket_type::pull),
      sendSocket_(context_, zmq::socket_type::push),
      running_(false)
{
    // Bind the Pull socket to receive incoming messages
    // Example: "tcp://*:5555" to allow external connections
    sendSocket_.setsockopt(ZMQ_IDENTITY, linkId_.data(), linkId_.size());

    recvSocket_.bind(recvEndpoint_);
    LOG_MODULE_INFO(linkId_, "Bound Pull socket at " << recvEndpoint_);

    // Connect the Push socket for sending messages
    // Example: "tcp://127.0.0.1:6666"
    sendSocket_.connect(sendEndpoint_);
    LOG_MODULE_INFO(linkId_, "Connected Push socket to " << sendEndpoint_);
}

Link::~Link() {
    stop();
}

void Link::start() {
    LOG_MODULE_INFO(linkId_, "Starting recvLoop in background thread...");
    running_ = true;
    recvThread_ = std::thread(&Link::recvLoop, this);
}

void Link::stop() {
    if (running_) {
        running_ = false;

        // Optionally close sockets to break any blocking recv
        try {
            recvSocket_.close();
            sendSocket_.close();
            context_.close();
        } catch (...) {
            // You can catch any ZMQ errors here
        }

        if (recvThread_.joinable()) {
            recvThread_.join();
        }
        LOG_MODULE_INFO(linkId_, "Link stopped");
    }
}

void Link::sendMessage(const std::string& dst,
                       const std::string& type,
                       const std::string& payload)
{
    LOG_MODULE_DEBUG(linkId_, "Sending message -> dst=" << dst
        << ", type=" << type << ", payloadLen=" << payload.size());

    
    zmq::message_t typeFrame(type.begin(), type.end());
    zmq::message_t payloadFrame(payload.begin(), payload.end());
    sendSocket_.send(typeFrame, zmq::send_flags::sndmore);
    sendSocket_.send(payloadFrame, zmq::send_flags::none);
}

void Link::registerCallback(const std::string& msgType, Callback cb) {
    callbackMap_[msgType] = cb;
}

/**
 * @brief The main loop that receives from the Pull socket.
 *        We expect 3 frames: [dst][type][payload].
 *        Then we dispatch to the callback if a matching type is found.
 */
void Link::recvLoop() {
    zmq::pollitem_t items[] = {
        { static_cast<void*>(recvSocket_), 0, ZMQ_POLLIN, 0 }
    };

    while (running_) {
        // Poll the socket with a short timeout to allow safe shutdown
        zmq::poll(items, 1, std::chrono::milliseconds(100));

        if (!running_) {
            break;
        }

        if (items[0].revents & ZMQ_POLLIN) {
            // ROUTER frames: [senderID][empty][dst][type][payload]
            zmq::message_t typeFrame;
            zmq::message_t payloadFrame;
            auto r1 = recvSocket_.recv(typeFrame, zmq::recv_flags::none);
            if (!r1.has_value()) continue;  // no message
            auto r2 = recvSocket_.recv(payloadFrame, zmq::recv_flags::none);
            if (!r2.has_value()) continue;  // partial message

            Message msg;
            msg.type.assign((char*)typeFrame.data(), typeFrame.size());
            msg.payload.assign((char*)payloadFrame.data(), payloadFrame.size());

            msg.dst = linkId_;  // this node is the dst

            LOG_MODULE_DEBUG(linkId_, "Received msg type=" << msg.type << ", len=" << msg.payload.size());
            
            // Callback according to the type
            auto it = callbackMap_.find(msg.type);
            if (it != callbackMap_.end()) {
                it->second(msg);
            } else {
                LOG_MODULE_WARN(linkId_, "No callback for type=" << msg.type);
            }
        }
    }

    LOG_MODULE_INFO(linkId_, "recvLoop thread exiting");
}
