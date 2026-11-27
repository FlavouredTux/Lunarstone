#pragma once

// Attachments, joints, constraints and the legacy body movers. The
// properties are stored here; physics::World simulates them. Included
// through <lunar/Classes.hpp>.

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/classes/Parts.hpp>

namespace lunar {

// A point and orientation relative to its parent part (or parent attachment,
// for bones).
class Attachment : public InstanceImpl<Attachment, Instance> {
public:
    LUNAR_FIELD(CFrame, cframe, CFrame)
    LUNAR_FIELD(bool, visible, Visible, false)

    // CFrame's position, keeping the rotation.
    LUNAR_PROPERTY() Vector3 position() const { return cframe().position; }
    void setPosition(Vector3 position) { setCFrame(CFrame(position) * cframe().rotation()); }

    // CFrame in world space: relative to the parent part, or to a parent
    // attachment's WorldCFrame. Without either it's CFrame itself.
    LUNAR_PROPERTY("WorldCFrame") CFrame worldCFrame() const;
    void setWorldCFrame(CFrame world);
    LUNAR_PROPERTY() Vector3 worldPosition() const { return worldCFrame().position; }
    void setWorldPosition(Vector3 position);

    // CFrame's rotation as YXZ angles in degrees, keeping the position.
    LUNAR_PROPERTY() Vector3 orientation() const;
    void setOrientation(Vector3 degrees);
    LUNAR_PROPERTY() Vector3 worldOrientation() const;
    void setWorldOrientation(Vector3 degrees);
    // CFrame's X and Y axes. Setting either rebuilds the rotation from the
    // two: X along Axis, Z perpendicular to both, Y finishing the frame.
    LUNAR_PROPERTY() Vector3 axis() const { return cframe().rightVector(); }
    void setAxis(Vector3 axis) { setCFrame(withAxes(cframe(), axis, secondaryAxis())); }
    LUNAR_PROPERTY() Vector3 secondaryAxis() const { return cframe().upVector(); }
    void setSecondaryAxis(Vector3 secondary) { setCFrame(withAxes(cframe(), axis(), secondary)); }
    LUNAR_PROPERTY() Vector3 worldAxis() const { return worldCFrame().rightVector(); }
    void setWorldAxis(Vector3 axis) { setWorldCFrame(withAxes(worldCFrame(), axis, worldSecondaryAxis())); }
    LUNAR_PROPERTY() Vector3 worldSecondaryAxis() const { return worldCFrame().upVector(); }
    void setWorldSecondaryAxis(Vector3 secondary) { setWorldCFrame(withAxes(worldCFrame(), worldAxis(), secondary)); }

private:
    // What CFrame is relative to.
    CFrame parentFrame() const;
    // `frame` turned so its X axis is along `axis` and its Y axis as near
    // `secondary` as stays perpendicular. GUESS: unchanged if they're parallel.
    static CFrame withAxes(CFrame frame, Vector3 axis, Vector3 secondary);
};

// An attachment animated by a skinned mesh's skeleton.
class Bone final : public InstanceImpl<Bone, Attachment> {
public:
    LUNAR_FIELD(CFrame, transform, Transform)
};

// Rigid connections between two parts: Part1's CFrame is Part0.CFrame * C0 * C1:Inverse().
class JointInstance : public InstanceImpl<JointInstance, Instance, ClassKind::Abstract> {
public:
    LUNAR_REF(BasePart, part0, Part0)
    LUNAR_REF(BasePart, part1, Part1)
    LUNAR_FIELD(CFrame, c0, C0)
    LUNAR_FIELD(CFrame, c1, C1)
    LUNAR_FIELD(bool, enabled, Enabled, true)
};

class Weld final : public InstanceImpl<Weld, JointInstance> {};
class ManualWeld final : public InstanceImpl<ManualWeld, JointInstance> {};
class ManualGlue final : public InstanceImpl<ManualGlue, JointInstance> {};
class Snap final : public InstanceImpl<Snap, JointInstance> {};
class Glue final : public InstanceImpl<Glue, JointInstance> {};

class Motor : public InstanceImpl<Motor, JointInstance> {
public:
    LUNAR_FIELD(float, desiredAngle, DesiredAngle, 0.0f)
    LUNAR_FIELD(float, maxVelocity, MaxVelocity, 0.0f)
    LUNAR_FIELD(float, currentAngle, CurrentAngle, 0.0f)
};

// What character rigs are made of. Transform is the animated offset, applied between C0 and C1.
class Motor6D final : public InstanceImpl<Motor6D, Motor> {
public:
    LUNAR_FIELD(CFrame, transform, Transform)
};

// Welds two parts where they are when it becomes active.
class WeldConstraint final : public InstanceImpl<WeldConstraint, Instance> {
public:
    LUNAR_REF(BasePart, part0, Part0)
    LUNAR_REF(BasePart, part1, Part1)
    LUNAR_FIELD(bool, enabled, Enabled, true)
};

class NoCollisionConstraint final : public InstanceImpl<NoCollisionConstraint, Instance> {
public:
    LUNAR_REF(BasePart, part0, Part0)
    LUNAR_REF(BasePart, part1, Part1)
    LUNAR_FIELD(bool, enabled, Enabled, true)
};

// Physics constraints between two attachments.
class Constraint : public InstanceImpl<Constraint, Instance, ClassKind::Abstract> {
public:
    LUNAR_REF(Attachment, attachment0, Attachment0)
    LUNAR_REF(Attachment, attachment1, Attachment1)
    LUNAR_FIELD(BrickColor, color, Color, BrickColor{23})
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(bool, visible, Visible, false)

