#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <csignal>

#include "config_parser.hpp"
#include "network/link.hpp"
#include "pdcp_receiver.hpp"
#include "log.hpp"

#define MODULE "PDCP_RX_MAIN"

static std::atomic_bool g_running(true);
static void signalHandler(int) { g_running.store(false); }

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Parse command-line arguments
    std::string configFile = "config.json";
    int receiveBasePort = 14000;  // Default base port to receive from UE links
    int sendPort = 9000;          // Default port to send to processor

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            configFile = argv[++i];
        } else if (arg == "-r" && i + 1 < argc) {
            receiveBasePort = std::stoi(argv[++i]);
        } else if (arg == "-s" && i + 1 < argc) {
            sendPort = std::stoi(argv[++i]);
        }
    }

    // Load configuration
    ConfigParser parser;
    if (!parser.loadConfig(configFile)) {
        std::cerr << "Failed to load config: " << configFile << std::endl;
        return 1;
    }

    auto allPdcpConfigs = parser.getAllPdcpConfigs();
    if (allPdcpConfigs.empty()) {
        std::cerr << "No PDCP configs found. Exiting.\n";
        return 1;
    }
    
    PdcpConfig rxConfig = allPdcpConfigs[0];
    int numPaths = rxConfig.common.path_num;
    
    LOG_MODULE_INFO(MODULE, "Using PDCP Receiver config with " << numPaths << " paths");
    
    std::unique_ptr<PdcpReceiver> pdcpReceiver = std::make_unique<PdcpReceiver>(rxConfig);

    // Create Links for receiving from multiple UE Links
    std::vector<std::unique_ptr<Link>> ueReceivers;
    
    for (int i = 0; i < numPaths; i++) {
        int receivePort = receiveBasePort + i;
        std::string receiveEndpoint = "tcp://*:" + std::to_string(receivePort);
        
        auto ueReceiver = std::make_unique<Link>(
            "PdcpReceiver_Receiver_" + std::to_string(i),
            receiveEndpoint,          // PULL (bind) - receives from UE Link
            "tcp://127.0.0.1:9997"    // PUSH (connect) - not used
        );
        
        // Register callback for receiving DL_DATA messages
        ueReceiver->registerCallback("DL_DATA", [&pdcpReceiver, i](const Message& msg) {
            LOG_MODULE_INFO(MODULE, "Received DL_DATA from UE link " << i << ", size=" 
                << msg.payload.size());
            
            if (msg.payload.empty()) return;
            
            // Copy the payload to a buffer
            unsigned char buffer[65535];
            size_t inLen = 0;
            copyMessageToBuffer(msg, buffer, inLen, sizeof(buffer));
            
            if (inLen == 0) {
                LOG_MODULE_WARN(MODULE, "DL_DATA too large or empty, ignoring.");
                return;
            }
            
            // Process packet through PDCP Receiver
            // This will eventually trigger the delivery callback for reassembled packets
            pdcpReceiver->processPacket(buffer, inLen);
        });
        
        // Start the receiver
        ueReceiver->start();
        
        LOG_MODULE_INFO(MODULE, "Listening for messages from UE link " << i << " on " << receiveEndpoint);
        ueReceivers.push_back(std::move(ueReceiver));
    }

    // Create a Link for sending back to PDCP Processor
    std::string sendEndpoint = "tcp://127.0.0.1:" + std::to_string(sendPort);
    std::unique_ptr<Link> processorLink = std::make_unique<Link>(
        "PdcpReceiver_Sender",
        "tcp://*:0",             // PULL (bind) - using ephemeral port, not used
        sendEndpoint             // PUSH (connect) - sends to PDCP Processor
    );
    LOG_MODULE_INFO(MODULE, "Sending messages to Processor at " << sendEndpoint);

    // Start the sender link
    processorLink->start();

    // Set the PDCP receiver's delivery callback
    pdcpReceiver->setDeliveryCallback([&pdcpReceiver, &processorLink]() {
        unsigned char buffer[65535];
        size_t delivered_size = 0;

        // deliverPacket may return multiple reassembled packets
        while (pdcpReceiver->deliverPacket(buffer, delivered_size)) {
            if (delivered_size == 0) {
                break;
            }

            // Convert reassembled packet to string for ZMQ message
            std::string payload(reinterpret_cast<const char*>(buffer), delivered_size);
            
            // Send to PDCP Processor with message type "UE_ARRIVAL"
            processorLink->sendMessage("PDCP_PROCESSOR", "UE_ARRIVAL", payload);
            
            LOG_MODULE_INFO(MODULE, "PDCP Receiver delivered and sent packet size=" 
                << delivered_size << " bytes to PDCP Processor");
        }
    });

    LOG_MODULE_INFO(MODULE, "PDCP Receiver running with " << numPaths << " input paths. Ctrl-C to stop...");
    
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    LOG_MODULE_INFO(MODULE, "Stopping all links...");
    
    for (auto& receiver : ueReceivers) {
        receiver->stop();
    }
    
    processorLink->stop();
    pdcpReceiver.reset();

    LOG_MODULE_INFO(MODULE, "PDCP Receiver terminated gracefully.");
    return 0;
}