#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <string>
#include <map>
#include <vector>
#include <atomic>
#include <mutex>

namespace esphome {
namespace sonos_player {

static const char *const TAG = "sonos_player";

// Structure to hold discovered speaker info
struct SonosSpeaker {
  std::string name;
  std::string ip;
  std::string uuid;
  std::string room;
  bool is_coordinator;
  std::vector<std::string> group_members;
};

class SonosPlayer : public PollingComponent {
 public:
  // ----- Configuration setters (called from generated code) -----
  void set_ip(const std::string &ip) {
    std::lock_guard<std::mutex> lk(ip_mutex_);
    ip_ = ip;
  }

  void set_title_sensor(text_sensor::TextSensor *s) { title_sensor_ = s; }
  void set_artist_sensor(text_sensor::TextSensor *s) { artist_sensor_ = s; }
  void set_artwork_sensor(text_sensor::TextSensor *s) { artwork_sensor_ = s; }
  void set_state_sensor(text_sensor::TextSensor *s) { state_sensor_ = s; }
  void set_source_sensor(text_sensor::TextSensor *s) { source_sensor_ = s; }
  void set_group_members_sensor(text_sensor::TextSensor *s) { group_members_sensor_ = s; }

  void set_duration_sensor(sensor::Sensor *s) { duration_sensor_ = s; }
  void set_position_sensor(sensor::Sensor *s) { position_sensor_ = s; }
  void set_volume_sensor(sensor::Sensor *s) { volume_sensor_ = s; }

  // ----- Runtime control -----
  // Called from YAML lambdas when the Sonos IP text entity changes.
  // Setting an empty string disables polling.
  void set_ip_dynamic(const std::string &ip);

  // Enable or disable this component's polling without changing the IP.
  void set_active(bool active) { active_ = active; }
  bool is_active() const { return active_; }

  // ----- Phase 3: SSDP Discovery and Grouping -----
  // Start SSDP discovery for Sonos speakers on the network (BLOCKING ~3 s –
  // only call off the main loop).
  void start_discovery();

  // Non-blocking: ask the background SOAP task to run a discovery scan. Returns
  // immediately; poll is_discovery_active() to know when results are ready.
  void request_discovery();

  // True while a background discovery scan is running.
  bool is_discovery_active() const { return discovery_active_.load(); }

  // Get list of discovered speakers
  const std::map<std::string, SonosSpeaker> &get_discovered_speakers() const { 
    return discovered_speakers_; 
  }

  // ----- Playback commands (called from YAML lambdas) -----
  void play_pause();
  void next_track();
  void previous_track();
  // volume: 0.0 – 1.0
  void set_volume(float volume);

  // ----- ESPHome component lifecycle -----
  void setup() override;
  void update() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  // Perform a SOAP call and return the raw HTTP response body.
  // Returns an empty string on failure.
  std::string soap_call(const std::string &path,
                        const std::string &service_ns,
                        const std::string &action,
                        const std::string &inner_body,
                        int timeout_ms = 3000);

  // Fire-and-forget command: no response needed.
  void soap_command(const std::string &path,
                    const std::string &service_ns,
                    const std::string &action,
                    const std::string &inner_body);

  // Result struct populated by the SOAP task and handed back to the main loop
  // via Component::defer() so all sensor writes remain on the main thread.
  struct PollResult {
    bool connected{false};
    bool have_metadata{false}; // true only when GetPositionInfo returned metadata
    std::string state;       // "playing" / "paused" / "idle"
    std::string title;
    std::string artist;
    std::string artwork_url; // empty = no artwork / unchanged
    float duration{-1.0f};   // negative = not available
    float position{-1.0f};
    float volume{-1.0f};     // negative = not polled this cycle
  };

  // Long-running FreeRTOS task: owns the persistent HTTP client and performs
  // all blocking SOAP I/O so the main loop / LVGL never stalls.
  static void soap_task_entry_(void *arg);

  // Apply a PollResult on the main loop (called from defer()).
  void apply_poll_result_(const PollResult &r);

  // ----- Phase 3: Discovery and Grouping helpers -----
  // Perform SSDP M-SEARCH for Sonos devices
  void perform_ssdp_discovery();
  