    // Both attachments set and Enabled: what the Current* properties need to
    // read anything but 0. GUESS: the client also wants both in the
    // Workspace (its "active" flag).
    bool attached() const { return enabled() && attachment0() && attachment1(); }
};

class RopeConstraint final : public InstanceImpl<RopeConstraint, Constraint> {
public:
    LUNAR_FIELD(float, length, Length, 5.0f)
    LUNAR_FIELD(float, restitution, Restitution, 0.0f)
    LUNAR_FIELD(float, thickness, Thickness, 0.1f)
    LUNAR_FIELD(bool, winchEnabled, WinchEnabled, false)
    LUNAR_FIELD(float, winchForce, WinchForce, 10000.0f)
    LUNAR_FIELD(float, winchResponsiveness, WinchResponsiveness, 45.0f)
    LUNAR_FIELD(float, winchSpeed, WinchSpeed, 2.0f)
    LUNAR_FIELD(float, winchTarget, WinchTarget, 5.0f)
    // Distance between the attachments.
    LUNAR_PROPERTY() float currentDistance() const;
};

class RodConstraint final : public InstanceImpl<RodConstraint, Constraint> {
public:
    LUNAR_FIELD(float, length, Length, 5.0f)
    LUNAR_FIELD(float, thickness, Thickness, 0.1f)
    LUNAR_FIELD(bool, limitsEnabled, LimitsEnabled, false)
    LUNAR_FIELD(float, limitAngle0, LimitAngle0, 90.0f)
    LUNAR_FIELD(float, limitAngle1, LimitAngle1, 90.0f)
    LUNAR_PROPERTY() float currentDistance() const;
};

class SpringConstraint final : public InstanceImpl<SpringConstraint, Constraint> {
public:
    LUNAR_FIELD(float, coils, Coils, 3.0f)
    LUNAR_FIELD(float, damping, Damping, 0.0f)
    LUNAR_FIELD(float, freeLength, FreeLength, 1.0f)
    LUNAR_FIELD(bool, limitsEnabled, LimitsEnabled, false)
    LUNAR_FIELD(float, maxForce, MaxForce, 1e38f)
    LUNAR_FIELD(float, maxLength, MaxLength, 5.0f)
    LUNAR_FIELD(float, minLength, MinLength, 0.0f)
    LUNAR_FIELD(float, radius, Radius, 0.4f)
    LUNAR_FIELD(float, stiffness, Stiffness, 0.0f)
    LUNAR_FIELD(float, thickness, Thickness, 0.1f)
    LUNAR_PROPERTY() float currentLength() const;
};

class BallSocketConstraint final : public InstanceImpl<BallSocketConstraint, Constraint> {
public:
    LUNAR_FIELD(bool, limitsEnabled, LimitsEnabled, false)
    LUNAR_FIELD(float, maxFrictionTorque, MaxFrictionTorque, 0.0f)
    LUNAR_FIELD(float, radius, Radius, 0.15f)
    LUNAR_FIELD(float, restitution, Restitution, 0.0f)
    LUNAR_FIELD(bool, twistLimitsEnabled, TwistLimitsEnabled, false)
    LUNAR_FIELD(float, twistLowerAngle, TwistLowerAngle, -45.0f)
    LUNAR_FIELD(float, twistUpperAngle, TwistUpperAngle, 45.0f)
    LUNAR_FIELD(float, upperAngle, UpperAngle, 45.0f)
};

class HingeConstraint final : public InstanceImpl<HingeConstraint, Constraint> {
public:
    LUNAR_FIELD(Enum::ActuatorType, actuatorType, ActuatorType, Enum::ActuatorType::None)
    LUNAR_FIELD(float, angularResponsiveness, AngularResponsiveness, 45.0f)
    LUNAR_FIELD(float, angularSpeed, AngularSpeed, 0.0f)
    LUNAR_FIELD(float, angularVelocity, AngularVelocity, 0.0f)
    LUNAR_FIELD(bool, limitsEnabled, LimitsEnabled, false)
    LUNAR_FIELD(float, lowerAngle, LowerAngle, -45.0f)
    LUNAR_FIELD(float, upperAngle, UpperAngle, 45.0f)
    LUNAR_FIELD(float, motorMaxAcceleration, MotorMaxAcceleration, 1e38f)
    LUNAR_FIELD(float, motorMaxTorque, MotorMaxTorque, 0.0f)
    LUNAR_FIELD(float, radius, Radius, 0.15f)
    LUNAR_FIELD(float, restitution, Restitution, 0.0f)
    LUNAR_FIELD(float, servoMaxTorque, ServoMaxTorque, 0.0f)
    LUNAR_FIELD(float, targetAngle, TargetAngle, 0.0f)
    // Attachment1's secondary axis from Attachment0's, about Attachment0's
    // primary axis, in degrees.
    LUNAR_PROPERTY() float currentAngle() const;
};

class SlidingBallConstraint : public InstanceImpl<SlidingBallConstraint, Constraint, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Enum::ActuatorType, actuatorType, ActuatorType, Enum::ActuatorType::None)
    LUNAR_FIELD(bool, limitsEnabled, LimitsEnabled, false)
    LUNAR_FIELD(float, linearResponsiveness, LinearResponsiveness, 45.0f)
    LUNAR_FIELD(float, lowerLimit, LowerLimit, 0.0f)
    LUNAR_FIELD(float, upperLimit, UpperLimit, 5.0f)
    LUNAR_FIELD(float, motorMaxAcceleration, MotorMaxAcceleration, 1e38f)
    LUNAR_FIELD(float, motorMaxForce, MotorMaxForce, 0.0f)
    LUNAR_FIELD(float, restitution, Restitution, 0.0f)
    LUNAR_FIELD(float, servoMaxForce, ServoMaxForce, 0.0f)
    LUNAR_FIELD(float, size, Size, 0.15f)
    LUNAR_FIELD(float, speed, Speed, 0.0f)
    LUNAR_FIELD(float, targetPosition, TargetPosition, 0.0f)
    LUNAR_FIELD(float, velocity, Velocity, 0.0f)
    // How far Attachment1 is from Attachment0 along Attachment0's primary axis.
    LUNAR_PROPERTY() float currentPosition() const;
};

