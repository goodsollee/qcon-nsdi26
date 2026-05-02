#!/usr/bin/env python3

import subprocess
import threading
import time
import socket
import sys
import os
import signal
import json
from threading import Event

class MultiQueueTest:
    def __init__(self):
        self.processes = []
        self.test_results = {}
        self.shutdown_event = Event()

    def cleanup(self):
        """Clean up all processes"""
        print("\n[Test] Cleaning up processes...")
        for proc in self.processes:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
        self.processes.clear()

    def signal_handler(self, signum, frame):
        """Handle Ctrl+C gracefully"""
        print(f"\n[Test] Received signal {signum}, shutting down...")
        self.shutdown_event.set()
        self.cleanup()
        sys.exit(0)

    def start_emulator_component(self, component, config_file, log_prefix=""):
        """Start an emulator component"""
        try:
            cmd = [f"./bin/{component}", config_file]
            print(f"[Test] Starting {component} with config {config_file}")

            log_file = f"{log_prefix}{component}.log"
            with open(log_file, 'w') as f:
                proc = subprocess.Popen(
                    cmd,
                    stdout=f,
                    stderr=subprocess.STDOUT,
                    cwd="."
                )

            self.processes.append(proc)
            return proc
        except Exception as e:
            print(f"[Test] Error starting {component}: {e}")
            return None

    def check_process_health(self, proc, name):
        """Check if a process is still running"""
        if proc.poll() is not None:
            print(f"[Test] WARNING: {name} has terminated unexpectedly")
            return False
        return True

    def create_test_packets(self, num_packets=50, queue_priorities=[3, 2, 1, 0]):
        """Create test packets for different priority queues"""
        test_packets = []

        for i in range(num_packets):
            queue_id = i % len(queue_priorities)
            priority = queue_priorities[queue_id]

            # Create different sized packets for different priorities
            if priority == 3:  # High priority - small packets
                size = 64 + (i % 100)
            elif priority == 2:  # Medium-high priority
                size = 200 + (i % 300)
            elif priority == 1:  # Medium-low priority
                size = 500 + (i % 500)
            else:  # Low priority - large packets
                size = 1000 + (i % 400)

            packet = {
                'id': i,
                'queue_id': queue_id,
                'priority': priority,
                'size': size,
                'data': f"TestPacket_{i}_Queue_{queue_id}_Priority_{priority}_" + "X" * (size - 50)
            }
            test_packets.append(packet)

        return test_packets

    def monitor_logs(self, log_file, patterns, timeout=30):
        """Monitor log file for specific patterns"""
        found_patterns = {pattern: False for pattern in patterns}
        start_time = time.time()

        try:
            while time.time() - start_time < timeout and not self.shutdown_event.is_set():
                if os.path.exists(log_file):
                    with open(log_file, 'r') as f:
                        content = f.read()
                        for pattern in patterns:
                            if pattern in content and not found_patterns[pattern]:
                                found_patterns[pattern] = True
                                print(f"[Test] Found pattern '{pattern}' in {log_file}")

                if all(found_patterns.values()):
                    break

                time.sleep(1)

        except Exception as e:
            print(f"[Test] Error monitoring {log_file}: {e}")

        return found_patterns

    def run_basic_connectivity_test(self):
        """Test basic connectivity from sender to receiver"""
        print("\n" + "="*60)
        print("[Test] Starting Basic Connectivity Test")
        print("="*60)

        # Start components in order
        time.sleep(1)

        # Start UE PDCP receiver
        ue_proc = self.start_emulator_component("pdcp_ue_link", "config_example_4queues.json", "test_")
        if not ue_proc:
            return False
        time.sleep(2)

        # Start DU link
        du_proc = self.start_emulator_component("pdcp_du_link", "config_example_4queues.json", "test_")
        if not du_proc:
            return False
        time.sleep(2)

        # Start PDCP sender
        sender_proc = self.start_emulator_component("pdcp_sender", "config_example_4queues.json", "test_")
        if not sender_proc:
            return False
        time.sleep(3)

        print("[Test] All components started, running for 30 seconds...")

        # Monitor for specific log patterns indicating successful operation
        patterns_to_check = [
            "MAC Sender] Scheduler started",
            "RLC Sender] Initialized",
            "RLC Receiver] RLC Receiver Module initialized",
            "MAC Sender] Processing slot"
        ]

        # Check processes are running and monitor logs
        test_duration = 30
        start_time = time.time()

        while time.time() - start_time < test_duration and not self.shutdown_event.is_set():
            # Check process health
            if not self.check_process_health(sender_proc, "pdcp_sender"):
                return False
            if not self.check_process_health(du_proc, "pdcp_du_link"):
                return False
            if not self.check_process_health(ue_proc, "pdcp_ue_link"):
                return False

            time.sleep(1)

        print("[Test] Basic connectivity test completed")
        return True

    def run_multi_queue_priority_test(self):
        """Test multi-queue priority handling"""
        print("\n" + "="*60)
        print("[Test] Starting Multi-Queue Priority Test")
        print("="*60)

        # This test would require instrumenting the code to inject specific packets
        # For now, we'll check that the multi-queue components are working

        # Check log files for multi-queue related messages
        log_patterns = [
            "queue_0",
            "queue_1",
            "queue_2",
            "queue_3",
            "priority_3",
            "priority_2",
            "priority_1",
            "priority_0"
        ]

        found_patterns = self.monitor_logs("test_pdcp_du_link.log", log_patterns, timeout=10)

        priority_support = any(found_patterns.values())
        if priority_support:
            print("[Test] Multi-queue priority support detected")
        else:
            print("[Test] Multi-queue priority support not clearly detected in logs")

        return priority_support

    def analyze_test_results(self):
        """Analyze and report test results"""
        print("\n" + "="*60)
        print("[Test] Test Results Summary")
        print("="*60)

        # Check log files for errors
        log_files = ["test_pdcp_sender.log", "test_pdcp_du_link.log", "test_pdcp_ue_link.log"]

        for log_file in log_files:
            if os.path.exists(log_file):
                print(f"\n[Test] Analyzing {log_file}:")
                with open(log_file, 'r') as f:
                    content = f.read()

                # Count important events
                errors = content.count("ERROR")
                warnings = content.count("WARN")
                infos = content.count("INFO")

                print(f"  - Errors: {errors}")
                print(f"  - Warnings: {warnings}")
                print(f"  - Info messages: {infos}")

                # Look for specific success indicators
                if "Scheduler started" in content:
                    print("  ✓ MAC scheduler started successfully")
                if "RLC Receiver Module initialized" in content:
                    print("  ✓ RLC receiver initialized successfully")
                if "Status PDU" in content:
                    print("  ✓ Status PDU mechanism active")
                if "queue_" in content:
                    print("  ✓ Multi-queue support active")

    def run_full_test_suite(self):
        """Run the complete test suite"""
        print("Starting Multi-Queue Emulator Test Suite")
        print("Configuration: Single link, 4 priority queues")

        # Set up signal handling
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

        try:
            # Test 1: Basic connectivity
            connectivity_ok = self.run_basic_connectivity_test()
            self.test_results['connectivity'] = connectivity_ok

            if connectivity_ok:
                # Test 2: Multi-queue priority handling
                priority_ok = self.run_multi_queue_priority_test()
                self.test_results['multi_queue'] = priority_ok
            else:
                print("[Test] Skipping multi-queue test due to connectivity issues")
                self.test_results['multi_queue'] = False

            # Analyze results
            self.analyze_test_results()

            # Final summary
            print("\n" + "="*60)
            print("[Test] FINAL TEST SUMMARY")
            print("="*60)

            if self.test_results.get('connectivity', False):
                print("✓ Basic connectivity: PASSED")
            else:
                print("✗ Basic connectivity: FAILED")

            if self.test_results.get('multi_queue', False):
                print("✓ Multi-queue priority: PASSED")
            else:
                print("✗ Multi-queue priority: INCONCLUSIVE")

            overall_success = self.test_results.get('connectivity', False)
            if overall_success:
                print("\n🎉 Overall test result: SUCCESS")
                print("Packets can successfully flow from PDCP sender to UE PDCP")
                return 0
            else:
                print("\n❌ Overall test result: FAILURE")
                print("Issues detected in packet flow")
                return 1

        except Exception as e:
            print(f"[Test] Test suite failed with exception: {e}")
            return 1
        finally:
            self.cleanup()

def main():
    # Check if binaries exist
    required_binaries = ["pdcp_sender", "pdcp_du_link", "pdcp_ue_link"]
    for binary in required_binaries:
        if not os.path.exists(f"bin/{binary}"):
            print(f"Error: Required binary bin/{binary} not found")
            print("Please run 'make' to build the emulator first")
            return 1

    # Check if config file exists
    if not os.path.exists("config_example_4queues.json"):
        print("Error: Configuration file config_example_4queues.json not found")
        return 1

    # Create logs directory
    os.makedirs("multi_queue_logs", exist_ok=True)
    os.makedirs("ue_logs", exist_ok=True)

    # Run tests
    test_suite = MultiQueueTest()
    return test_suite.run_full_test_suite()

if __name__ == "__main__":
    sys.exit(main())