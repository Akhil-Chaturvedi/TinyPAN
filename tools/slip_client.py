#!/usr/bin/env python3
"""
TinyPAN SLIP Client (Companion App Mock)

This script connects to the `slip_simulator.py` on 127.0.0.1:8080 and simulates
the Flutter/Kotlin Companion App. It reads raw bytes, decodes the SLIP frames,
and parses the underlying IPv4 packets.
"""

import socket
import struct
import sys

# SLIP Protocol Constants
SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

def parse_ipv4(packet):
    """Deeply parses an IPv4 packet (assuming no IP options for simplicity)"""
    if len(packet) < 20:
        return "Packet too short!"
    
    version_ihl = packet[0]
    version = version_ihl >> 4
    ihl = (version_ihl & 0x0F) * 4
    
    total_len = struct.unpack('!H', packet[2:4])[0]
    proto = packet[9]
    src_ip = socket.inet_ntoa(packet[12:16])
    dst_ip = socket.inet_ntoa(packet[16:20])
    
    proto_str = "ICMP" if proto == 1 else "UDP" if proto == 17 else "TCP" if proto == 6 else str(proto)
    
    result = f"IPv{version} | {src_ip} -> {dst_ip} | Proto: {proto_str} | Len: {total_len}"
    
    # Parse ICMP if applicable
    if proto == 1 and len(packet) >= ihl + 8:
        icmp_type, icmp_code = struct.unpack('!BB', packet[ihl:ihl+2])
        if icmp_type == 8:
            result += " [Echo Request]"
            payload = packet[ihl+8:]
            if payload:
                # print safe printable chars
                safe_payload = "".join(chr(c) if 32 <= c < 127 else "." for c in payload)
                result += f" Payload: '{safe_payload}'"
    return result

class SlipDecoder:
    def __init__(self):
        self.buffer = bytearray()
        self.escape_active = False

    def decode_bytes(self, raw_bytes):
        packets = []
        for b in raw_bytes:
            if b == SLIP_END:
                if len(self.buffer) > 0:
                    packets.append(bytes(self.buffer))
                    self.buffer.clear()
                self.escape_active = False
            elif self.escape_active:
                if b == SLIP_ESC_END:
                    self.buffer.append(SLIP_END)
                elif b == SLIP_ESC_ESC:
                    self.buffer.append(SLIP_ESC)
                else:
                    print(f"SLIP Protocol Error: Invalid escape character {hex(b)}")
                self.escape_active = False
            elif b == SLIP_ESC:
                self.escape_active = True
            else:
                self.buffer.append(b)
        return packets

def main():
    HOST = '127.0.0.1'
    PORT = 8080
    
    decoder = SlipDecoder()
    
    print(f"[Companion App] Connecting to MCU Simulator at {HOST}:{PORT}...")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((HOST, PORT))
            print("[Companion App] Connected! Listening for SLIP frames...")
            
            while True:
                data = s.recv(1024)
                if not data:
                    print("[Companion App] Connection closed by Simulator.")
                    break
                
                # We might receive multiple frames, partial frames, or chunks. The decoder handles it.
                packets = decoder.decode_bytes(data)
                for pkt in packets:
                    print(f"\n<<< Received SLIP Frame ({len(pkt)} bytes) <<<")
                    print(f"    {parse_ipv4(pkt)}")
                    
    except ConnectionRefusedError:
        print("\n[Error] Could not connect. Ensure `slip_simulator.py` is running.")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n[Companion App] Terminated.")

if __name__ == '__main__':
    main()