class PrismaticConstraint final : public InstanceImpl<PrismaticConstraint, SlidingBallConstraint> {};

class CylindricalConstraint final : public InstanceImpl<CylindricalConstraint, SlidingBallConstraint> {
public:
    LUNAR_FIELD(Enum::ActuatorType, angularActuatorType, AngularActuatorType, Enum::ActuatorType::None)
    LUNAR_FIELD(bool, angularLimitsEnabled, AngularLimitsEnabled, false)
    LUNAR_FIELD(float, angularResponsiveness, AngularResponsiveness, 45.0f)
    LUNAR_FIELD(float, angularRestitution, AngularRestitution, 0.0f)
    LUNAR_FIELD(float, angularSpeed, AngularSpeed, 0.0f)
    LUNAR_FIELD(float, angularVelocity, AngularVelocity, 0.0f)
    LUNAR_FIELD(float, inclinationAngle, InclinationAngle, 0.0f)
    LUNAR_FIELD(float, lowerAngle, LowerAngle, -45.0f)
    LUNAR_FIELD(float, upperAngle, UpperAngle, 45.0f)
    LUNAR_FIELD(float, motorMaxAngularAcceleration, MotorMaxAngularAcceleration, 1e38f)
    LUNAR_FIELD(float, motorMaxTorque, MotorMaxTorque, 0.0f)
    LUNAR_FIELD(bool, rotationAxisVisible, RotationAxisVisible, false)
    LUNAR_FIELD(float, servoMaxTorque, ServoMaxTorque, 0.0f)
    LUNAR_FIELD(float, targetAngle, TargetAngle, 0.0f)
    // As HingeConstraint's, about the axis InclinationAngle tilts.
    LUNAR_PROPERTY() float currentAngle() const;
};

