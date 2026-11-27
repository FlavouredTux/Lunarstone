#include "UnionShapes.hpp"

UnionShapes::UnionShapes()
    : loader_(std::make_shared<lunar::assets::AssetCache>(), 2, false)
{
}

void UnionShapes::attach(lunar::physics::World& world)
{
    world.setUnionGeometry([this](const lunar::PartOperation& part) { return geometry(part); });
}

void UnionShapes::update(lunar::physics::World& world)
{
    const std::uint64_t generation = loader_.generation();
    if (generation == generation_ || waiting_.empty())
        return;
    generation_ = generation;
    std::erase_if(waiting_, [&](const std::weak_ptr<const lunar::Instance>& weak) {
        const auto instance = weak.lock();
        const auto* part = dynamic_cast<const lunar::PartOperation*>(instance.get());
        if (!part)
            return true;
        if (loader_.state(part->assetId()) == lunar::assets::MeshState::Loading)
            return false;
        world.reshape(const_cast<lunar::PartOperation&>(*part));
        return true;
    });
}

std::shared_ptr<const lunar::physics::MeshGeometry> UnionShapes::geometry(const lunar::PartOperation& part)
{
    const std::string& id = part.assetId();
    if (id.empty())
        return nullptr;
    if (auto it = byAsset_.find(id); it != byAsset_.end())
        return it->second;
    const std::shared_ptr<const lunar::assets::MeshFile> file = loader_.file(id);
    if (!file) {
        if (loader_.state(id) == lunar::assets::MeshState::Failed)
            byAsset_.emplace(id, nullptr);
        else
            waiting_.push_back(part.weak_from_this());
        return nullptr;
    }
    using Kind = lunar::assets::UnionOperand::Kind;
    std::vector<lunar::physics::GeometryPiece> pieces;
    bool usable = !file->operands.empty();
    for (const lunar::assets::UnionOperand& operand : file->operands) {
        lunar::physics::ShapeKind kind = lunar::physics::ShapeKind::Box;
        switch (operand.kind) {
        case Kind::Block: break;
        case Kind::Wedge: kind = lunar::physics::ShapeKind::Wedge; break;
        case Kind::CornerWedge: kind = lunar::physics::ShapeKind::CornerWedge; break;
        case Kind::Cylinder: kind = lunar::physics::ShapeKind::Cylinder; break;
        case Kind::Ball:
        case Kind::Other: usable = false; break;
        }
        if (operand.negate)
            usable = false;
        pieces.push_back({kind, operand.cframe, operand.size});
    }
    auto geometry = usable ? lunar::physics::piecesGeometry(pieces) : nullptr;
    byAsset_.emplace(id, geometry);
    return geometry;
}
