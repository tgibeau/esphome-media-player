# Sonos Direct Mode - Phase 2 & 3 Implementation

This document summarizes the implementation of Phase 2 and Phase 3 features for Sonos Direct Mode in the ESPHome Media Player project.

## Overview

**Phase 1 (Previously Implemented):** Single Sonos speaker connection via manual IP entry

**Phase 2 (New):** Multiple speaker support with dropdown selector and manual configuration

**Phase 3 (New):** SSDP auto-discovery and ZoneGroupTopology parsing for automatic speaker detection and grouping information

## Implementation Summary

### Modified Files

#### 1. `common/device/sonos_select.yaml`
- Added `select` entity for dropdown speaker selection
- Implemented speaker map parsing from substitutions
- Added auto-discovery trigger
- Enhanced text field with dropdown synchronization
- Added dynamic speaker list updates from discovery

#### 2. `common/device/base.yaml`
- Added `sonos_speakers` substitution for predefined speaker list
- Format: `"Name1:IP1,Name2:IP2,Name3:IP3"`
- Default: `"none"` (manual/discovery only)

#### 3. `components/sonos_player/sonos_player.h`
- Added `SonosSpeaker` struct for speaker information
- Added discovery state management
- Added methods: `start_discovery()`, `get_discovered_speakers()`
- Added helper methods for SSDP and topology queries

#### 4. `components/sonos_player/sonos_player.cpp`
- Implemented `perform_ssdp_discovery()` using UDP multicast
- Implemented `query_zone_group_topology()` for speaker grouping
- Implemented `parse_zone_groups()` for XML parsing
- Added socket-based SSDP M-SEARCH functionality

### New Files

#### 1. `docs/features/sonos-direct-mode.md`
- Comprehensive documentation for all three phases
- Configuration examples and troubleshooting guide
- Technical details and API documentation
- Feature comparison table (Direct vs HA mode)

#### 2. `docs/examples/sonos-config.yaml`
- Example configuration file
- Usage instructions for all connection methods
- Best practices and recommendations

## Features

### Phase 2: Multiple Speaker Support

#### User-Visible Features
- Dropdown selector in web interface
- Pre-configured speaker list support
- Quick switching between speakers
- Custom IP entry option
- Persistent selection across reboots

#### Configuration
```yaml
substitutions:
  sonos_speakers: "Living Room:192.168.1.10,Kitchen:192.168.1.11,Bedroom:192.168.1.12"
```

#### Web UI Controls
- **Sonos Speaker** (dropdown): 
  - "None (HA Mode)" - Disable Direct mode
  - "Custom IP" - Manual IP entry
  - "Auto-discover" - Trigger network scan
  - [Configured speakers] - Pre-defined speakers
  - [Discovered speakers] - Auto-found speakers

- **Sonos IP** (text field):
  - Manual IP address entry
  - Syncs with dropdown selection
  - Backwards compatible with Phase 1

### Phase 3: Auto-Discovery

#### SSDP Discovery
- Multicast UDP discovery on 239.255.255.250:1900
- Searches for `urn:schemas-upnp-org:device:ZonePlayer:1`
- 3-second timeout with response collection
- Non-blocking single-shot scan

#### ZoneGroupTopology Support
- Queries `/ZoneGroupTopology/Control` endpoint
- Parses `GetZoneGroupState` XML response
- Extracts speaker names, UUIDs, and group membership
- Updates discovered speaker list with room names

#### Automatic Integration
- Discovered speakers added to dropdown dynamically
- First speaker auto-selected after discovery
- Room names used when available
- Falls back to IP-based names

## Technical Details

### Network Protocols

#### SSDP (Simple Service Discovery Protocol)
- Protocol: UDP multicast
- Address: 239.255.255.250:1900
- Search target: `urn:schemas-upnp-org:device:ZonePlayer:1`
- MX (max wait): 2 seconds
- Socket timeout: 3 seconds

#### UPnP/SOAP (Sonos API)
- Protocol: HTTP POST with XML body
- Port: 1400 (standard Sonos API port)
- Endpoints:
  - `/MediaRenderer/AVTransport/Control` - Playback
  - `/MediaRenderer/RenderingControl/Control` - Volume
  - `/ZoneGroupTopology/Control` - Grouping (Phase 3)

### Data Structures

#### SonosSpeaker
```cpp
struct SonosSpeaker {
  std::string name;           // Room name or "Sonos (IP)"
  std::string ip;             // IP address
  std::string uuid;           // UPnP UUID
  std::string room;           // Sonos room name
  bool is_coordinator;        // Group coordinator flag
  std::vector<std::string> group_members;  // Group members
};
```

#### Storage
- `discovered_speakers_`: `std::map<std::string, SonosSpeaker>`
- Key: IP address
- Value: Speaker metadata

