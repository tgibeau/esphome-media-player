#include "sonos_player.h"

#include "esphome/core/log.h"

#include <esp_http_client.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace sonos_player {

// =============================================================================
// ESP-IDF HTTP response accumulator
// =============================================================================

struct HttpResponse {
  std::string body;
};

static esp_err_t http_event_cb(esp_http_client_event_t *evt) {
  auto *resp = static_cast<HttpResponse *>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && resp != nullptr) {
    if (resp->body.size() + evt->data_len < 8192) {
      resp->body.append(static_cast<const char *>(evt->data), evt->data_len);
    }
  }
  return ESP_OK;
}

// =============================================================================
// Lifecycle
// =============================================================================

void SonosPlayer::setup() {
  ESP_LOGD(TAG, "SonosPlayer setup (ip='%s' active=%d)", ip_.c_str(), active_);
}

void SonosPlayer::update() {
  if (!active_ || ip_.empty()) return;

  poll_av_transport();

  poll_counter_++;
  if (poll_counter_ >= kVolumePollDivisor) {
    poll_counter_ = 0;
    poll_volume();
  }
}

// =============================================================================
// Dynamic IP / activation
// =============================================================================

void SonosPlayer::set_ip_dynamic(const std::string &ip) {
  if (ip == ip_) return;
  ip_ = ip;
  consecutive_failures_ = 0;
  poll_counter_ = 0;
  ESP_LOGI(TAG, "Sonos IP updated: '%s'", ip_.c_str());
}

// =============================================================================
// Playback commands
// =============================================================================

void SonosPlayer::play_pause() {
  // We need the current state to decide play vs pause.
  // A combined GetTransportInfo + conditional is not worth the complexity;
  // instead, issue Play and let the Sonos speaker toggle naturally if it is
  // already playing.  If state is known from the last poll, use it.
  // For simplicity we send Play; if playing the Sonos device will ignore it and
  // we rely on the state sensor to reflect reality.
  // A proper implementation sends Pause when state=="playing" else Play.
  // We do that here using the state_sensor cache.
  bool is_playing = state_sensor_ != nullptr &&
                    state_sensor_->has_state() &&
                    state_sensor_->state == "playing";

  if (is_playing) {
    soap_command(
        "/MediaRenderer/AVTransport/Control",
        "urn:schemas-upnp-org:service:AVTransport:1",
        "Pause",
        "<u:Pause xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID>"
        "</u:Pause>");
    ESP_LOGD(TAG, "Command: Pause");
  } else {
    soap_command(
        "/MediaRenderer/AVTransport/Control",
        "urn:schemas-upnp-org:service:AVTransport:1",
        "Play",
        "<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID>"
        "<Speed>1</Speed>"
        "</u:Play>");
    ESP_LOGD(TAG, "Command: Play");
  }
}

void SonosPlayer::next_track() {
  soap_command(
      "/MediaRenderer/AVTransport/Control",
      "urn:schemas-upnp-org:service:AVTransport:1",
      "Next",
      "<u:Next xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
      "<InstanceID>0</InstanceID>"
      "</u:Next>");
  ESP_LOGD(TAG, "Command: Next");
}

void SonosPlayer::previous_track() {
  soap_command(
      "/MediaRenderer/AVTransport/Control",
      "urn:schemas-upnp-org:service:AVTransport:1",
      "Previous",
      "<u:Previous xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
      "<InstanceID>0</InstanceID>"
      "</u:Previous>");
  ESP_LOGD(TAG, "Command: Previous");
}

void SonosPlayer::set_volume(float volume) {
  if (volume < 0.0f) volume = 0.0f;
  if (volume > 1.0f) volume = 1.0f;
  int vol_int = static_cast<int>(volume * 100.0f + 0.5f);

  char body[256];
  snprintf(body, sizeof(body),
           "<u:SetVolume xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
           "<InstanceID>0</InstanceID>"
           "<Channel>Master</Channel>"
           "<DesiredVolume>%d</DesiredVolume>"
           "</u:SetVolume>",
           vol_int);

  soap_command(
      "/MediaRenderer/RenderingControl/Control",
      "urn:schemas-upnp-org:service:RenderingControl:1",
      "SetVolume",
      body);
  ESP_LOGD(TAG, "Command: SetVolume %d%%", vol_int);
}

