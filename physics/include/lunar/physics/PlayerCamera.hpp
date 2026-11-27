#pragma once

// Roblox's default camera (PlayerModule's CameraModule, Classic movement
// mode) for a Humanoid subject: mouse orbit while the right button is held,
// wheel zoom with the curved step and the 4.5 Hz zoom spring, first person
// below a stud, shift lock, Poppercam occlusion and the close-up character
// fade. Constants are from PlayerModule 0.742 (the Lua that ships with the
// 0.742 client), file and name next to each.

#include <lunar/Classes.hpp>
#include <lunar/physics/World.hpp>

#include <memory>
#include <numbers>
#include <optional>
#include <vector>

namespace lunar::physics {

namespace camera {
constexpr float pi = std::numbers::pi_v<float>;
constexpr float degrees = pi / 180.0f;
// CameraInput.lua ROTATION_SPEED_MOUSE = Vector2(1, 0.77) * rad(0.5), radians per pixel.
constexpr float mouseYawPerPixel = 0.5f * degrees;
constexpr float mousePitchPerPixel = 0.77f * 0.5f * degrees;
// CameraInput.lua ROTATION_SPEED_KEYS = rad(120) per second (Left/Right arrows).
constexpr float keyTurnSpeed = 120.0f * degrees;
// CameraInput.lua ZOOM_SPEED_KEYS: 0.1 per frame while I or O is held.
constexpr float keyZoomPerFrame = 0.1f;
// BaseCamera.lua.
constexpr float defaultDistance = 12.5f;        // DEFAULT_DISTANCE
constexpr float firstPersonThreshold = 1.0f;    // FIRST_PERSON_DISTANCE_THRESHOLD
constexpr float firstPersonDistance = 0.5f;     // FIRST_PERSON_DISTANCE_MIN
constexpr float zoomCurvature = 0.5f;           // ZOOM_SENSITIVITY_CURVATURE
constexpr float minPitch = -80.0f * degrees;    // MIN_Y
constexpr float maxPitch = 80.0f * degrees;     // MAX_Y
constexpr Vector3 headOffset{0.0f, 1.5f, 0.0f}; // HEAD_OFFSET / R15_HEAD_OFFSET
constexpr Vector3 seatOffset{0.0f, 5.0f, 0.0f}; // SEAT_OFFSET, a VehicleSeat subject
// ClassicCamera.lua INITIAL_CAMERA_ANGLE: 15 degrees down, behind the character.
constexpr float initialPitch = -15.0f * degrees;
// ZoomController.lua.
constexpr float zoomStiffness = 4.5f;       // ZOOM_STIFFNESS, Hz
constexpr float zoomAcceleration = 0.0375f; // ZOOM_ACCELERATION
constexpr float minFocusDistance = 0.5f;    // MIN_FOCUS_DIST
// MouseLockController.lua CAMERA_OFFSET_DEFAULT.
constexpr Vector3 mouseLockOffset{1.75f, 0.0f, 0.0f};
// Popper.lua.
constexpr float occlusionTransparency = 0.25f; // canOcclude threshold
constexpr int queryPointCastLimit = 64;        // QUERY_POINT_CAST_LIMIT
constexpr float sampleDt = 0.0625f;            // SAMPLE_DT
constexpr float sampleMaxT = 1.25f;            // SAMPLE_MAX_T
// TransparencyController.lua MAX_TWEEN_RATE, per second.
constexpr float fadeTweenRate = 2.8f;
} // namespace camera

// CFrame.lookAt(at, target) with world up, as Roblox builds it.
CFrame lookAt(Vector3 at, Vector3 target);
// The curved wheel step of BaseCamera:StepZoom: z after a zoom delta (out positive).
float stepZoom(float distance, float delta);

class PlayerCamera {
public:
    // One frame of input, already sorted by what the PlayerModule binds.
    struct Input {
        float mouseDeltaX = 0.0f;     // pixels, positive right; only counts while rotating or locked
        float mouseDeltaY = 0.0f;     // positive down
        float wheel = 0.0f;           // notches, positive away from the user (zoom in)
        bool rotate = false;          // right mouse button held
        bool turnLeft = false;        // Left arrow
        bool turnRight = false;       // Right arrow
        bool zoomIn = false;          // I
        bool zoomOut = false;         // O
        bool toggleMouseLock = false; // Shift pressed this frame (when shift lock is allowed)
        float aspect = 16.0f / 9.0f;
    };

    PlayerCamera(World& world, Camera& camera);

    // Camera.CameraSubject; looks from behind the subject, 15 degrees down.
    // Scripts may set CameraSubject to another Humanoid, a part or a model
    // later: the camera follows whatever it is.
    void setSubject(Humanoid* humanoid);
    // Player.CameraMinZoomDistance and CameraMaxZoomDistance; changes clamp the zoom.
    void setZoomLimits(float minDistance, float maxDistance);
    // The limits a player's settings give: theirs, or first person only
    // under CameraMode LockFirstPerson (BaseCamera.lua holds the zoom at
    // its first person distance then).
    void setZoomLimits(const Player& player);
    // Shift lock needs the user setting, off by default in the real client;
    // turning it off lets go of a locked mouse.
    void setMouseLockAllowed(bool allowed)
    {
        mouseLockAllowed_ = allowed;
        mouseLocked_ = mouseLocked_ && allowed;
    }

    // Updates Camera.CFrame and Focus and the subject's LocalTransparencyModifier.
    void update(float dt, const Input& input);

    // The mouse should stay put (right button held) or sit at the centre
    // (first person, shift lock): the window locks the pointer then.
    bool pointerLocked(const Input& input) const { return firstPerson_ || mouseLocked_ || input.rotate; }
    // The character should face the camera's yaw (RotationType CameraRelative).
    bool cameraRelativeRotation() const { return firstPerson_ || mouseLocked_; }
    bool firstPerson() const { return firstPerson_; }
    bool mouseLocked() const { return mouseLocked_; }
    float goalDistance() const { return goal_; }
    float zoom() const { return springX_; }
    float characterTransparency() const { return transparency_.value_or(0.0f); }

private:
    void followCameraSubject();
    BasePart* subjectPart() const;
    std::optional<Vector3> subjectPosition() const;
    void setDistance(float distance);
    float popper(const CFrame& focus, float target, const CFrame& extrapolatedFrom, Vector3 linear, Vector3 angular,
                 float dt);
    std::pair<float, float> queryPoint(Vector3 origin, Vector3 dir, float dist, std::optional<Vector3> lastPos) const;
    bool canOcclude(const BasePart& part) const;
    void updateTransparency(float dt);

    World& world_;
    Camera& camera_;
    Humanoid* subject_ = nullptr; // Camera.CameraSubject when it's a Humanoid
    std::weak_ptr<Instance> subjectRef_;
    Vector3 look_{0.0f, 0.0f, -1.0f};
    float minZoom_ = 0.5f;
    float maxZoom_ = 128.0f;
    float goal_ = camera::defaultDistance;
    float lastGoal_ = camera::defaultDistance;
    float springX_ = camera::defaultDistance;
    float springV_ = 0.0f;
    bool firstPerson_ = false;
    bool mouseLocked_ = false;
    bool mouseLockAllowed_ = false;
    float aspect_ = 16.0f / 9.0f;
    std::optional<CFrame> lastFocusFrame_;
    std::optional<CFrame> lastCamera_;
    std::optional<float> transparency_;
};

} // namespace lunar::physics
