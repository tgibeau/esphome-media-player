# Sonos Direct Mode

Sonos Direct Mode allows your ESPHome Media Player to connect directly to Sonos speakers on your local network without requiring Home Assistant. The device polls the Sonos speaker's UPnP/SOAP API (port 1400) to display playback information and control the speaker.

This feature is useful when:
- You want a standalone Sonos controller without Home Assistant integration
- You need faster response times by polling directly
- You want to reduce network traffic through Home Assistant
- You're troubleshooting Home Assistant connectivity issues

## Features

### Phase 1: Single Speaker Connection
- Manual IP address entry
- Direct UPnP/SOAP communication
- Persistent configuration across reboots
- Switchable between HA mode and Direct mode

### Phase 2: Multiple Speaker Support
- Dropdown selector for multiple predefined speakers
- Easy switching between rooms
- Custom IP entry for ad-hoc connections
- Web UI configuration

### Phase 3: Auto-Discovery
- SSDP network discovery of Sonos speakers
- Automatic speaker detection
- Zone group topology support
- Group member information

## Configuration

### Basic Setup (Phase 1)

To use Sonos Direct Mode with a single speaker:

1. Navigate to the device's web interface
2. Find the **Sonos IP** text field in the configuration section
3. Enter your Sonos speaker's IP address (e.g., `192.168.1.100`)
4. The device will automatically connect and start polling the speaker

To return to Home Assistant mode, clear the IP address field.

### Multiple Speakers (Phase 2)

To configure multiple speakers, add them to your device YAML file:

```yaml
substitutions:
  sonos_speakers: "Living Room:192.168.1.10,Kitchen:192.168.1.11,Bedroom:192.168.1.12"
```

Then in the web interface:

1. Navigate to the **Sonos Speaker** dropdown
2. Select a speaker from the list
3. The device will automatically connect to the selected speaker

#### Dropdown Options

- **None (HA Mode)**: Disable Sonos Direct and use Home Assistant
- **Custom IP**: Enter a specific IP address manually
- **Auto-discover**: Scan the network for Sonos speakers
- **[Your Speakers]**: Pre-configured speakers from the YAML file

### Auto-Discovery (Phase 3)

To discover Sonos speakers on your network:

1. Navigate to the **Sonos Speaker** dropdown
2. Select **Auto-discover**
3. Wait a few seconds for the SSDP scan to complete
4. The first discovered speaker will be automatically selected
5. Discovered speakers are added to the dropdown list

The discovery process:
- Uses SSDP (Simple Service Discovery Protocol)
- Scans the local network for UPnP devices
- Filters for Sonos ZonePlayer devices
- Queries ZoneGroupTopology for speaker names and grouping
- Updates the speaker list dynamically

## Technical Details

### Communication Protocol

Sonos Direct Mode uses:
- **SOAP over HTTP**: For control commands (play, pause, volume, etc.)
- **UPnP**: For device discovery and service interaction
- **Port 1400**: Standard Sonos API port
- **SSDP**: Multicast discovery on 239.255.255.250:1900

### Polling Intervals

The component polls different services at different rates:
- **Transport state**: Every update interval (default: 1 second)
- **Position info**: Every update interval
- **Volume**: Every 5 update intervals (5 seconds)
- **Zone topology**: On-demand during discovery

### Services Used

1. **AVTransport**
   - `GetTransportInfo`: Playback state (playing/paused/stopped)
   - `GetPositionInfo`: Track metadata, position, duration
   - `Play`, `Pause`, `Next`, `Previous`: Playback control

2. **RenderingControl**
   - `GetVolume`: Current volume level
   - `SetVolume`: Volume adjustment

3. **ZoneGroupTopology** (Phase 3)
   - `GetZoneGroupState`: Speaker groups and members
   - Room names and coordinator information

## Compatibility

### Supported Sonos Devices

Sonos Direct Mode works with all Sonos speakers that support the UPnP API, including:
- Play:1, Play:3, Play:5
- Sonos One, One SL
- Beam, Arc, Ray
- Move, Roam
- Port, Amp
- And all other Sonos speakers with network connectivity

### Network Requirements

- Speakers must be on the same network as the ESPHome device
- Multicast traffic must be allowed for SSDP discovery
- Port 1400 must be accessible (not blocked by firewall)
- Static IP addresses or DHCP reservations recommended for reliability