// =============================================================================
// Polling helpers
// =============================================================================

void SonosPlayer::poll_av_transport() {
  // ---- Transport state (playing / paused / stopped) ----
  std::string transport_resp = soap_call(
      "/MediaRenderer/AVTransport/Control",
      "urn:schemas-upnp-org:service:AVTransport:1",
      "GetTransportInfo",
      "<u:GetTransportInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
      "<InstanceID>0</InstanceID>"
      "</u:GetTransportInfo>");

  if (transport_resp.empty()) {
    consecutive_failures_++;
    if (consecutive_failures_ <= kMaxLoggedFailures) {
      ESP_LOGW(TAG, "GetTransportInfo failed (attempt %d)", consecutive_failures_);
    }
    return;
  }
  consecutive_failures_ = 0;

  std::string raw_state = extract_xml_value(transport_resp, "CurrentTransportState");
  std::string ha_state;
  if (raw_state == "PLAYING") {
    ha_state = "playing";
  } else if (raw_state == "PAUSED_PLAYBACK") {
    ha_state = "paused";
  } else if (raw_state == "STOPPED") {
    ha_state = "idle";
  } else {
    ha_state = "idle";
  }

  if (state_sensor_ != nullptr) {
    state_sensor_->publish_state(ha_state);
  }

  // ---- Position / track info ----
  std::string pos_resp = soap_call(
      "/MediaRenderer/AVTransport/Control",
      "urn:schemas-upnp-org:service:AVTransport:1",
      "GetPositionInfo",
      "<u:GetPositionInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
      "<InstanceID>0</InstanceID>"
      "</u:GetPositionInfo>");

  if (pos_resp.empty()) return;

  // Duration
  std::string duration_str = extract_xml_value(pos_resp, "TrackDuration");
  float duration = parse_duration(duration_str);
  if (duration >= 0.0f && duration_sensor_ != nullptr) {
    duration_sensor_->publish_state(duration);
  }

  // Elapsed position
  std::string reltime_str = extract_xml_value(pos_resp, "RelTime");
  float position = parse_duration(reltime_str);
  if (position >= 0.0f && position_sensor_ != nullptr) {
    position_sensor_->publish_state(position);
  }

  // Track metadata (DIDL-Lite encoded in the response)
  std::string metadata_encoded = extract_xml_value(pos_resp, "TrackMetaData");
  if (!metadata_encoded.empty() && metadata_encoded != "NOT_IMPLEMENTED") {
    std::string metadata = decode_xml_entities(metadata_encoded);

    std::string title = extract_xml_value(metadata, "title");    // dc:title
    std::string artist = extract_xml_value(metadata, "creator"); // dc:creator
    std::string art_path = extract_xml_value(metadata, "albumArtURI"); // upnp:albumArtURI

    // Decode &amp; in albumArtURI (Sonos double-encodes the query string)
    if (!art_path.empty()) {
      art_path = decode_xml_entities(art_path);
    }

    if (title_sensor_ != nullptr) {
      title_sensor_->publish_state(title);
    }
    if (artist_sensor_ != nullptr) {
      artist_sensor_->publish_state(artist);
    }
    if (artwork_sensor_ != nullptr && !art_path.empty()) {
      artwork_sensor_->publish_state(resolve_artwork_url(art_path));
    } else if (artwork_sensor_ != nullptr && art_path.empty()) {
      artwork_sensor_->publish_state("");
    }
  }
}

void SonosPlayer::poll_volume() {
  std::string resp = soap_call(
      "/MediaRenderer/RenderingControl/Control",
      "urn:schemas-upnp-org:service:RenderingControl:1",
      "GetVolume",
      "<u:GetVolume xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
      "<InstanceID>0</InstanceID>"
      "<Channel>Master</Channel>"
      "</u:GetVolume>");

  if (resp.empty()) return;

  std::string vol_str = extract_xml_value(resp, "CurrentVolume");
  if (!vol_str.empty()) {
    float vol = std::stof(vol_str) / 100.0f;
    if (volume_sensor_ != nullptr) {
      volume_sensor_->publish_state(vol);
    }
  }
}