  // Query ZoneGroupTopology to get speaker grouping information
  void query_zone_group_topology();
  
  // Parse zone group state XML response
  void parse_zone_groups(const std::string &xml);

  // ----- XML / DIDL helpers -----
  // Extract the text content of the first matching tag.
  // Handles both <tag>value</tag> and <prefix:tag>value</prefix:tag>.
  static std::string extract_xml_value(const std::string &xml, const std::string &tag);

  // Decode HTML entities: &lt; &gt; &amp; &quot; &apos;
  static std::string decode_xml_entities(const std::string &s);

  // Parse "H:MM:SS" or "MM:SS" into seconds.  Returns -1 on failure.
  static float parse_duration(const std::string &s);

  // Build the full artwork URL: if path starts with '/' prefix with http://IP:1400.
  std::string resolve_artwork_url(const std::string &path) const;

  // Thread-safe snapshot of ip_.  The SOAP task and main loop both read ip_
  // while the main loop may rewrite it via set_ip_dynamic(), so all reads go
  // through this locked accessor (returns a copy that is safe to use).
  std::string get_ip_() const {
    std::lock_guard<std::mutex> lk(ip_mutex_);
    return ip_;
  }

  // ----- Members -----
  mutable std::mutex ip_mutex_;  // guards ip_ across the main loop / SOAP task
  std::string ip_;
  std::atomic<bool> active_{false};

  // SOAP is only fired every kSoapPollDivisor update() calls (1 s default).
  // Position is incremented locally each second in between.
  static constexpr int kSoapPollDivisor = 1;
  int soap_skip_counter_{0};

  // Volume is polled every kVolumePollDivisor SOAP cycles (every ~25 s).
  static constexpr int kVolumePollDivisor = 5;

  // Consecutive failure counter – written by apply_poll_result_() (main loop),
  // read by update() (main loop).  No cross-thread access.
  int consecutive_failures_{0};
  static constexpr int kMaxLoggedFailures = 3;

  // Backoff: update() skips sending task notifications when the speaker has
  // been repeatedly unreachable, preventing socket-table exhaustion.
  int backoff_skip_counter_{0};

  // FreeRTOS task handle for the long-running SOAP poll task.
  // Stored as void* to avoid pulling <freertos/task.h> into the header.
  void *soap_task_handle_{nullptr};

  // Local playback state – updated by apply_poll_result_() and used by
  // update() to advance position without a SOAP call every second.
  std::string local_state_;     // "playing" / "paused" / "idle"
  float local_position_{0.0f}; // seconds elapsed in current track
  float local_duration_{0.0f}; // total track length in seconds (0 = unknown)

  // Last-published values for sensors that rarely change.
  // apply_poll_result_() compares against these before calling publish_state()
  // so the API / HA is not flooded with duplicate values at 1-second intervals.
  std::string last_published_state_;
  std::string last_published_title_;
  std::string last_published_artist_;
  std::string last_published_artwork_;
  float last_published_duration_{-1.0f};
  float last_published_volume_{-1.0f};

  // Phase 3: Discovery state
  bool discovery_in_progress_{false};
  std::map<std::string, SonosSpeaker> discovered_speakers_;
  // Async discovery flags (set on main loop, consumed by the SOAP task).
  std::atomic<bool> discovery_requested_{false};  // a scan has been requested
  std::atomic<bool> discovery_active_{false};     // a scan is currently running

  // Sensor pointers (all optional – nullptr if not wired up in YAML)
  text_sensor::TextSensor *title_sensor_{nullptr};
  text_sensor::TextSensor *artist_sensor_{nullptr};
  text_sensor::TextSensor *artwork_sensor_{nullptr};
  text_sensor::TextSensor *state_sensor_{nullptr};
  text_sensor::TextSensor *source_sensor_{nullptr};
  text_sensor::TextSensor *group_members_sensor_{nullptr};

  sensor::Sensor *duration_sensor_{nullptr};
  sensor::Sensor *position_sensor_{nullptr};
  sensor::Sensor *volume_sensor_{nullptr};
};

}  // namespace sonos_player
}  // namespace esphome
