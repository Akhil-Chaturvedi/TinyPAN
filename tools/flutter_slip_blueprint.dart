/**
 * TinyPAN Flutter Companion App - SLIP Decoder Blueprint
 * 
 * This file serves as a reference implementation for the mobile companion app 
 * (iOS/Android) required for TinyPAN `TINYPAN_USE_BLE_SLIP=1` mode.
 * 
 * When using pure BLE chips (e.g., nRF52), the MCU cannot create a native Bluetooth 
 * Personal Hotspot. Instead, it streams raw IPv4 packets escaped in SLIP framing
 * over a BLE UART characteristic.
 * 
 * This Dart class takes the raw byte stream from a library like `flutter_blue_plus`,
 * reverses the SLIP escaping, and yields raw, fully-formed IPv4 packets that can be
 * injected into a `VpnService` on Android or `NetworkExtension` on iOS.
 */

import 'dart:typed_data';

// SLIP Protocol Constants (RFC 1055)
const int SLIP_END = 0xC0;
const int SLIP_ESC = 0xDB;
const int SLIP_ESC_END = 0xDC;
const int SLIP_ESC_ESC = 0xDD;

class TinyPanSlipDecoder {
  final List<int> _buffer = [];
  bool _escapeActive = false;
  
  /// Call this function every time you receive a new BLE characteristic notification
  /// containing raw bytes from the MCU.
  /// 
  /// Returns a list of decoded IPv4 packets. Usually, an IP packet takes multiple
  /// BLE UART transfers to arrive, in which case this will return an empty list `[]`
  /// until the packet boundary (SLIP_END) is received.
  List<Uint8List> decodeStream(List<int> incomingBytes) {
    List<Uint8List> packets = [];
    
    for (int b in incomingBytes) {
      if (b == SLIP_END) {
        if (_buffer.isNotEmpty) {
          // We reached the end of a valid SLIP frame!
          // Convert our dynamic buffer into a fixed IP packet payload.
          packets.add(Uint8List.fromList(_buffer));
          _buffer.clear();
        }
        _escapeActive = false;
      } else if (_escapeActive) {
        if (b == SLIP_ESC_END) {
          _buffer.add(SLIP_END);
        } else if (b == SLIP_ESC_ESC) {
          _buffer.add(SLIP_ESC);
        } else {
          print("TinyPAN Protocol Error: Invalid SLIP escape Sequence!");
          // Depending on robustness, you may want to clear the buffer here.
        }
        _escapeActive = false;
      } else if (b == SLIP_ESC) {
        _escapeActive = true;
      } else {
        _buffer.add(b);
      }
    }
    
    return packets;
  }
}

// ============================================================================
// Example Usage
// ============================================================================
/*
void _onBleDataReceived(List<int> bleData) {
  // Feed chunked BLE UART data into the decoder
  final decodedIpv4Packets = _slipDecoder.decodeStream(bleData);
  
  for (var ipv4Packet in decodedIpv4Packets) {
    print("Received full IP Datagram from MCU! Length: ${ipv4Packet.length} bytes");
    
    // --> Inject `ipv4Packet` into your OS VPN Tunnel Builder
    // VpnInjector.write(ipv4Packet);
  }
}
*/