// =============================================================================
// SOAP HTTP transport
// =============================================================================

std::string SonosPlayer::soap_call(const std::string &path,
                                    const std::string &service_ns,
                                    const std::string &action,
                                    const std::string &inner_body,
                                    int timeout_ms) {
  if (ip_.empty()) return "";

  std::string url = "http://" + ip_ + ":1400" + path;
  std::string soap_action = "\"" + service_ns + "#" + action + "\"";
  std::string body =
      "<?xml version=\"1.0\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
      " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body>" +
      inner_body +
      "</s:Body>"
      "</s:Envelope>";

  HttpResponse resp;

  esp_http_client_config_t cfg{};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = timeout_ms;
  cfg.event_handler = http_event_cb;
  cfg.user_data = &resp;
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 1024;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    ESP_LOGE(TAG, "Failed to init HTTP client for %s", url.c_str());
    return "";
  }

  esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
  esp_http_client_set_header(client, "SOAPAction", soap_action.c_str());
  esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));

  esp_err_t err = esp_http_client_perform(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    return "";
  }
  return resp.body;
}

void SonosPlayer::soap_command(const std::string &path,
                                const std::string &service_ns,
                                const std::string &action,
                                const std::string &inner_body) {
  // Commands don't need the response body; just fire and forget.
  soap_call(path, service_ns, action, inner_body, 2000);
}

// =============================================================================
// XML / DIDL helpers
// =============================================================================

std::string SonosPlayer::extract_xml_value(const std::string &xml,
                                            const std::string &tag) {
  // Try plain <tag>value</tag> first.
  {
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    size_t s = xml.find(open);
    if (s != std::string::npos) {
      s += open.size();
      size_t e = xml.find(close, s);
      if (e != std::string::npos) return xml.substr(s, e - s);
    }
  }

  // Try namespaced <prefix:tag>value</prefix:tag>.
  // Look for ":tag>" anywhere in the string.
  {
    std::string suffix = ":" + tag + ">";
    size_t pos = xml.find(suffix);
    while (pos != std::string::npos) {
      // Find the '<' that started this tag.
      size_t lt = xml.rfind('<', pos);
      if (lt == std::string::npos) break;

      // The namespace prefix is between '<' (exclusive) and ':' (exclusive).
      std::string prefix = xml.substr(lt + 1, pos - lt - 1);
      // prefix must not contain spaces or '/' (avoid matching inside attrs).
      if (prefix.find(' ') != std::string::npos ||
          prefix.find('/') != std::string::npos) {
        pos = xml.find(suffix, pos + 1);
        continue;
      }

      std::string open_tag = "<" + prefix + ":" + tag + ">";
      std::string close_tag = "</" + prefix + ":" + tag + ">";
      size_t s = lt + open_tag.size();
      size_t e = xml.find(close_tag, s);
      if (e != std::string::npos) return xml.substr(s, e - s);

      pos = xml.find(suffix, pos + 1);
    }
  }

  return "";
}

std::string SonosPlayer::decode_xml_entities(const std::string &s) {
  std::string out;
  out.reserve(s.size());

  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '&') {
      if (s.compare(i, 4, "&lt;") == 0) { out += '<'; i += 4; }
      else if (s.compare(i, 4, "&gt;") == 0) { out += '>'; i += 4; }
      else if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 5; }
      else if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 6; }
      else if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; }
      else { out += s[i++]; }
    } else {
      out += s[i++];
    }
  }
  return out;
}

float SonosPlayer::parse_duration(const std::string &s) {
  if (s.empty() || s == "NOT_IMPLEMENTED") return -1.0f;

  // Format: [H:]MM:SS  (hours optional)
  int h = 0, m = 0, sec = 0;
  int n = sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec);
  if (n == 3) return static_cast<float>(h * 3600 + m * 60 + sec);
  if (n == 2) {
    // Two fields matched as h and m; reparse as MM:SS.
    h = 0;
    if (sscanf(s.c_str(), "%d:%d", &m, &sec) == 2) {
      return static_cast<float>(m * 60 + sec);
    }
  }
  return -1.0f;
}

