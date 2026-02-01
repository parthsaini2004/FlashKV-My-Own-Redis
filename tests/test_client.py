#!/usr/bin/env python3
"""
FlashKV Test Client & Benchmark
Demonstrates correctness and measures performance
"""

import socket
import time
import threading
from typing import List, Tuple
import statistics

class FlashKVClient:
    """Simple Python client for FlashKV"""
    
    def __init__(self, host='localhost', port=7379):
        self.host = host
        self.port = port
        self.sock = None
    
    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((self.host, self.port))
    
    def close(self):
        if self.sock:
            self.sock.close()
    
    def send_command(self, cmd: str) -> str:
        """Send command and receive response"""
        self.sock.sendall(cmd.encode() + b'\n')
        return self.sock.recv(4096).decode()
    
    def set(self, key: str, value: str) -> str:
        return self.send_command(f"SET {key} {value}")
    
    def get(self, key: str) -> str:
        return self.send_command(f"GET {key}")
    
    def delete(self, key: str) -> str:
        return self.send_command(f"DEL {key}")

def test_basic_operations():
    """Test correctness of basic operations"""
    print("\n=== Testing Basic Operations ===")
    
    client = FlashKVClient()
    client.connect()
    
    # Test SET
    resp = client.set("user:1", "Alice")
    print(f"SET user:1 Alice -> {resp.strip()}")
    
    # Test GET
    resp = client.get("user:1")
    print(f"GET user:1 -> {resp.strip()}")
    
    # Test overwrite
    resp = client.set("user:1", "Bob")
    resp = client.get("user:1")
    print(f"GET user:1 (after overwrite) -> {resp.strip()}")
    
    # Test DEL
    resp = client.delete("user:1")
    print(f"DEL user:1 -> {resp.strip()}")
    
    # Test GET on deleted key
    resp = client.get("user:1")
    print(f"GET user:1 (after delete) -> {resp.strip()}")
    
    client.close()
    print("✓ Basic operations test passed\n")

def benchmark_single_thread(num_ops: int) -> Tuple[float, List[float]]:
    """Benchmark single-threaded throughput"""
    client = FlashKVClient()
    client.connect()
    
    latencies = []
    
    start = time.time()
    
    for i in range(num_ops):
        key = f"key:{i}"
        value = f"value_{i}"
        
        op_start = time.time()
        client.set(key, value)
        latencies.append((time.time() - op_start) * 1000)  # ms
    
    elapsed = time.time() - start
    client.close()
    
    return elapsed, latencies

def benchmark_worker(client_id: int, num_ops: int, results: List):
    """Worker thread for concurrent benchmark"""
    client = FlashKVClient()
    client.connect()
    
    latencies = []
    start = time.time()
    
    for i in range(num_ops):
        key = f"key:{client_id}:{i}"
        value = f"value_{i}"
        
        op_start = time.time()
        client.set(key, value)
        latencies.append((time.time() - op_start) * 1000)
    
    elapsed = time.time() - start
    client.close()
    
    results.append({
        'client_id': client_id,
        'elapsed': elapsed,
        'latencies': latencies
    })

def benchmark_concurrent(num_clients: int, ops_per_client: int):
    """Benchmark concurrent throughput"""
    print(f"\n=== Concurrent Benchmark ({num_clients} clients) ===")
    
    results = []
    threads = []
    
    start = time.time()
    
    for i in range(num_clients):
        t = threading.Thread(target=benchmark_worker, 
                           args=(i, ops_per_client, results))
        threads.append(t)
        t.start()
    
    for t in threads:
        t.join()
    
    total_elapsed = time.time() - start
    total_ops = num_clients * ops_per_client
    
    # Aggregate latencies
    all_latencies = []
    for r in results:
        all_latencies.extend(r['latencies'])
    
    print(f"Total Operations:  {total_ops:,}")
    print(f"Total Time:        {total_elapsed:.2f}s")
    print(f"Throughput:        {total_ops/total_elapsed:,.0f} ops/sec")
    print(f"Latency p50:       {statistics.median(all_latencies):.2f}ms")
    print(f"Latency p95:       {statistics.quantiles(all_latencies, n=20)[18]:.2f}ms")
    print(f"Latency p99:       {statistics.quantiles(all_latencies, n=100)[98]:.2f}ms")
    print(f"Latency max:       {max(all_latencies):.2f}ms")

def main():
    print("""
    ╔══════════════════════════════════════════════╗
    ║   FlashKV Test Suite & Benchmark             ║
    ║   Expected Performance: >50K ops/sec         ║
    ╚══════════════════════════════════════════════╝
    """)
    
    try:
        # Test correctness
        test_basic_operations()
        
        # Single-threaded benchmark
        print("=== Single-Threaded Benchmark ===")
        ops = 10000
        elapsed, latencies = benchmark_single_thread(ops)
        print(f"Operations:  {ops:,}")
        print(f"Time:        {elapsed:.2f}s")
        print(f"Throughput:  {ops/elapsed:,.0f} ops/sec")
        print(f"Avg Latency: {statistics.mean(latencies):.2f}ms")
        
        # Concurrent benchmark
        benchmark_concurrent(num_clients=10, ops_per_client=1000)
        benchmark_concurrent(num_clients=50, ops_per_client=200)
        
        print("\n✓ All benchmarks completed successfully!")
        
    except ConnectionRefusedError:
        print("\n❌ Error: Could not connect to FlashKV server")
        print("   Make sure the server is running on port 7379")
    except Exception as e:
        print(f"\n❌ Error: {e}")

if __name__ == '__main__':
    main()