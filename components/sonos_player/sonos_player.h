#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <string>
#include <map>
#include <vector>

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
  void set_ip(const std::string &ip) { ip_ = ip; }

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
  // Start SSDP discovery for Sonos speakers on the network
  void start_discovery();
  
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

  // Poll AVTransport (transport state + position info) and update sensors.
  void poll_av_transport();

  // Poll RenderingControl volume and update sensor.
  void poll_volume();

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

  // ----- Members -----
  std::string ip_;
  bool active_{false};

  // How often to refresh volume relative to the main poll cycle.
  // Volume is polled every volume_poll_divisor_ update() calls.
  int poll_counter_{0};
  static constexpr int kVolumePollDivisor = 5;

  // Consecutive failure counter – used to reduce log spam on unreachable hosts.
  int consecutive_failures_{0};
  static constexpr int kMaxLoggedFailures = 3;

  // Phase 3: Discovery state
  bool discovery_in_progress_{false};
  std::map<std::string, SonosSpeaker> discovered_speakers_;

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
