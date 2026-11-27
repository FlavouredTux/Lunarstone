#include <lunar/physics/PlayerCamera.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lunar::physics {

using namespace camera;

namespace {

constexpr float infinity = std::numeric_limits<float>::infinity();
// Camera.NearPlaneZ.
constexpr float nearPlaneZ = -0.1f;

// The rotation taking a to b, as an axis times its angle.
Vector3 rotationBetween(const CFrame& a, const CFrame& b)
{
    const CFrame d = b.rotation() * a.rotation().inverse();
    const float trace = d.r[0][0] + d.r[1][1] + d.r[2][2];
    const float angle = std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f));
    if (angle < 1e-6f)
        return {};
    const Vector3 axis{d.r[2][1] - d.r[1][2], d.r[0][2] - d.r[2][0], d.r[1][0] - d.r[0][1]};
    return safeUnit(axis) * angle;
}

CFrame axisAngleFrame(Vector3 w)
{
    const float angle = w.magnitude();
    if (angle < 1e-9f)
        return {};
    const Vector3 k = w / angle;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float t = 1.0f - c;
    CFrame r;
    r.r[0][0] = t * k.x * k.x + c;
    r.r[0][1] = t * k.x * k.y - s * k.z;
    r.r[0][2] = t * k.x * k.z + s * k.y;
    r.r[1][0] = t * k.x * k.y + s * k.z;
    r.r[1][1] = t * k.y * k.y + c;
    r.r[1][2] = t * k.y * k.z - s * k.x;
    r.r[2][0] = t * k.x * k.z - s * k.y;
    r.r[2][1] = t * k.y * k.z + s * k.x;
    r.r[2][2] = t * k.z * k.z + c;
    return r;
}

// ZoomController.lua stepTargetZoom.
float stepTargetZoom(float z, float dz, float lo, float hi)
{
    z = std::clamp(z + dz * (1.0f + z * zoomAcceleration), lo, hi);
    if (z < firstPersonThreshold)
        z = dz <= 0.0f ? lo : firstPersonThreshold;
    return z;
}

} // namespace

CFrame lookAt(Vector3 at, Vector3 target)
{
    const Vector3 back = safeUnit(at - target);
    Vector3 right = Vector3{0.0f, 1.0f, 0.0f}.cross(back);
    if (right.magnitude() < 1e-6f)
        right = back.y > 0.0f ? Vector3{0.0f, 0.0f, -1.0f}.cross(back) : Vector3{0.0f, 0.0f, 1.0f}.cross(back);
    right = right.unit();
    const Vector3 up = back.cross(right);
    CFrame cf(at);
    cf.r[0][0] = right.x;
    cf.r[1][0] = right.y;
    cf.r[2][0] = right.z;
    cf.r[0][1] = up.x;
    cf.r[1][1] = up.y;
    cf.r[2][1] = up.z;
    cf.r[0][2] = back.x;
    cf.r[1][2] = back.y;
    cf.r[2][2] = back.z;
    return cf;
}

float stepZoom(float distance, float delta)
{
    // BaseCamera.lua StepZoom.
    float z = distance;
    if (delta > 0.0f)
        z = std::max(z + delta * (1.0f + z * zoomCurvature), firstPersonThreshold);
    else if (delta < 0.0f)
        z = std::max((z + delta) / (1.0f - delta * zoomCurvature), firstPersonDistance);
    return z < firstPersonThreshold ? firstPersonDistance : z;
}

PlayerCamera::PlayerCamera(World& world, Camera& camera) : world_(world), camera_(camera) {}

void PlayerCamera::setZoomLimits(const Player& player)
{
    if (player.cameraMode() == Enum::CameraMode::LockFirstPerson)
        setZoomLimits(firstPersonDistance, firstPersonDistance);
    else
        setZoomLimits(player.cameraMinZoomDistance(), player.cameraMaxZoomDistance());
}

void PlayerCamera::setZoomLimits(float minDistance, float maxDistance)
{
    if (minDistance == minZoom_ && maxDistance == maxZoom_)
        return;
    minZoom_ = minDistance;
    maxZoom_ = maxDistance;
    // BaseCamera.lua OnPlayerCameraPropertyChange: the distance clamps to the new limits.
    setDistance(goal_);
}

void PlayerCamera::setSubject(Humanoid* humanoid)
{
    subject_ = humanoid;
    subjectRef_ = humanoid ? humanoid->shared_from_this() : InstancePtr();
    camera_.setCameraSubject(humanoid);
    camera_.setCameraType(Enum::CameraType::Custom);
    if (BasePart* root = humanoid ? humanoid->rootPart() : nullptr)
        look_ = (root->cframe() * CFrame::fromOrientation(initialPitch, 0.0f, 0.0f)).lookVector();
    goal_ = std::clamp(defaultDistance, minZoom_, maxZoom_);
    lastGoal_ = goal_;
    springX_ = defaultDistance;
    springV_ = 0.0f;
    lastFocusFrame_.reset();
    lastCamera_.reset();
}

