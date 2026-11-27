#pragma once

#include <lunar/Math.hpp>
#include <lunar/render/Mat4.hpp>

namespace lunar::render {

// What the renderer looks through. Mirrors Roblox's Camera: a CFrame looking
// along its LookVector, FieldOfView in degrees (vertical).
struct Camera {
    CFrame cframe;
    float fieldOfView = 70.0f;
    float nearPlane = 0.1f;
    float farPlane = 10000.0f;

    Mat4 view() const { return viewMatrix(cframe); }
    Mat4 projection(float aspect) const;
    Mat4 viewProjection(float aspect) const { return projection(aspect) * view(); }

    // A camera at eye whose LookVector points at target. Up is world Y unless
    // the view is vertical.
    static CFrame lookAt(Vector3 eye, Vector3 target);
};

// Free-fly ("studio") camera controller. Pure logic, fed plain input values so
// any window or test can drive it.
class FreeFlyCamera {
public:
    struct Input {
        float mouseDeltaX = 0.0f; // pixels, positive is right
        float mouseDeltaY = 0.0f; // pixels, positive is down
        bool forward = false;     // W
        bool back = false;        // S
        bool left = false;        // A
        bool right = false;       // D
        bool up = false;          // E
        bool down = false;        // Q
        bool fast = false;        // Shift
    };

    FreeFlyCamera() = default;
    explicit FreeFlyCamera(const CFrame& cframe) { setCFrame(cframe); }

    // Adopts position and direction; roll is dropped.
    void setCFrame(const CFrame& cframe);
    CFrame cframe() const;

    void update(const Input& input, float deltaSeconds);

    Vector3 position() const { return position_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }

    float speed = 32.0f;         // studs per second
    float fastMultiplier = 4.0f; // while fast is held
    float sensitivity = 0.003f;  // radians per pixel

private:
    Vector3 position_{};
    float yaw_ = 0.0f;   // around world Y, 0 looks down -Z, positive turns left
    float pitch_ = 0.0f; // positive looks up, kept just short of straight up/down
};

} // namespace lunar::render
