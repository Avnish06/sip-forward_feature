#!/usr/bin/env python3
"""
Resource analyzer for SIP container - diagnose audio issues
"""
import subprocess
import json
import time
import requests
from datetime import datetime

def run_docker_cmd(cmd):
    try:
        result = subprocess.run(
            ["docker", "exec", "sip_outbound", "sh", "-c", cmd],
            capture_output=True,
            text=True,
            check=True
        )
        return result.stdout.strip()
    except subprocess.CalledProcessError as e:
        return f"ERROR: {e.stderr.strip()}"

def analyze_file_descriptors(pid="1"):
    """Analyze file descriptor usage patterns"""
    print("=== FILE DESCRIPTOR ANALYSIS ===")
    
    # Count FDs
    fd_count = run_docker_cmd(f"ls /proc/{pid}/fd")
    fd_lines = fd_count.split('\n') if fd_count else []
    print(f"Total FDs: {len(fd_lines)}")
    
    # Analyze FD types
    fd_details = run_docker_cmd(f"ls -la /proc/{pid}/fd")
    socket_count = fd_details.count("socket:")
    eventfd_count = fd_details.count("eventfd")
    timerfd_count = fd_details.count("timerfd")
    pipe_count = fd_details.count("pipe:")
    
    print(f"  - Sockets: {socket_count}")
    print(f"  - EventFDs: {eventfd_count}")
    print(f"  - TimerFDs: {timerfd_count}")
    print(f"  - Pipes: {pipe_count}")
    
    return {
        'total': len(fd_lines),
        'sockets': socket_count,
        'eventfds': eventfd_count,
        'timerfds': timerfd_count,
        'pipes': pipe_count
    }

def analyze_threads(pid="1"):
    """Analyze thread patterns"""
    print("\n=== THREAD ANALYSIS ===")
    
    # Get thread info
    threads = run_docker_cmd(
        f"find /proc/{pid}/task -name comm -exec cat {{}} \\;"
    )
    thread_list = threads.split('\n') if threads else []
    
    thread_counts = {}
    for thread in thread_list:
        thread_counts[thread] = thread_counts.get(thread, 0) + 1
    
    print(f"Total threads: {len(thread_list)}")
    for name, count in thread_counts.items():
        print(f"  - {name}: {count}")
    
    return thread_counts

def analyze_memory(pid="1"):
    """Analyze memory usage patterns"""
    print("\n=== MEMORY ANALYSIS ===")
    
    status = run_docker_cmd(f"cat /proc/{pid}/status")
    for line in status.split('\n'):
        if any(key in line for key in ['VmSize', 'VmRSS', 'VmPeak', 'VmHWM']):
            print(f"  {line}")

def main():
    print(f"=== SIP AUDIO DEBUG ANALYSIS - {datetime.now()} ===")
    
    # Intall pgrep conditionally
    run_docker_cmd(
        "command -v pgrep >/dev/null 2>&1 || "
        "(apt-get update && apt-get install -y procps)"
    )
    
    # Get sip_outbound PID
    sip_outbound_pid = run_docker_cmd("pgrep -fo sip_outbound | awk '{print $1}'")
    print(f"SIP Outbound PID: {sip_outbound_pid}")
    
    # Basic resource analysis
    analyze_file_descriptors(pid=sip_outbound_pid)
    analyze_threads(pid=sip_outbound_pid)
    analyze_memory(pid=sip_outbound_pid)

if __name__ == "__main__":
    main()
