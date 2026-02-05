# Battery Guard Multi-Device Monitor

ESP32-based BLE monitor for up to 4 Battery Guard devices simultaneously. Supports automatic device discovery, encrypted communication, and real-time battery monitoring.

## Features

- ✅ **Multi-Device Support** - Monitor up to 4 Battery Guard devices in parallel
- ✅ **Automatic Discovery** - Continuously scans for devices as they come and go
- ✅ **Auto-Reconnection** - Automatically reconnects when devices power cycle
- ✅ **Encrypted Communication** - AES-128-CBC handshake protocol
- ✅ **Real-time Monitoring** - Voltage, State of Charge (SOC), Temperature, Status
- ✅ **Retry Logic** - 3 connection attempts with 30-second cooldown period
- ✅ **Clean Output** - Production mode with optional debug logging

## Hardware Requirements

- **ESP32 Development Board** (tested with DOIT ESP32 DEVKIT V1)
- **Battery Guard BLE Device(s)** (50:54:7B:XX:XX:XX)
- USB cable for programming

## Software Requirements

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) library (auto-installed)
- AES key, hardcoded in App. Decompile the Android APK to find it

## Quick Start

### 1. Clone and Setup

```bash
cd "Battery Guard Demo"
cp include/config.h.sample include/config.h
```

### 2. Configure Devices

Edit `include/config.h`:

```cpp
const DeviceConfig DEVICES[] = {
    {
        .serial = "50547B815AFB",    // Your device MAC (no colons)
        .name = "Main Battery",
        .type = LEAD_ACID,           // or AGM, LITHIUM, etc.
        .enabled = true
    },
    // Add up to 3 more devices...
};
```

**Finding your device MAC:**
- it is printed on device
- Enter the correct AES key. Otherwise BLE communication wont work.

### 3. Build and Upload

**Production mode (clean output):**
```bash
platformio run -e release --target upload
```

**Debug mode (detailed logging):**
```bash
platformio run -e debug --target upload
```

### 4. Monitor Serial Output

```bash
platformio device monitor -b 115200
```

## Configuration

### Device Types (include/types.h)

```cpp
LEAD_ACID           // Standard Lead Acid (6-write handshake)
AGM                 // AGM Battery (6-write handshake)
LITHIUM             // Lithium Standard (6-write handshake)
OTHER_INTELLIGENT   // Other + Intelligent mode (7-write handshake)
OTHER_MANUAL        // Other + Manual mode (7-write handshake)
LITHIUM_INTELLIGENT // Lithium + Intelligent (7-write handshake)
LITHIUM_MANUAL      // Lithium + Manual (7-write handshake)
```

### Adjustable Parameters (include/config.h)

```cpp
SCAN_INTERVAL           // BLE scan interval (default: 62.5ms)
SCAN_WINDOW             // BLE scan window (default: 50ms)
CONNECT_TIMEOUT_MS      // Connection timeout (default: 10s)
MAX_CONNECT_RETRIES     // Retry attempts (default: 3)
RETRY_COOLDOWN_MS       // Cooldown period (default: 30s)
NOTIFICATION_TIMEOUT_MS // Data timeout (default: 60s)
```

## Serial Output

### Production Mode (Release)

```
============================================================
Battery Guard Multi-Device Monitor
============================================================
[Main Battery] Initialized: 50547B815AFB (Type: 0x01)

Monitoring 1 device(s):
  [1] Main Battery (50547B815AFB) - Type: 0x01
============================================================

[Main Battery] Found device: 50:54:7b:81:5a:fb - STOPPING SCAN!
[Main Battery] Connecting to 50:54:7b:81:5a:fb...
[Main Battery] Connected!
[Main Battery] Connected successfully!
[Main Battery] Handshake complete, waiting for notifications...

Main Battery (50547B815AFB): 11.99V | 42% | 23°C | normal
Main Battery (50547B815AFB): 11.98V | 41% | 23°C | normal
```

### Debug Mode

Enable with:
```bash
platformio run -e debug --target upload
```

Shows detailed information:
- Timestamp for each operation `[XXX.XXs]`
- All BLE scan results
- Connection attempts and durations
- Handshake write operations (encrypted data)
- Notification decryption (plaintext bytes)
- State transitions

## State Machine

```
DISCONNECTED → SCANNING → CONNECTING → HANDSHAKE → MONITORING
                    ↑                                    ↓
                    └────────────────────────────────────┘
                              (on disconnect)
                    
After 3 failed attempts → COOLDOWN (30s) → back to SCANNING
```

## Protocol Details

### BLE Characteristics

- **Service UUID:** `0xFFF0`
- **Write Characteristic:** `0xFFF3` (Handshake)
- **Notify Characteristic:** `0xFFF4` (Data)

### Handshake Sequence

6-write encrypted sequence (LEAD_ACID/AGM):
1. Device type initialization
2. Battery type setting
3. Parameter configuration (voltage thresholds)
4. Parameter configuration (current thresholds)
5. Mode setting
6. Finalization

All writes use AES-128-CBC encryption with static key.

### Data Format

Notifications arrive every ~1 second:
- **Voltage:** 0.01V resolution (uint16 / 100)
- **SOC:** 0-100% (uint8)
- **Temperature:** °C (int8)
- **Status:** normal, low, critical, charging

## Troubleshooting

### Device Not Found
- Check Battery Guard is powered on
- Verify MAC address in config.h (no colons)
- Enable debug mode to see all scanned devices
- Ensure device is not connected to phone app

### Connection Fails
- Device may be too far away
- Check battery level of Battery Guard
- Wait for cooldown period to expire
- Try power cycling the Battery Guard

### Immediate Disconnect After Connection
- Check AES key is correct
- Verify battery type setting matches device
- Enable debug mode to see handshake details

### No Data After Handshake
- This is normal - device sends data every ~1 second
- Check NOTIFICATION_TIMEOUT_MS setting
- Verify device has battery connected

### Flash Size Too Large
- Release mode: ~599KB
- Debug mode: ~603KB
- ESP32 has 1310KB available

## Build Environments

### Release (default)
- Minimal output
- Optimized for production use
- Flash size: ~599KB
- `platformio run -e release --target upload`

### Debug
- Verbose logging with timestamps
- Byte-level protocol inspection
- State transition tracking
- Flash size: ~603KB
- `platformio run -e debug --target upload`

## File Structure

```
Battery Guard Demo/
├── include/
│   ├── config.h           # Your device configuration (git-ignored)
│   ├── config.h.sample    # Template for config.h
│   └── types.h            # Battery type definitions
├── src/
│   └── main.cpp           # Main application code
├── platformio.ini         # Build configuration
└── README.md              # This file
```

## Technical Details

- **Platform:** ESP32 (espressif32)
- **Framework:** Arduino
- **BLE Stack:** NimBLE-Arduino v1.4.3
- **Encryption:** mbedtls AES-128-CBC
- **Language:** C++
- **RAM Usage:** 11.0% (35,924 / 327,680 bytes)
- **Flash Usage:** 45.7% (599,185 / 1,310,720 bytes)

## License

See project documentation for license information.

## Contributing

This is a demo/research project for Battery Guard BLE protocol reverse engineering.

## Credits

Based on protocol analysis of the Battery Guard mobile application and BLE packet captures.
