#pragma once

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>

namespace lunar::physics {

// PhysicalProperties.new(material): the material's own density, friction,
// elasticity and their weights.
PhysicalProperties defaultPhysicalProperties(Enum::Material material);

// How two touching surfaces combine: the weighted average Roblox documents,
// (a * wa + b * wb) / (wa + wb), for friction and elasticity alike.
inline float combineFriction(const PhysicalProperties& a, const PhysicalProperties& b)
{
    return (a.friction * a.frictionWeight + b.friction * b.frictionWeight) / (a.frictionWeight + b.frictionWeight);
}
inline float combineElasticity(const PhysicalProperties& a, const PhysicalProperties& b)
{
    return (a.elasticity * a.elasticityWeight + b.elasticity * b.elasticityWeight)
           / (a.elasticityWeight + b.elasticityWeight);
}

} // namespace lunar::physics
