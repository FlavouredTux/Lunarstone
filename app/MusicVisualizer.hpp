#pragma once

#include <lunar/Math.hpp>
#include <lunar/audio/Music.hpp>
#include <lunar/render/EffectGeometry.hpp>
#include <lunar/render/Renderer.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The Music tab's beat ring, Lunarstone's own: what the speakers play
// (audio::MonitorCapture, so any player: Spotify, a browser, the game)
// becomes a spectrum (audio::Spectrum), drawn as a rainbow ring of 3D
// pillars round the feet of our character that turns clockwise. Each
// pillar rises with its band under a cap that holds its peak and falls;
// a beat lifts them all, sends a ring out over the ground and makes the
// glow under them pulse. Only this client sees it.
class MusicVisualizer final : public lunar::render::EffectSource {
public:
    explicit MusicVisualizer(lunar::render::MeshRenderer& renderer);
    ~MusicVisualizer() override;
    MusicVisualizer(const MusicVisualizer&) = delete;
    MusicVisualizer& operator=(const MusicVisualizer&) = delete;

    // On listens to the speakers and shows the ring; off lets go of the sound.
    void setEnabled(bool on);
    // Once a frame: where our character stands (the ground under it), none
    // without one.
    void update(std::optional<lunar::Vector3> feet, float dt);
    // For the Music tab: listening, or why not.
    std::string status() const;

    void collect(const lunar::render::EffectView& view, lunar::render::EffectGeometry& out) override;

    static constexpr int bars = 96;
    static constexpr float radius = 10.5f;      // studs
    static constexpr float turnSeconds = 10.0f; // one clockwise turn
    static constexpr float tallest = 5.0f;      // studs, a band at full

private:
    // Bar `i`'s pillar height this frame, studs.
    float barHeight(int i) const;

    lunar::render::MeshRenderer& renderer_;
    std::unique_ptr<lunar::audio::MonitorCapture> capture_;
    lunar::audio::Spectrum spectrum_{lunar::audio::MonitorCapture::rate, bars / 2};
    std::optional<lunar::Vector3> feet_;
    double turn_ = 0.0;                  // of a full turn
    std::array<float, bars> peaks_{};    // where each cap is, studs
    std::array<float, bars> peakFall_{}; // how fast it falls, studs a second
    std::vector<float> waves_;           // seconds since each beat's ring went out
    float lastBeat_ = 0.0f;
};
