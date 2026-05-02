#ifndef THREAD_SAFE_QUEUE_HPP
#define THREAD_SAFE_QUEUE_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

// Packet structure for inter-module communication
struct Packet {
    std::vector<unsigned char> data;
    size_t len;
    uint8_t pathId;  // Added path ID field
    
    Packet() : len(0), pathId(0) {}
    
    Packet(const unsigned char* buffer, size_t length)
        : data(buffer, buffer + length), len(length), pathId(0) {}
        
    Packet(const unsigned char* buffer, size_t length, uint8_t path)
        : data(buffer, buffer + length), len(length), pathId(path) {}
    
    Packet(const Packet& other)
        : data(other.data), len(other.len), pathId(other.pathId) {}
    
    Packet& operator=(const Packet& other) {
        if (this != &other) {
            data = other.data;
            len = other.len;
            pathId = other.pathId;
        }
        return *this;
    }
};

// ---------------------------------------------------------------------------
// ThreadSafeQueue using std::queue with mutex and condition variables
// Multiple Producer, Multiple Consumer, with blocking & capacity
// ---------------------------------------------------------------------------
template<typename T>
class ThreadSafeQueue {
public:
    // Constructor with optional capacity
    explicit ThreadSafeQueue(size_t capacity = 1024)
        : capacity_(capacity), 
          stop_(false) 
    {
    }

    // Stop the queue and notify all waiting threads
    void stop() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_.store(true, std::memory_order_release);
        }
        // Notify all waiting threads so they can check the stop condition
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }

    // Resume the queue (caution: only if you really want to "unstop" it)
    void resume() {
        std::unique_lock<std::mutex> lock(mutex_);
        stop_.store(false, std::memory_order_release);
    }

    // Push an item to the queue (blocking if full)
    // Returns false if we were "stopped" before or during the push.
    bool push(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Wait until there's space or we're stopped
        not_full_cv_.wait(lock, [this] {
            return queue_.size() < capacity_ || stop_.load(std::memory_order_acquire);
        });
        
        if (stop_.load(std::memory_order_acquire)) {
            return false;
        }
        
        queue_.push(item);
        
        // Notify one waiting consumer that data is available
        not_empty_cv_.notify_one();
        
        return !stop_.load(std::memory_order_acquire);
    }

    // Pop an item from the queue (blocking)
    // Returns false if the queue is stopped or is aborted.
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Wait until an item is available or we're stopped
        not_empty_cv_.wait(lock, [this] {
            return !queue_.empty() || stop_.load(std::memory_order_acquire);
        });
        
        if (queue_.empty()) {
            // Queue is empty and we're stopped
            return false;
        }
        
        item = queue_.front();
        queue_.pop();
        
        // Notify one waiting producer that space is available
        not_full_cv_.notify_one();
        
        return !stop_.load(std::memory_order_acquire);
    }

    // Pop an item with timeout
    // Returns false if timed out or stopped.
    bool pop(T& item, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Wait until an item is available, we're stopped, or timeout
        bool success = not_empty_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
            return !queue_.empty() || stop_.load(std::memory_order_acquire);
        });
        
        if (queue_.empty()) {
            // Either timed out or we're stopped with an empty queue
            return false;
        }
        
        item = queue_.front();
        queue_.pop();
        
        // Notify one waiting producer that space is available
        not_full_cv_.notify_one();
        
        return !stop_.load(std::memory_order_acquire);
    }

    // Try to pop without blocking
    bool try_pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Wait until an item is available or we're stopped
        not_empty_cv_.wait(lock, [this] {
            return !queue_.empty() || stop_.load(std::memory_order_acquire);
        });
        
        if (queue_.empty()) {
            // Queue is empty and we're stopped
            return false;
        }
        
        item = queue_.front();
        queue_.pop();
        
        // Notify one waiting producer that space is available
        not_full_cv_.notify_one();
        
        return !stop_.load(std::memory_order_acquire);
    }

    // Check if queue is empty
    bool empty() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    // Get approximate queue size
    size_t size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // Clear the queue
    void clear() {
        std::unique_lock<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
        // Notify all waiting producers that space is available
        not_full_cv_.notify_all();
    }

private:
    std::queue<T> queue_;
    size_t capacity_;
    std::atomic<bool> stop_;
    
    mutable std::mutex mutex_;
    std::condition_variable not_empty_cv_;  // Signaled when queue becomes non-empty
    std::condition_variable not_full_cv_;   // Signaled when queue is no longer full
};

#endif // THREAD_SAFE_QUEUE_HPP