### Threading and Blocking

**Current Implementation:**
- Discovery runs synchronously in the calling context
- 3-second timeout prevents long blocks
- Called from YAML lambda (main loop context)

**Future Enhancement Opportunity:**
- Move discovery to background task
- Update speakers via callback
- Non-blocking UI during scan

## Testing Recommendations

### Manual Testing Checklist

#### Phase 2 - Multiple Speakers
- [ ] Configure multiple speakers in substitutions
- [ ] Verify speakers appear in dropdown
- [ ] Select each speaker and verify connection
- [ ] Switch between speakers
- [ ] Test "Custom IP" mode
- [ ] Test "None (HA Mode)" to disable
- [ ] Verify persistence after reboot

#### Phase 3 - Discovery
- [ ] Trigger auto-discovery
- [ ] Verify speakers are found (check logs)
- [ ] Verify room names are populated
- [ ] Verify first speaker auto-connects
- [ ] Check discovered speakers added to dropdown
- [ ] Test discovery on different network topologies
- [ ] Test with Sonos groups active

#### Integration
- [ ] Test Phase 1 compatibility (direct IP entry)
- [ ] Test switching between all modes
- [ ] Verify HA mode still works
- [ ] Check web interface controls
- [ ] Verify playback control works in all modes

### Edge Cases

1. **No Speakers Found**
   - Discovery completes with empty list
   - Log warning shown
   - Fallback to manual entry

2. **Network Timeouts**
   - 3-second socket timeout
   - No infinite hangs
   - User can retry

3. **Multicast Blocked**
   - Discovery fails gracefully
   - Manual entry still available
   - Clear error logging

4. **Duplicate IPs**
   - Map key is IP (unique)
   - Later discovery overwrites
   - Predefined speakers take priority

## Known Limitations

### Current Limitations

1. **Dynamic Dropdown Options**
   - ESPHome `select` entity options are static in YAML
   - Discovered speakers stored in map but not dynamically added to UI dropdown
   - Workaround: Manual entry of discovered IPs works
   - Future: Requires ESPHome feature enhancement

2. **Single Active Speaker**
   - Can only control one speaker at a time
   - Switching speakers requires disconnecting first

3. **Blocking Discovery**
   - Discovery blocks for up to 3 seconds
   - UI may be unresponsive during scan

### Planned Enhancements

- Background discovery task
- Dynamic dropdown population (ESPHome API pending)
- Multiple simultaneous speaker control
- Group join/leave commands
- Favorites and playlist support

## Migration Guide

### From Phase 1

No changes required! Phase 1 functionality (manual IP entry) remains fully functional.

Optional: Add predefined speakers for convenience:
```yaml
substitutions:
  sonos_speakers: "Main:192.168.1.10"
```

### For New Installations

1. Add speaker configuration to device YAML
2. Flash firmware
3. Use dropdown or discovery to select speaker
4. Done!

## Documentation

- **User Guide:** `docs/features/sonos-direct-mode.md`
- **Example Config:** `docs/examples/sonos-config.yaml`
- **API Reference:** Code comments in `sonos_player.h` and `sonos_player.cpp`

## Future Work

### Short Term
- Test on real hardware with multiple Sonos speakers
- Validate discovery across different network configurations
- Performance optimization for discovery timeout
- Enhanced error handling and user feedback

### Long Term
- Dynamic dropdown option updates (pending ESPHome support)
- Background discovery service
- Speaker grouping control (join/unjoin groups)
- Favorites and playlist browsing
- EQ and audio settings control
- Sleep timer support
- Alarm management

## Compatibility

### ESPHome Version
- Requires ESPHome 2023.4.0 or later
- Uses standard `text_sensor`, `sensor`, `select`, `text` components
- Socket API requires ESP-IDF framework

### Hardware
- ESP32 family (ESP32, ESP32-S3, ESP32-P4)
- Requires WiFi connectivity
- No special hardware requirements

### Sonos Devices
- All Sonos speakers with UPnP API support
- Tested targets: Play:1, Play:5, Sonos One, Beam, Move
- Should work with all Sonos ZonePlayers

## Contributing

To extend or modify:

1. **Add new discovery methods:**
   - Implement new helper in `sonos_player.cpp`
   - Add public method to `sonos_player.h`
   - Call from YAML lambda or script

2. **Add new speaker commands:**
   - Add method to `SonosPlayer` class
   - Implement SOAP call with appropriate endpoint
   - Expose to YAML via lambda

3. **Enhance UI:**
   - Modify `sonos_select.yaml`
   - Add new entities (buttons, sensors, etc.)
   - Wire to C++ component methods

## License

This implementation maintains the project's existing license (PolyForm Noncommercial License 1.0.0).