void PlayerCamera::followCameraSubject()
{
    // The CameraModule reads Camera.CameraSubject every frame, so a script
    // can point it at anything (the battle royale template's bus).
    auto* humanoid = instanceCast<Humanoid>(camera_.cameraSubject());
    const InstancePtr previous = subjectRef_.lock();
    if (humanoid == previous.get()) {
        subject_ = humanoid;
        return;
    }
    // TransparencyController.SetSubject: the old character stops fading.
    if (previous && previous->parent() && transparency_.value_or(0.0f) > 0.0f)
        for (const InstancePtr& d : previous->parent()->descendants())
            if (auto* part = instanceCast<BasePart>(d.get()))
                part->setLocalTransparencyModifier(0.0f);
    subject_ = humanoid;
    subjectRef_ = humanoid ? humanoid->shared_from_this() : InstancePtr();
    transparency_.reset();
}

BasePart* PlayerCamera::subjectPart() const
{
    // Popper.lua's subjectPart: a Humanoid's root part or the part itself.
    if (subject_)
        return subject_->rootPart();
    return instanceCast<BasePart>(camera_.cameraSubject());
}

std::optional<Vector3> PlayerCamera::subjectPosition() const
{
    // BaseCamera.lua GetSubjectPosition.
    Instance* subject = camera_.cameraSubject();
    if (!subject)
        return std::nullopt;
    if (!subject_) {
        if (auto* seat = instanceCast<VehicleSeat>(subject))
            return seat->cframe().position + seat->cframe().vectorToWorldSpace(seatOffset);
        if (auto* part = instanceCast<BasePart>(subject))
            return part->cframe().position;
        if (auto* model = instanceCast<Model>(subject)) {
            if (BasePart* primary = model->primaryPart())
                return primary->cframe().position;
            return model->getBoundingBox().first.position;
        }
        return std::nullopt;
    }
    BasePart* part = subject_->rootPart();
    Vector3 offset = headOffset;
    if (subject_->health() <= 0.0f) {
        offset = {};
        if (auto* head
            = instanceCast<BasePart>(subject_->parent() ? subject_->parent()->findFirstChild("Head") : nullptr))
            part = head;
    } else if (part && subject_->rigType() == Enum::HumanoidRigType::R15) {
        // R15_HEAD_OFFSET plus the root part's growth past the standard 2 studs.
        offset = subject_->automaticScalingEnabled() ? headOffset + Vector3{0.0f, (part->size().y - 2.0f) * 0.5f, 0.0f}
                                                     : Vector3{0.0f, 2.0f, 0.0f};
    }
    if (!part)
        return std::nullopt;
    // CameraOffset is dropped while the mouse is locked.
    const Vector3 cameraOffset = mouseLocked_ ? Vector3{} : subject_->cameraOffset();
    return part->cframe().position + part->cframe().vectorToWorldSpace(offset + cameraOffset);
}

void PlayerCamera::setDistance(float distance)
{
    // BaseCamera.lua SetCameraToSubjectDistance.
    const float clamped = std::clamp(distance, minZoom_, maxZoom_);
    if (clamped < firstPersonThreshold) {
        goal_ = firstPersonDistance;
        firstPerson_ = true;
    } else {
        goal_ = clamped;
        firstPerson_ = false;
    }
}

