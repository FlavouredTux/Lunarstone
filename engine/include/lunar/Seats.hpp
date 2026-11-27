#pragma once

// Seats and VehicleSeats, after the 0.742 client's RBX::SeatImpl: a seated
// Humanoid's HumanoidRootPart is held by a Weld named "SeatWeld" in the
// seat, the seat's Occupant is the Humanoid and the Humanoid's Sit and
// SeatPart say where it sits. The character controller sits a character
// down when it touches a free seat and gets it up again when it jumps.

#include <lunar/Classes.hpp>

namespace lunar {

// A Seat or a VehicleSeat.
bool isSeat(const Instance& instance);
// Seat.Disabled / VehicleSeat.Disabled.
bool seatDisabled(const BasePart& seat);
// Who sits in the seat, if anyone.
Humanoid* seatOccupant(const BasePart& seat);

// The SeatWeld the client makes (sub_101140592): Part0 the seat, Part1 the
// HumanoidRootPart. C0 is on top of the seat, C1 where the root part sits
// above it: for R15 the rig attachments down to the bottom of the left
// thigh, for R6 half the root part plus half a stud. Both are turned by
// CFrame.Angles(-pi/2, 0, 0), which cancels out.
CFrame seatWeldC0(const BasePart& seat);
CFrame seatWeldC1(const Humanoid& humanoid);
// The seat's SeatWeld holding this Humanoid's root part, if there is one.
Weld* seatWeld(const BasePart& seat, const Humanoid& humanoid);

// Seat:Sit / VehicleSeat:Sit: welds the character into the seat and sets
// Occupant, Sit and SeatPart. False if the seat is taken or the Humanoid has
// no root part. Disabled only stops sitting down on touch: vehicles keep
// their seats Disabled and seat players through Sit (the Dune Buggy's
// GetInSeat remote does).
bool sit(BasePart& seat, Humanoid& humanoid);
// Gets the Humanoid up: its SeatWeld goes, Occupant, Sit and SeatPart clear.
void leaveSeat(Humanoid& humanoid);

// What the occupant's controls put into a VehicleSeat, as the client's
// VehicleController does: forward and back (W/S) are ThrottleFloat, left
// and right (A/D) SteerFloat, -1 to 1, with Throttle and Steer their whole
// numbers. `move` is the raw control vector, -Z forward and +X right.
void driveVehicleSeat(VehicleSeat& seat, Vector3 move);

} // namespace lunar
