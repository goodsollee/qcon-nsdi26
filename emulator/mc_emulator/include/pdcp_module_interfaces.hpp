#ifndef PDCP_MODULE_INTERFACES_HPP
#define PDCP_MODULE_INTERFACES_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <chrono>
#include "pdcp_common.hpp"


// Maximum number of paths
#define MAX_PATHS 16

// Forward declarations for module components
class AsyncLogger;

// Forward declare PDCP module classes
class PdcpSender;
class PdcpDULink;
class PdcpUELink;
class PdcpReceiver;

#endif // PDCP_MODULE_INTERFACES_HPP