## Limitations

### Current Limitations

1. **Single Active Connection**: Only one speaker can be controlled at a time
2. **Playlist Management**: Not supported in Direct mode
3. **Favorites/Services**: Cannot browse or play from music services
4. **Queue Management**: Cannot add/remove tracks from queue
5. **Speaker Settings**: Cannot change EQ, night mode, etc.

### Vs. Home Assistant Mode

| Feature | Direct Mode | HA Mode |
|---------|-------------|---------|
| Playback control | ✅ | ✅ |
| Volume control | ✅ | ✅ |
| Track metadata | ✅ | ✅ |
| Album artwork | ✅ | ✅ |
| Speaker grouping | ⚠️ Info only | ✅ Full control |
| Music service browsing | ❌ | ✅ |
| Playlist management | ❌ | ✅ |
| Multi-room control | ❌ | ✅ |
| Setup complexity | Easy | Moderate |
| Network hops | 1 | 2 |
| Latency | Lower | Higher |

## Troubleshooting

### Discovery Not Working

If auto-discovery fails to find speakers:

1. **Check network connectivity**: Ensure the ESP device and Sonos speakers are on the same subnet
2. **Verify multicast support**: Some routers block multicast traffic; check router settings
3. **Check firewall**: Ensure UDP port 1900 is not blocked
4. **Use manual IP**: Fall back to entering the IP address manually

To find your Sonos speaker IP:
- Open the Sonos app → Settings → System → About My System
- Use your router's DHCP client list
- Use a network scanner like `nmap` or Fing

### Connection Issues

If the device cannot connect to a speaker:

1. **Verify IP address**: Ensure the IP is correct and reachable
2. **Check port 1400**: Use `telnet <ip> 1400` or `nc -zv <ip> 1400` to test
3. **Restart speaker**: Power cycle the Sonos speaker
4. **Check logs**: View ESPHome logs for error messages

### Metadata Not Updating

If track information is stale:

1. **Check polling interval**: Verify `playback_refresh_interval` setting
2. **Speaker responsiveness**: The speaker may be busy or slow to respond
3. **Network latency**: High network latency can delay updates
4. **Log warnings**: Check for consecutive failure messages in logs

## Advanced Configuration

### Custom Polling Intervals

Adjust the update interval in your device YAML:

```yaml
substitutions:
  playback_refresh_interval: "2s"  # Poll every 2 seconds instead of 1
```

Lower values = more frequent updates but higher network traffic.

### Static Speaker List

For a permanent installation, define speakers in the YAML:

```yaml
substitutions:
  sonos_speakers: "Main:192.168.1.10,Patio:192.168.1.20,Office:192.168.1.30"
```

Benefit: No discovery needed, faster startup, reliable connection.

### Disable Direct Mode

To remove Sonos Direct functionality entirely:

1. Remove or comment out the `sonos_direct` and `sonos_select` includes from `base.yaml`
2. The device will use only Home Assistant mode

## Example Use Cases

### Dedicated Room Controller

Set up a device permanently connected to a specific room's speaker:

```yaml
substitutions:
  sonos_speakers: "Living Room:192.168.1.10"
```

The device acts as a wall-mounted or desk controller for that speaker.

### Multi-Room Selector

Configure all your Sonos speakers:

```yaml
substitutions:
  sonos_speakers: "Living:192.168.1.10,Kitchen:192.168.1.11,Bedroom:192.168.1.12,Office:192.168.1.13"
```

Use the dropdown to switch between rooms from the web interface.

### Auto-Discovery Setup

For dynamic environments where speaker IPs change:

1. Leave `sonos_speakers` as `"none"`
2. Use Auto-discover to find speakers
3. Select the discovered speaker you want

## Future Enhancements

Planned features for future releases:

- **Group control**: Join/leave speaker groups
- **Multiple simultaneous speakers**: Control a group as one
- **Favorites**: Quick access to Sonos favorites
- **Alarm management**: View and control Sonos alarms
- **EQ controls**: Adjust bass, treble, loudness
- **Sleep timer**: Set and control sleep timer
- **Crossfade**: Toggle crossfade setting

## See Also

- [Web Settings](webserver.md) - Configure the device from its web interface
- [Speaker Grouping](speaker-grouping.md) - Multi-room speaker control
- [Troubleshooting](../advanced/troubleshooting.md) - Common issues and solutions