class AlignPosition final : public InstanceImpl<AlignPosition, Constraint> {
public:
    LUNAR_FIELD(Enum::PositionAlignmentMode, mode, Mode, Enum::PositionAlignmentMode::TwoAttachment)
    LUNAR_FIELD(bool, applyAtCenterOfMass, ApplyAtCenterOfMass, false)
    LUNAR_FIELD(float, maxForce, MaxForce, 10000.0f)
    LUNAR_FIELD(float, maxVelocity, MaxVelocity, 1e38f)
    LUNAR_FIELD(Vector3, position, Position)
    LUNAR_FIELD(bool, reactionForceEnabled, ReactionForceEnabled, false)
    LUNAR_FIELD(float, responsiveness, Responsiveness, 10.0f)
    LUNAR_FIELD(bool, rigidityEnabled, RigidityEnabled, false)
};

class AlignOrientation final : public InstanceImpl<AlignOrientation, Constraint> {
public:
    LUNAR_FIELD(Enum::OrientationAlignmentMode, mode, Mode, Enum::OrientationAlignmentMode::TwoAttachment)
    LUNAR_FIELD(CFrame, cframe, CFrame)
    LUNAR_FIELD(float, maxAngularVelocity, MaxAngularVelocity, 1e38f)
    LUNAR_FIELD(float, maxTorque, MaxTorque, 10000.0f)
    LUNAR_FIELD(Vector3, primaryAxis, PrimaryAxis, Vector3{1.0f, 0.0f, 0.0f})
    LUNAR_FIELD(bool, primaryAxisOnly, PrimaryAxisOnly, false)
    LUNAR_FIELD(bool, reactionTorqueEnabled, ReactionTorqueEnabled, false)
    LUNAR_FIELD(float, responsiveness, Responsiveness, 10.0f)
    LUNAR_FIELD(bool, rigidityEnabled, RigidityEnabled, false)
    LUNAR_FIELD(Vector3, secondaryAxis, SecondaryAxis, Vector3{0.0f, 1.0f, 0.0f})
};

class AnimationConstraint final : public InstanceImpl<AnimationConstraint, Constraint> {
public:
    LUNAR_FIELD(bool, isKinematic, IsKinematic, false)
    LUNAR_FIELD(float, maxForce, MaxForce, 10000.0f)
    LUNAR_FIELD(float, maxTorque, MaxTorque, 10000.0f)
    LUNAR_FIELD(CFrame, transform, Transform)
};

// Holds Attachment1's part rigidly at Attachment0 (accessories on upgraded
// avatars come held by one). avatar::solveJoints places the parts.
class RigidConstraint final : public InstanceImpl<RigidConstraint, Constraint> {
public:
    LUNAR_FIELD(bool, enableSkinning, EnableSkinning, false)
};

// Legacy movers that push their parent part.
class BodyMover : public InstanceImpl<BodyMover, Instance, ClassKind::Abstract> {};

class BodyPosition final : public InstanceImpl<BodyPosition, BodyMover> {
public:
    LUNAR_FIELD(float, d, D, 1250.0f)
    LUNAR_FIELD(Vector3, maxForce, MaxForce, Vector3{4000.0f, 4000.0f, 4000.0f})
    LUNAR_FIELD(float, p, P, 10000.0f)
    LUNAR_FIELD(Vector3, position, Position, Vector3{0.0f, 50.0f, 0.0f})
};

class BodyVelocity final : public InstanceImpl<BodyVelocity, BodyMover> {
public:
    LUNAR_FIELD(Vector3, maxForce, MaxForce, Vector3{4000.0f, 4000.0f, 4000.0f})
    LUNAR_FIELD(float, p, P, 1250.0f)
    LUNAR_FIELD(Vector3, velocity, Velocity, Vector3{0.0f, 2.0f, 0.0f})
};

class BodyAngularVelocity final : public InstanceImpl<BodyAngularVelocity, BodyMover> {
public:
    LUNAR_FIELD(Vector3, angularVelocity, AngularVelocity, Vector3{0.0f, 2.0f, 0.0f})
    LUNAR_FIELD(Vector3, maxTorque, MaxTorque, Vector3{4000.0f, 4000.0f, 4000.0f})
    LUNAR_FIELD(float, p, P, 1250.0f)
};

class BodyGyro final : public InstanceImpl<BodyGyro, BodyMover> {
public:
    LUNAR_FIELD(CFrame, cframe, CFrame)
    LUNAR_FIELD(float, d, D, 500.0f)
    LUNAR_FIELD(Vector3, maxTorque, MaxTorque, Vector3{400000.0f, 0.0f, 400000.0f})
    LUNAR_FIELD(float, p, P, 3000.0f)
};

class BodyForce final : public InstanceImpl<BodyForce, BodyMover> {
public:
    LUNAR_FIELD(Vector3, force, Force)
};

} // namespace lunar