void PlayerCamera::update(float dt, const Input& input)
{
    aspect_ = input.aspect;
    if (input.toggleMouseLock && mouseLockAllowed_)
        mouseLocked_ = !mouseLocked_;
    followCameraSubject();

    // CameraInput.getRotation: arrows always, the mouse only while it's captured.
    float yaw = (static_cast<float>(input.turnRight) - static_cast<float>(input.turnLeft)) * keyTurnSpeed * dt;
    float pitch = 0.0f;
    if (pointerLocked(input)) {
        yaw += input.mouseDeltaX * mouseYawPerPixel;
        pitch += input.mouseDeltaY * mousePitchPerPixel;
    }
    // CameraInput.getZoomDelta: wheel up zooms in.
    const float zoomDelta
        = -input.wheel + (static_cast<float>(input.zoomOut) - static_cast<float>(input.zoomIn)) * keyZoomPerFrame;
    if (zoomDelta != 0.0f)
        setDistance(stepZoom(goal_, zoomDelta));

    const std::optional<Vector3> subject = subjectPosition();
    if (!subject)
        return;
    Vector3 subjectPos = *subject;

    // BaseCamera.lua CalculateNewLookCFrameFromArg.
    const float currentPitch = std::asin(std::clamp(look_.y, -1.0f, 1.0f));
    const float yT = std::clamp(pitch, -maxPitch + currentPitch, -minPitch + currentPitch);
    const CFrame newLook = CFrame::angles(0.0f, -yaw, 0.0f) * lookAt({}, look_) * CFrame::angles(-yT, 0.0f, 0.0f);
    look_ = newLook.lookVector();

    if (mouseLocked_ && !firstPerson_) {
        const Vector3 offset = mouseLockOffset + (subject_ ? subject_->cameraOffset() : Vector3{});
        subjectPos = subjectPos + newLook.rightVector() * offset.x + newLook.upVector() * offset.y
                     + newLook.lookVector() * offset.z;
    }
    const CFrame focus(subjectPos);

    // Poppercam: the camera's rotation placed at the focus, pushed back by the zoom spring.
    CFrame rotatedFocus = lookAt(subjectPos, subjectPos + look_);
    Vector3 linear;
    Vector3 angular;
    if (lastFocusFrame_ && dt > 0.0f) {
        linear = (rotatedFocus.position - lastFocusFrame_->position) / dt;
        angular = rotationBetween(*lastFocusFrame_, rotatedFocus) / dt;
    }
    lastFocusFrame_ = rotatedFocus;

    // ZoomController.Update.
    float popped = infinity;
    if (goal_ > firstPersonThreshold) {
        const float maxPossible = std::max(springX_, stepTargetZoom(goal_, zoomDelta, minZoom_, maxZoom_));
        popped = popper(rotatedFocus * CFrame(0.0f, 0.0f, minFocusDistance), maxPossible - minFocusDistance,
                        rotatedFocus, linear, angular, dt)
                 + minFocusDistance;
    }
    // Critically damped spring towards the goal, clamped to what occlusion allows.
    const float w = zoomStiffness * 2.0f * pi;
    const float offset = goal_ - springX_;
    const float s = w * dt;
    const float decay = std::exp(-s);
    float x = goal_ + (springV_ * dt - offset * (s + 1.0f)) * decay;
    float v = ((offset * w - springV_) * s + springV_) * decay;
    const float hi = std::min(maxZoom_, popped);
    if (x < minFocusDistance || x > hi) {
        x = std::clamp(x, minFocusDistance, std::max(hi, minFocusDistance));
        v = 0.0f;
    }
    springX_ = x;
    springV_ = v;

    const CFrame cf = rotatedFocus * CFrame(0.0f, 0.0f, springX_);
    camera_.setCFrame(cf);
    camera_.setFocus(focus);
    lastCamera_ = cf;
    lastGoal_ = goal_;
    updateTransparency(dt);
}

bool PlayerCamera::canOcclude(const BasePart& part) const
{
    // Popper.lua canOcclude.
    const float transparency = 1.0f - (1.0f - part.transparency()) * (1.0f - part.localTransparencyModifier());
    if (transparency >= occlusionTransparency || part.isA("TrussPart"))
        return false;
    if (BasePart* root = subjectPart()) {
        Assembly* mine = world_.assemblyOf(root);
        if (mine && world_.assemblyOf(&part) == mine)
            return false;
    }
    return true;
}

std::pair<float, float> PlayerCamera::queryPoint(Vector3 origin, Vector3 dir, float dist,
                                                 std::optional<Vector3> lastPos) const
{
    // Popper.lua queryPoint.
    dist += nearPlaneZ;
    const Vector3 target = origin + dir * dist;
    float soft = infinity;
    float hard = infinity;
    Vector3 moving = origin;

    RaycastParams params;
    params.respectCanCollide = true;
    params.ignoreWater = true;
    if (subject_ && subject_->parent())
        params.filterDescendantsInstances.push_back(subject_->parent());

    for (int casts = 0;; ++casts) {
        const auto hit = world_.raycast(moving, target - moving, params);
        if (!hit)
            break;
        const float lim = (hit->position - origin).magnitude();
        if (casts + 1 >= queryPointCastLimit) {
            hard = lim;
        } else if (canOcclude(*hit->instance)) {
            RaycastParams only;
            only.filterType = Enum::RaycastFilterType::Include;
            only.filterDescendantsInstances = {hit->instance};
            only.respectCanCollide = true;
            only.ignoreWater = true;
            if (world_.raycast(target, hit->position - target, only)) {
                // The target is outside the part: it's in between. Promote if it
                // was already between last frame's corner and the target.
                bool promote = false;
                if (lastPos)
                    promote = world_.raycast(*lastPos, target - *lastPos, only)
                              || world_.raycast(target, *lastPos - target, only);
                if (promote)
                    hard = lim;
                else if (dist < soft)
                    soft = lim;
            } else {
                hard = lim;
            }
        }
        params.filterDescendantsInstances.push_back(hit->instance);
        moving = hit->position - dir * 1e-3f;
        if (hard < infinity)
            break;
    }
    return {soft - nearPlaneZ, hard - nearPlaneZ};
}

