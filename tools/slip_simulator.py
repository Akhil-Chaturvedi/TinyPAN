#!/usr/bin/env python3
"""
TinyPAN SLIP Simulator (slip_simulator.py)

This script mimics the behavior of a BLE MCU running TinyPAN in `TINYPAN_USE_BLE_SLIP=1` mode.
It acts as a local TCP Server (simulating the BLE UART pipe). When a Companion App
connects to it, it continuously streams SLIP-encoded IPv4 ICMP Echo Requests (Pings)
to simulate live MCU network traffic.

Usage:
    python tools/slip_simulator.py

The Companion App should connect to 127.0.0.1:8080 and decode the SLIP stream.
"""

import socket
import struct
import time

# SLIP Protocol Constants (RFC 1055)
SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

def checksum(data):
    """Calculate the standard IP/ICMP checksum"""
    if len(data) % 2 != 0:
        data += b'\0'
    s = sum(struct.unpack('!%dH' % (len(data)//2), data))
    s = (s >> 16) + (s & 0xffff)
    s += s >> 16
    return ~s & 0xffff

def build_icmp_echo():
    """Builds a raw IPv4 ICMP Echo Request packet from 192.168.44.2 to 8.8.8.8"""
    # ICMP Header
    type_icmp = 8 # Echo Request
    code = 0
    chksum_icmp = 0
    id_icmp = 0x1234
    seq = 1
    payload = b"Hello from TinyPAN Simulator!   "
    
    icmp_header = struct.pack('!BBHHH', type_icmp, code, chksum_icmp, id_icmp, seq)
    chksum_icmp = checksum(icmp_header + payload)
    icmp_header = struct.pack('!BBHHH', type_icmp, code, chksum_icmp, id_icmp, seq)
    icmp_packet = icmp_header + payload
    
    # IPv4 Header
    version_ihl = (4 << 4) | 5
    dscp_ecn = 0
    total_len = 20 + len(icmp_packet)
    id_ip = 0xabcd
    flags_frag = 0
    ttl = 64
    proto = 1 # ICMP
    chksum_ip = 0
    src_ip = socket.inet_aton("192.168.44.2")
    dst_ip = socket.inet_aton("8.8.8.8")
    
    ip_header = struct.pack('!BBHHHBBH4s4s', version_ihl, dscp_ecn, total_len, 
                            id_ip, flags_frag, ttl, proto, chksum_ip, src_ip, dst_ip)
    chksum_ip = checksum(ip_header)
    ip_header = struct.pack('!BBHHHBBH4s4s', version_ihl, dscp_ecn, total_len, 
                            id_ip, flags_frag, ttl, proto, chksum_ip, src_ip, dst_ip)
    
    return ip_header + icmp_packet

def slip_encode(packet):
    """Encodes a raw bytearray into SLIP format with escape character stuffing"""
    encoded = bytearray()
    encoded.append(SLIP_END) # Start delimiter
    
    for b in packet:
        if b == SLIP_END:
            encoded.append(SLIP_ESC)
            encoded.append(SLIP_ESC_END)
        elif b == SLIP_ESC:
            encoded.append(SLIP_ESC)
            encoded.append(SLIP_ESC_ESC)
        else:
            encoded.append(b)
            
    encoded.append(SLIP_END) # End delimiter
    return bytes(encoded)

def main():
    print("Building raw IPv4 ICMP packet...")
    raw_packet = build_icmp_echo()
    
    print("SLIP encoding packet...")
    slip_packet = slip_encode(raw_packet)
    print(f"Original Length: {len(raw_packet)} bytes -> SLIP Length: {len(slip_packet)} bytes")
    
    HOST = '127.0.0.1'
    PORT = 8080
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen()
        
        print(f"\n[MCU Simulator] Waiting for Companion App to connect on {HOST}:{PORT}...")
        conn, addr = s.accept()
        with conn:
            print(f"[MCU Simulator] Companion App connected from {addr}")
            
            packet_count = 1
            try:
                while True:
                    print(f"[MCU Simulator] TX: SLIP ICMP Echo Request #{(packet_count)}")
                    conn.sendall(slip_packet)
                    packet_count += 1
                    time.sleep(2) # Send a ping every 2 seconds
            except (ConnectionResetError, BrokenPipeError):
                print("\n[MCU Simulator] Companion App disconnected.")

if __name__ == '__main__':
    main()
