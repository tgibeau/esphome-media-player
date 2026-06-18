#include "sonos_player.h"

#include "esphome/core/log.h"

#include <esp_http_client.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstdlib>
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
    // Cap response accumulation to bound memory. The largest expected response
    // is GetZoneGroupState on a big Sonos system (~15-20 KB for ~20 zones), so
    // allow up to 64 KB. With PSRAM-backed malloc this lives in PSRAM, not the
    // scarce internal DRAM. Without a large enough cap the topology XML is
    // truncated and room names fail to parse.
    if (resp->body.size() + evt->data_len < 65536) {
      resp->body.append(static_cast<const char *>(evt->data), evt->data_len);
    }
  }
  return ESP_OK;
}

// =============================================================================
// Lifecycle
// =============================================================================

void SonosPlayer::setup() {
  ESP_LOGD(TAG, "SonosPlayer setup (ip='%s' active=%d)", ip_.c_str(), active_.load());
  // Spawn the long-running SOAP poll task.  It blocks on
  // ulTaskNotifyTake() until update() wakes it, then performs all SOAP I/O
  // without ever touching the main loop.
  xTaskCreatePinnedToCore(
      soap_task_entry_,
      "sonos_soap",
      8192,   // stack: includes url[128]+soap_action[128]+body[512] per call
      this,
      1,      // priority: same as ESPHome main loop
      reinterpret_cast<TaskHandle_t *>(&soap_task_handle_),
      1);     // pin to APP_CPU (core 1) — same as ESPHome main loop,
              // keeps core 0 (PRO_CPU/WiFi) free for the WiFi driver
}

void SonosPlayer::update() {
  if (!active_ || ip_.empty() || soap_task_handle_ == nullptr) return;

  // --- 1. Advance position locally (no SOAP required) ---
  // While the track is playing, increment the cached position by 1 s and
  // publish it so the UI stays smooth between SOAP polls.
  // update() is the SOLE publisher of position_sensor_; apply_poll_result_()
  // only syncs local_position_ to avoid double-publish / 0-flash artifacts.
  if (local_state_ == "playing") {
    local_position_ += 1.0f;
  }
  // Publish position every tick once state is known (covers paused too).
  if (position_sensor_ && !local_state_.empty()) {
    position_sensor_->publish_state(local_position_);
  }

  // --- 2. Decide whether to wake the SOAP task ---

  // Backoff: after kMaxLoggedFailures repeated failures, poll much less
  // aggressively so TIME_WAIT sockets have time to expire (~60 s).
  if (consecutive_failures_ > kMaxLoggedFailures) {
    static constexpr int kBackoffCycles = 6;
    if (++backoff_skip_counter_ < kBackoffCycles) return;
    backoff_skip_counter_ = 0;
    soap_skip_counter_ = 0; // send notification immediately after backoff
  } else {
    // Normal mode: poll every kSoapPollDivisor seconds, but trigger
    // immediately when the track is about to end (within 2 s of duration)
    // so we pick up the new track without a noticeable gap.
    bool near_end = local_duration_ > 0.0f &&
                    local_position_ >= local_duration_ - 2.0f;
    if (!near_end) {
      if (++soap_skip_counter_ < kSoapPollDivisor) return;
    }
    soap_skip_counter_ = 0;
  }

  xTaskNotifyGive(static_cast<TaskHandle_t>(soap_task_handle_));
}

// =============================================================================
// Dynamic IP / activation
// =============================================================================