float PlayerCamera::popper(const CFrame& focus, float target, const CFrame& extrapolatedFrom, Vector3 linear,
                           Vector3 angular, float dt)
{
    (void)dt;
    // Popper.lua queryViewport: the four corners of the near plane.
    const float projY = 2.0f * std::tan(camera_.fieldOfView() * degrees * 0.5f);
    const float projX = aspect_ * projY;
    const Vector3 fP = focus.position;
    const Vector3 fX = focus.rightVector();
    const Vector3 fY = focus.upVector();
    const Vector3 fZ = -focus.lookVector();
    float soft = infinity;
    float hard = infinity;
    for (int viewX = 0; viewX < 2; ++viewX)
        for (int viewY = 0; viewY < 2; ++viewY) {
            const float vx = static_cast<float>(viewX) - 0.5f;
            const float vy = static_cast<float>(viewY) - 0.5f;
            const Vector3 origin = fP + (fX * (vx * projX) + fY * (vy * projY)) * nearPlaneZ;
            std::optional<Vector3> lastPos;
            if (lastCamera_)
                lastPos = lastCamera_->pointToWorldSpace(
                    Vector3{vx * projX * -nearPlaneZ, -vy * projY * -nearPlaneZ, nearPlaneZ});
            const auto [s, h] = queryPoint(origin, fZ, target, lastPos);
            soft = std::min(soft, s);
            hard = std::min(hard, h);
        }
    float dist = target;
    if (hard < dist)
        dist = hard;
    if (soft < dist) {
        // testPromotion: does the occluder stay in the way as the focus keeps moving?
        bool promote = true;
        RaycastParams params;
        params.respectCanCollide = true;
        params.ignoreWater = true;
        if (subject_ && subject_->parent())
            params.filterDescendantsInstances.push_back(subject_->parent());
        auto collisionPoint = [&](Vector3 origin, Vector3 dir) {
            auto hit = world_.raycast(origin, dir, params);
            return hit ? hit->position : origin + dir;
        };
        const float speed = linear.magnitude();
        const float maxDist = (collisionPoint(fP, linear * sampleMaxT) - fP).magnitude();
        // Lua's min(1.25, rot + 0/0) is 1.25: a still focus samples the whole horizon.
        const float horizon = speed > 0.0f ? std::min(sampleMaxT, angular.magnitude() + maxDist / speed) : sampleMaxT;
        for (int sample = 0; static_cast<float>(sample) * sampleDt <= horizon && promote; ++sample) {
            const float t = static_cast<float>(sample) * sampleDt;
            CFrame cf = axisAngleFrame(angular * t) * extrapolatedFrom.rotation();
            cf.position = extrapolatedFrom.position + linear * t;
            // Lua compares queryPoint's first result, the soft limit.
            if (queryPoint(cf.position, -cf.lookVector(), dist, std::nullopt).first >= dist)
                promote = false;
        }
        const Vector2 offsets[]{{0.4f, 0.0f}, {-0.4f, 0.0f}, {0.0f, -0.4f}, {0.0f, 0.4f}, {0.0f, 0.2f}};
        for (const Vector2 o : offsets) {
            if (!promote)
                break;
            const Vector3 pos = collisionPoint(fP, fX * o.x + fY * o.y);
            if (queryPoint(pos, safeUnit(fP + fZ * dist - pos), dist, std::nullopt).first == infinity)
                promote = false;
        }
        if (promote)
            dist = soft;
    }
    return dist;
}

void PlayerCamera::updateTransparency(float dt)
{
    // TransparencyController.lua Update.
    const float d = (camera_.focus().position - camera_.cframe().position).magnitude();
    float t = d < 2.0f ? 1.0f - (d - 0.5f) / 1.5f : 0.0f;
    if (t < 0.5f)
        t = 0.0f;
    if (transparency_ && t < 1.0f && *transparency_ < 0.95f) {
        const float step = fadeTweenRate * dt;
        t = *transparency_ + std::clamp(t - *transparency_, -step, step);
    }
    t = std::clamp(std::round(t * 100.0f) / 100.0f, 0.0f, 1.0f);
    const bool changed = !transparency_ || *transparency_ != t;
    transparency_ = t;
    if (!changed || !subject_ || !subject_->parent())
        return;
    for (const InstancePtr& d2 : subject_->parent()->descendants())
        if (auto* part = instanceCast<BasePart>(d2.get()); part && !part->findFirstAncestorOfClass("Tool"))
            part->setLocalTransparencyModifier(t);
}

} // namespace lunar::physics
