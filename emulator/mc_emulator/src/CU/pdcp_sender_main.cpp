#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>

#include "config_parser.hpp"
#include "CU/pdcp_sender.hpp"
#include "network/link.hpp"
#include "log.hpp"

#define MODULE "PDCP_SENDER_MAIN"

static std::atomic_bool g_running(true);

// Simple signal handler to allow graceful termination
static void signalHandler(int) {
    g_running.store(false);
}


int main(int argc, char* argv[]) {
    // 1) Handle signals (Ctrl-C, kill)
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 2) Parse command-line arguments
    std::string configFile = "config.json";
    int receivePort = 10000;  // Default port to receive from processor
    int sendBasePort = 11000; // Default base port to send to DU links

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            configFile = argv[++i];
        } else if (arg == "-r" && i + 1 < argc) {
            receivePort = std::stoi(argv[++i]);
        } else if (arg == "-s" && i + 1 < argc) {
            sendBasePort = std::stoi(argv[++i]);
        }
    }

    // 3) Create a config parser and load the file
    ConfigParser parser;
    if (!parser.loadConfig(configFile)) {
        std::cerr << "Failed to load config file: " << configFile << std::endl;
        return 1;
    }

    // 4) Get PDCP configs
    auto allPdcpConfigs = parser.getAllPdcpConfigs();
    if (allPdcpConfigs.empty()) {
        std::cerr << "No PDCP configs found. Exiting.\n";
        return 1;
    }
    
    // Use config index 0 for Sender
    PdcpConfig senderConfig = allPdcpConfigs[0];
    LOG_MODULE_INFO(MODULE, "Using Sender config");

    // 5) Create a PdcpSender with the CU config
    std::unique_ptr<PdcpSender> pdcpSender = std::make_unique<PdcpSender>(senderConfig);

    // 6) Create a receiver Link that gets messages from PDCP Processor
    std::string receiveEndpoint = "tcp://*:" + std::to_string(receivePort);
    std::unique_ptr<Link> processorReceiver = std::make_unique<Link>(
        "PdcpSender_Receiver",
        receiveEndpoint,        // PULL (bind) - receives from PDCP Processor
        "tcp://127.0.0.1:9999"  // PUSH (connect) - not used
    );
    LOG_MODULE_INFO(MODULE, "Listening for messages from processor on " << receiveEndpoint);

    // 7) Create Links for sending to multiple DU endpoints
    std::vector<std::unique_ptr<Link>> duLinks;
    int numLinks = senderConfig.common.path_num;

    // Create a link for each path
    for (int i = 0; i < numLinks; i++) {
        int duPort = sendBasePort + i;
        std::string duEndpoint = "tcp://127.0.0.1:" + std::to_string(duPort);
        
        auto duLink = std::make_unique<Link>(
            "PdcpSender_DuLink_" + std::to_string(i),
            "tcp://*:0",         // PULL (bind) - using ephemeral port, not used
            duEndpoint           // PUSH (connect) - sends to DU
        );
        
        duLink->start();
        LOG_MODULE_INFO(MODULE, "Created link to DU " << i << " at " << duEndpoint);
        duLinks.push_back(std::move(duLink));
    }

    // 8) Register the delivery callback with PdcpSender
    pdcpSender->setDeliveryCallback([&duLinks](unsigned char* packet, size_t len, uint8_t pathId) -> bool {
        if (len == 0) {
            return false;
        }

        // Check if path ID is valid
        if (pathId >= duLinks.size()) {
            LOG_MODULE_ERROR(MODULE, "Invalid path ID: " << static_cast<int>(pathId)
                << ", max: " << duLinks.size() - 1);
            // Default to the first link if path is invalid
            pathId = 0;
        }

        // Convert data to string
        std::string payload(reinterpret_cast<const char*>(packet), len);

        // Forward the processed packet to the appropriate DU Link
        duLinks[pathId]->sendMessage("DU", "DL_DATA", payload);

        LOG_MODULE_INFO(MODULE, "PDCP Sender forwarded packet, size=" << len 
            << " bytes to DU Link " << static_cast<int>(pathId));
        
        return true;
    });

    // 9) Define how processorReceiver handles incoming messages from PDCP Processor
    processorReceiver->registerCallback("DL_DATA", [&pdcpSender](const Message& msg) {
        LOG_MODULE_INFO(MODULE, "Received DL_DATA from PDCP Processor, size=" 
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
        
        // Process the packet through the PDCP Sender
        pdcpSender->processPacket(buffer, inLen);
    });

    // 10) Start the processor receiver link
    processorReceiver->start();

    LOG_MODULE_INFO(MODULE, "pdcp_sender_main is running with " << numLinks 
        << " DU links. Press Ctrl-C to stop.");

    // 11) Wait until the user sends Ctrl-C or similar
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 12) Cleanup: stop all links
    LOG_MODULE_INFO(MODULE, "Stopping links and PDCP sender...");
    processorReceiver->stop();
    for (auto& link : duLinks) {
        link->stop();
    }
    pdcpSender.reset();  // calls destructor

    LOG_MODULE_INFO(MODULE, "pdcp_sender_main terminated gracefully.");
    return 0;
}