void SonosPlayer::set_ip_dynamic(const std::string &ip) {
  {
    std::lock_guard<std::mutex> lk(ip_mutex_);
    if (ip == ip_) return;
    ip_ = ip;
  }
  consecutive_failures_ = 0;
  backoff_skip_counter_ = 0;
  soap_skip_counter_ = 0;
  local_state_.clear();
  local_position_ = 0.0f;
  local_duration_ = 0.0f;
  // Clear last-published cache so the new speaker's values publish immediately.
  last_published_state_.clear();
  last_published_title_.clear();
  last_published_artist_.clear();
  last_published_artwork_.clear();
  last_published_duration_ = -1.0f;
  last_published_volume_ = -1.0f;
  ESP_LOGI(TAG, "Sonos IP updated: '%s'", ip.c_str());
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
// SOAP background task + result application
// =============================================================================

// static
void SonosPlayer::soap_task_entry_(void *arg) {
  auto *self = static_cast<SonosPlayer *>(arg);

  // Persistent HTTP client – reused across poll cycles for the same IP.
  // Avoids the per-call alloc/free overhead and socket CLOSE_WAIT churn.
  esp_http_client_handle_t client = nullptr;
  std::string client_ip;
  int poll_ctr = 0;

  // Build a SOAP envelope and perform one request using the persistent client.
  // Uses stack buffers (no heap allocation) so a low-DRAM situation can never
  // trigger std::bad_alloc -> abort() inside the task.
  // Returns the response body, or "" on error (client is cleaned up on error).
  auto soap = [&](const char *path,
                  const char *service_ns,
                  const char *action,
                  const char *inner_body) -> std::string {
    // Stack-allocated buffers: no heap involvement for request setup.
    char url[128];
    snprintf(url, sizeof(url), "http://%s:1400%s", client_ip.c_str(), path);

    char soap_action[128];
    snprintf(soap_action, sizeof(soap_action), "\"%s#%s\"", service_ns, action);

    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>%s</s:Body></s:Envelope>",
        inner_body);

    HttpResponse resp;
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_user_data(client, &resp);
    esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(client, "SOAPAction", soap_action);
    esp_http_client_set_post_field(client, body, body_len);

    if (esp_http_client_perform(client) != ESP_OK) {
      esp_http_client_cleanup(client);
      client = nullptr;
      return "";
    }
    return resp.body;
  };

  while (true) {
    // Block until update() sends a notification.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // --- Async discovery request ---
    // Runs regardless of active state / current IP: SSDP needs no speaker, and
    // topology only needs one if an IP is already set. Guarded by
    // discovery_active_ so the UI only reads results once this clears.
    if (self->discovery_requested_.exchange(false)) {
      self->perform_ssdp_discovery();
      if (!self->get_ip_().empty()) {
        self->query_zone_group_topology();
      }
      self->discovery_active_ = false;
      continue;  // skip the poll cycle for this wake
    }

    const std::string ip = self->get_ip_();
    if (!self->active_ || ip.empty()) continue;

    // Recreate the persistent client when the target IP changes.
    if (client != nullptr && ip != client_ip) {
      esp_http_client_cleanup(client);
      client = nullptr;
    }

    if (client == nullptr) {
      // Initialise with any valid URL; soap() overrides it per call.
      std::string init_url = "http://" + ip + ":1400/MediaRenderer/AVTransport/Control";
      esp_http_client_config_t cfg{};
      cfg.url            = init_url.c_str();
      cfg.method         = HTTP_METHOD_POST;
      cfg.timeout_ms     = 3000;
      cfg.event_handler  = http_event_cb;
      cfg.buffer_size    = 512;
      cfg.buffer_size_tx = 512;
      // Sonos always responds with Connection: close — keep-alive is not supported.
      // Setting keep_alive_enable = false prevents the client from trying to reuse
      // a socket that Sonos has already closed, which would cause a 3-second SYN timeout.
      cfg.keep_alive_enable = false;
      client = esp_http_client_init(&cfg);
      if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to init SOAP client for %s", ip.c_str());
        auto *result = new PollResult{};
        self->defer([self, result]() {
          self->apply_poll_result_(*result);
          delete result;
        });
        continue;
      }
      client_ip = ip;
    }

    // --- GetTransportInfo ---
    std::string t_resp = soap(
        "/MediaRenderer/AVTransport/Control",
        "urn:schemas-upnp-org:service:AVTransport:1",
        "GetTransportInfo",
        "<u:GetTransportInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID></u:GetTransportInfo>");

    if (t_resp.empty()) {
      // client was already cleaned up inside soap(); post a failure result.
      auto *result = new PollResult{};
      self->defer([self, result]() {
        self->apply_poll_result_(*result);
        delete result;
      });
      continue;
    }

    // Build the result that will be applied on the main loop.
    auto *result = new PollResult{};
    result->connected = true;

    const std::string raw_state =
        self->extract_xml_value(t_resp, "CurrentTransportState");
    if (raw_state == "PLAYING")          result->state = "playing";
    else if (raw_state == "PAUSED_PLAYBACK") result->state = "paused";
    else                                     result->state = "idle";

    // --- GetPositionInfo (non-fatal if it fails) ---
    if (client != nullptr) {
      std::string p_resp = soap(
          "/MediaRenderer/AVTransport/Control",
          "urn:schemas-upnp-org:service:AVTransport:1",
          "GetPositionInfo",
          "<u:GetPositionInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
          "<InstanceID>0</InstanceID></u:GetPositionInfo>");

      if (!p_resp.empty()) {
        result->duration = self->parse_duration(
            self->extract_xml_value(p_resp, "TrackDuration"));
        result->position = self->parse_duration(
            self->extract_xml_value(p_resp, "RelTime"));

        std::string meta_enc = self->extract_xml_value(p_resp, "TrackMetaData");
        if (!meta_enc.empty() && meta_enc != "NOT_IMPLEMENTED") {
          std::string meta = self->decode_xml_entities(meta_enc);
          result->title  = self->decode_xml_entities(self->extract_xml_value(meta, "title"));
          result->artist = self->decode_xml_entities(self->extract_xml_value(meta, "creator"));
          std::string art = self->extract_xml_value(meta, "albumArtURI");
          if (!art.empty()) art = self->decode_xml_entities(art);
          result->artwork_url = self->resolve_artwork_url(art);
          // Mark that this poll carried authoritative track metadata so the
          // main loop knows it may overwrite title/artist/artwork (including
          // clearing them when a field is genuinely empty).
          result->have_metadata = true;
        }
      }
    }

    // --- GetVolume (every kVolumePollDivisor cycles) ---
    if (client != nullptr && ++poll_ctr >= kVolumePollDivisor) {
      poll_ctr = 0;
      std::string v_resp = soap(
          "/MediaRenderer/RenderingControl/Control",
          "urn:schemas-upnp-org:service:RenderingControl:1",
          "GetVolume",
          "<u:GetVolume xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
          "<InstanceID>0</InstanceID><Channel>Master</Channel></u:GetVolume>");

      if (!v_resp.empty()) {
        std::string vol_str = self->extract_xml_value(v_resp, "CurrentVolume");
        if (!vol_str.empty()) {
          // Non-throwing parse: std::stof would abort the task on malformed
          // input, and this task deliberately avoids exceptions.
          char *end = nullptr;
          float vol = strtof(vol_str.c_str(), &end);
          if (end != vol_str.c_str()) {
            result->volume = vol / 100.0f;
          }
        }
      }
    }

    // Hand result to the main loop – all sensor writes happen there.
    self->defer([self, result]() {
      self->apply_poll_result_(*result);
      delete result;
    });
  }

  // Should never reach here; clean up if it ever does.
  if (client != nullptr) esp_http_client_cleanup(client);
  vTaskDelete(nullptr);
}