std::string SonosPlayer::resolve_artwork_url(const std::string &path) const {
  if (path.empty()) return "";
  // Already absolute?
  if (path.substr(0, 7) == "http://" || path.substr(0, 8) == "https://") {
    return path;
  }
  // Relative path — prefix with Sonos speaker base URL.
  return "http://" + ip_ + ":1400" + path;
}

// =============================================================================
// Phase 3: SSDP Discovery and Zone Group Topology
// =============================================================================

void SonosPlayer::start_discovery() {
  if (discovery_in_progress_) {
    ESP_LOGW(TAG, "Discovery already in progress");
    return;
  }
  
  ESP_LOGI(TAG, "Starting SSDP discovery for Sonos speakers...");
  discovery_in_progress_ = true;
  discovered_speakers_.clear();
  
  // Perform SSDP discovery (non-blocking, one-shot scan)
  perform_ssdp_discovery();
  
  discovery_in_progress_ = false;
  
  if (!discovered_speakers_.empty()) {
    ESP_LOGI(TAG, "Discovery complete: found %d speaker(s)", discovered_speakers_.size());
    
    // Query zone group topology for grouping info
    if (!ip_.empty()) {
      query_zone_group_topology();
    }
  } else {
    ESP_LOGW(TAG, "No Sonos speakers discovered on the network");
  }
}

void SonosPlayer::perform_ssdp_discovery() {
  // Create UDP socket for SSDP M-SEARCH
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create SSDP socket");
    return;
  }
  
  // Set socket timeout
  struct timeval tv;
  tv.tv_sec = 3;  // 3 second timeout
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  
  // Prepare SSDP M-SEARCH message
  const char *ssdp_msg =
      "M-SEARCH * HTTP/1.1\r\n"
      "HOST: 239.255.255.250:1900\r\n"
      "MAN: \"ssdp:discover\"\r\n"
      "MX: 2\r\n"
      "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
      "\r\n";
  
  // Send to SSDP multicast address
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1900);
  inet_pton(AF_INET, "239.255.255.250", &addr.sin_addr);
  
  if (sendto(sock, ssdp_msg, strlen(ssdp_msg), 0, 
             (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "Failed to send SSDP M-SEARCH");
    close(sock);
    return;
  }
  
  ESP_LOGD(TAG, "Sent SSDP M-SEARCH, waiting for responses...");
  
  // Receive responses
  char buffer[2048];
  struct sockaddr_in from;
  socklen_t fromlen = sizeof(from);
  
  while (true) {
    int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                      (struct sockaddr *)&from, &fromlen);
    if (len <= 0) break;  // Timeout or error
    
    buffer[len] = '\0';
    std::string response(buffer);
    
    // Extract IP address from response
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ip_str, INET_ADDRSTRLEN);
    std::string speaker_ip(ip_str);
    
    // Parse LOCATION header to get device description URL
    size_t loc_pos = response.find("LOCATION:");
    if (loc_pos == std::string::npos) {
      loc_pos = response.find("Location:");
    }
    
    if (loc_pos != std::string::npos) {
      size_t url_start = response.find("http://", loc_pos);
      if (url_start != std::string::npos) {
        size_t url_end = response.find("\r\n", url_start);
        if (url_end != std::string::npos) {
          std::string location_url = response.substr(url_start, url_end - url_start);
          
          // Extract device info from the IP
          SonosSpeaker speaker;
          speaker.ip = speaker_ip;
          speaker.name = "Sonos (" + speaker_ip + ")";  // Will be updated from device description
          speaker.uuid = speaker_ip;  // Placeholder
          speaker.is_coordinator = false;
          
          discovered_speakers_[speaker_ip] = speaker;
          ESP_LOGI(TAG, "Discovered Sonos speaker at %s", speaker_ip.c_str());
        }
      }
    }
  }
  
  close(sock);
  ESP_LOGD(TAG, "SSDP discovery scan complete");
}

