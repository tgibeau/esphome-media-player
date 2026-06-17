#include "sonos_player.h"

#include "esphome/core/log.h"

#include <esp_http_client.h>
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

}  // namespace sonos_player
}  // namespace esphome