void SonosPlayer::apply_poll_result_(const PollResult &r) {
  if (!r.connected) {
    consecutive_failures_++;
    if (consecutive_failures_ <= kMaxLoggedFailures) {
      ESP_LOGW(TAG, "SOAP poll failed (attempt %d)", consecutive_failures_);
    }
    return;
  }

  consecutive_failures_ = 0;

  // Sync local tracking vars from the authoritative SOAP response so the
  // per-second local increment in update() starts from the correct baseline.
  local_state_ = r.state;
  if (r.duration >= 0.0f) local_duration_ = r.duration;
  if (r.position >= 0.0f) local_position_ = r.position;

  // Only publish sensors when the value has changed to avoid flooding the
  // native API / HA with duplicate packets at 1-second poll intervals.
  if (state_sensor_ && r.state != last_published_state_) {
    last_published_state_ = r.state;
    state_sensor_->publish_state(r.state);
  }
  if (duration_sensor_ && r.duration >= 0.0f && r.duration != last_published_duration_) {
    last_published_duration_ = r.duration;
    duration_sensor_->publish_state(r.duration);
  }
  // position_sensor_ is published solely by update() to prevent double-publish
  // and the 0-flash that occurs when apply and update both fire in one tick.
  //
  // Only touch title/artist/artwork when this poll actually fetched metadata
  // (GetPositionInfo succeeded).  Otherwise a transient GetPositionInfo failure
  // would publish empty strings and momentarily blank the UI even though the
  // track has not changed.
  if (r.have_metadata) {
  if (title_sensor_ && r.title != last_published_title_) {
    last_published_title_ = r.title;
    title_sensor_->publish_state(r.title);
  }
  if (artist_sensor_ && r.artist != last_published_artist_) {
    last_published_artist_ = r.artist;
    artist_sensor_->publish_state(r.artist);
  }
  if (artwork_sensor_ && r.artwork_url != last_published_artwork_) {
    last_published_artwork_ = r.artwork_url;
    // Defer artwork URL publish to a separate main-loop callback so that
    // apply_poll_result_() returns quickly.  The artwork_image component's
    // get_local_idf_() call blocks the loop for ~300-500 ms while it opens
    // the HTTP connection to the Sonos /getaa endpoint; keeping it out of
    // apply_poll_result_() prevents the "sonos_player took a long time" warning
    // and lets title/artist appear on screen before the artwork connect starts.
    const std::string url = r.artwork_url;
    this->defer([this, url]() { artwork_sensor_->publish_state(url); });
  }
  }  // r.have_metadata
  if (volume_sensor_ && r.volume >= 0.0f && r.volume != last_published_volume_) {
    last_published_volume_ = r.volume;
    volume_sensor_->publish_state(r.volume);
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

  // Per-call response accumulator.
  HttpResponse resp;

  esp_http_client_config_t cfg{};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = timeout_ms;
  cfg.event_handler = http_event_cb;
  cfg.user_data = &resp;
  cfg.buffer_size = 512;
  cfg.buffer_size_tx = 512;
  auto *client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    ESP_LOGE(TAG, "Failed to init HTTP client for %s", url.c_str());
    return "";
  }

  esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
  esp_http_client_set_header(client, "SOAPAction", soap_action.c_str());
  esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));

  esp_err_t err = esp_http_client_perform(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) return "";
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
  return "http://" + get_ip_() + ":1400" + path;
}

// =============================================================================
// Phase 3: SSDP Discovery and Zone Group Topology
// =============================================================================

void SonosPlayer::request_discovery() {
  if (discovery_active_.load()) {
    ESP_LOGW(TAG, "Discovery already running");
    return;
  }
  // Set active first so is_discovery_active() is true the instant we return.
  discovery_active_ = true;
  discovery_requested_ = true;
  if (soap_task_handle_ != nullptr) {
    xTaskNotifyGive(static_cast<TaskHandle_t>(soap_task_handle_));
  }
  ESP_LOGI(TAG, "Async discovery requested");
}

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
