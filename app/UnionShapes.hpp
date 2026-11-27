#pragma once

#include <lunar/Classes.hpp>
#include <lunar/assets/MeshLoader.hpp>
#include <lunar/physics/World.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// What a World's unions collide as when their collision data is only a hash
// (World::setUnionGeometry): the parts each was made from, read from its
// asset's ChildData. The assets load in the background; a union collides as
// its box until its asset is in, then is reshaped. Unions with a cut
// (NegateOperation), a mesh, a ball or a nested union among their parts stay
// boxes. GUESS: the genuine client's source for these shapes wasn't found;
// the parts give the same collision for unions of boxes, wedges and
// cylinders, which is what the game's walls are.
class UnionShapes {
public:
    UnionShapes();

    // Gives the world this provider. Call update once a frame after.
    void attach(lunar::physics::World& world);
    // Reshapes the unions whose assets arrived since the last call.
    void update(lunar::physics::World& world);

private:
    std::shared_ptr<const lunar::physics::MeshGeometry> geometry(const lunar::PartOperation& part);

    lunar::assets::MeshLoader loader_;
    std::unordered_map<std::string, std::shared_ptr<const lunar::physics::MeshGeometry>> byAsset_;
    std::vector<std::weak_ptr<const lunar::Instance>> waiting_; // asked while their asset loaded
    std::uint64_t generation_ = 0;
};
