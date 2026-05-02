#!/usr/bin/env python3
# tcp_test.py - A simple TCP client that attempts to establish a connection and send/receive data

import socket
import sys
import time
import argparse

def tcp_test(host, port, payload_size=1024, attempts=3, verbose=True):
    """Test TCP connectivity to a host and port, with retries."""
    
    if verbose:
        print(f"Testing TCP connection to {host}:{port}")
        print(f"Payload size: {payload_size} bytes")
        print(f"Attempts: {attempts}")
    
    success = False
    
    for i in range(attempts):
        if verbose:
            print(f"\nAttempt {i+1}/{attempts}:")
        
        try:
            # Create socket
            if verbose:
                print("  Creating socket...", end="", flush=True)
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5)  # 5 second timeout
            if verbose:
                print("OK")
            
            # Connect
            if verbose:
                print(f"  Connecting to {host}:{port}...", end="", flush=True)
            start_time = time.time()
            s.connect((host, port))
            connect_time = time.time() - start_time
            if verbose:
                print(f"OK ({connect_time:.3f}s)")
            
            # Send data
            if verbose:
                print(f"  Sending {payload_size} bytes...", end="", flush=True)
            payload = b'X' * payload_size
            start_time = time.time()
            s.sendall(payload)
            send_time = time.time() - start_time
            if verbose:
                print(f"OK ({send_time:.3f}s)")
            
            # Try to receive data
            if verbose:
                print("  Receiving data...", end="", flush=True)
            start_time = time.time()
            data = s.recv(1024)
            recv_time = time.time() - start_time
            if verbose:
                print(f"OK ({recv_time:.3f}s, {len(data)} bytes)")
            
            # Close
            s.close()
            if verbose:
                print("  Connection closed")
            
            success = True
            break
            
        except socket.timeout:
            if verbose:
                print("FAILED (timeout)")
            s.close()
        
        except ConnectionRefusedError:
            if verbose:
                print("FAILED (connection refused)")
            s.close()
        
        except ConnectionResetError:
            if verbose:
                print("FAILED (connection reset)")
            s.close()
        
        except Exception as e:
            if verbose:
                print(f"FAILED ({e})")
            s.close()
        
        # Wait before retry
        if i < attempts - 1:
            time.sleep(1)
    
    if success:
        if verbose:
            print("\nTCP test PASSED")
        return True
    else:
        if verbose:
            print("\nTCP test FAILED")
        return False

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test TCP connectivity")
    parser.add_argument("host", help="Target hostname or IP")
    parser.add_argument("port", type=int, help="Target port")
    parser.add_argument("--size", type=int, default=1024, help="Payload size in bytes")
    parser.add_argument("--attempts", type=int, default=3, help="Number of connection attempts")
    parser.add_argument("--quiet", action="store_true", help="Suppress verbose output")
    
    args = parser.parse_args()
    
    success = tcp_test(args.host, args.port, args.size, args.attempts, not args.quiet)
    sys.exit(0 if success else 1)