void SonosPlayer::query_zone_group_topology() {
  if (ip_.empty()) return;
  
  std::string response = soap_call(
      "/ZoneGroupTopology/Control",
      "urn:schemas-upnp-org:service:ZoneGroupTopology:1",
      "GetZoneGroupState",
      "<u:GetZoneGroupState xmlns:u=\"urn:schemas-upnp-org:service:ZoneGroupTopology:1\"/>");
  
  if (response.empty()) {
    ESP_LOGW(TAG, "Failed to query zone group topology");
    return;
  }
  
  // Extract ZoneGroupState XML
  std::string zone_state = extract_xml_value(response, "ZoneGroupState");
  if (!zone_state.empty()) {
    zone_state = decode_xml_entities(zone_state);
    parse_zone_groups(zone_state);
  }
}

void SonosPlayer::parse_zone_groups(const std::string &xml) {
  ESP_LOGD(TAG, "Parsing zone groups...");
  
  // Parse ZoneGroups and ZoneGroupMember elements
  // Format: <ZoneGroups><ZoneGroup Coordinator="..." ID="...">
  //           <ZoneGroupMember UUID="..." Location="http://IP:1400/..." 
  //                           ZoneName="Room" ... />
  //         </ZoneGroup></ZoneGroups>
  
  size_t pos = 0;
  while ((pos = xml.find("<ZoneGroupMember", pos)) != std::string::npos) {
    size_t end = xml.find("/>", pos);
    if (end == std::string::npos) break;
    
    std::string member = xml.substr(pos, end - pos + 2);
    
    // Extract UUID
    size_t uuid_start = member.find("UUID=\"");
    std::string uuid;
    if (uuid_start != std::string::npos) {
      uuid_start += 6;
      size_t uuid_end = member.find("\"", uuid_start);
      if (uuid_end != std::string::npos) {
        uuid = member.substr(uuid_start, uuid_end - uuid_start);
      }
    }
    
    // Extract Location (IP)
    size_t loc_start = member.find("Location=\"http://");
    std::string member_ip;
    if (loc_start != std::string::npos) {
      loc_start += 17;  // Skip "Location=\"http://"
      size_t loc_end = member.find(":", loc_start);
      if (loc_end != std::string::npos) {
        member_ip = member.substr(loc_start, loc_end - loc_start);
      }
    }
    
    // Extract ZoneName (room name)
    size_t name_start = member.find("ZoneName=\"");
    std::string room_name;
    if (name_start != std::string::npos) {
      name_start += 10;
      size_t name_end = member.find("\"", name_start);
      if (name_end != std::string::npos) {
        room_name = member.substr(name_start, name_end - name_start);
      }
    }
    
    // Update or add speaker info
    if (!member_ip.empty()) {
      auto it = discovered_speakers_.find(member_ip);
      if (it != discovered_speakers_.end()) {
        it->second.uuid = uuid;
        it->second.room = room_name;
        it->second.name = room_name.empty() ? "Sonos (" + member_ip + ")" : room_name;
      } else {
        // Add new speaker discovered via topology
        SonosSpeaker speaker;
        speaker.ip = member_ip;
        speaker.uuid = uuid;
        speaker.room = room_name;
        speaker.name = room_name.empty() ? "Sonos (" + member_ip + ")" : room_name;
        speaker.is_coordinator = false;
        discovered_speakers_[member_ip] = speaker;
      }
      
      ESP_LOGI(TAG, "Speaker: %s (%s) - UUID: %s", 
               room_name.c_str(), member_ip.c_str(), uuid.c_str());
    }
    
    pos = end + 2;
  }
  
  // Update group members sensor if active speaker is part of a group
  if (group_members_sensor_ != nullptr && !ip_.empty()) {
    // Find the current speaker's group
    std::string group_info;
    for (const auto &pair : discovered_speakers_) {
      if (pair.first == ip_) {
        group_info = pair.second.name;
        break;
      }
    }
    
    if (!group_info.empty()) {
      group_members_sensor_->publish_state(group_info);
    }
  }
}

}  // namespace sonos_player
}  // namespace esphome
