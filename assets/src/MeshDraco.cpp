#include "MeshGeometry.hpp"

#include <lunar/assets/MeshFile.hpp>

#if LUNAR_WITH_DRACO
#include <draco/compression/decode.h>
#include <draco/core/decoder_buffer.h>
#include <draco/mesh/mesh.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#endif

namespace lunar::assets {

#if LUNAR_WITH_DRACO

bool meshDracoSupported()
{
    return true;
}

namespace detail {

namespace {

// Up to four components of one point's attribute value, as floats.
std::array<float, 4> attributeValue(const draco::PointAttribute& attribute, draco::PointIndex point)
{
    std::array<float, 4> out{};
    const auto components = static_cast<std::int8_t>(std::min<int>(attribute.num_components(), 4));
    attribute.ConvertValue<float>(attribute.mapped_index(point), components, out.data());
    return out;
}

std::uint8_t colorByte(float value, bool unitRange)
{
    const float scaled = unitRange ? value * 255.0f : value;
    if (!std::isfinite(scaled))
        return 255;
    return static_cast<std::uint8_t>(std::clamp(std::lround(scaled), 0L, 255L));
}

} // namespace

std::expected<RawGeometry, std::string> decodeDraco(std::string_view bitstream)
{
    draco::DecoderBuffer buffer;
    buffer.Init(bitstream.data(), bitstream.size());
    draco::Decoder decoder;
    auto decoded = decoder.DecodeMeshFromBuffer(&buffer);
    if (!decoded.ok())
        return std::unexpected(std::format("Draco: {}", decoded.status().error_msg_string()));
    const std::unique_ptr<draco::Mesh> mesh = std::move(decoded).value();
    if (!mesh)
        return std::unexpected("Draco: no mesh");

    const draco::PointAttribute* position = mesh->GetNamedAttribute(draco::GeometryAttribute::POSITION);
    const draco::PointAttribute* normal = mesh->GetNamedAttribute(draco::GeometryAttribute::NORMAL);
    // Roblox's encoder stores normals as a generic float3 attribute (and the
    // packed tangent as a generic byte4 one).
    for (int i = 0; !normal && i < mesh->num_attributes(); ++i) {
        const draco::PointAttribute* attribute = mesh->attribute(i);
        if (attribute->attribute_type() == draco::GeometryAttribute::GENERIC
            && attribute->data_type() == draco::DT_FLOAT32 && attribute->num_components() == 3)
            normal = attribute;
    }
    const draco::PointAttribute* uv = mesh->GetNamedAttribute(draco::GeometryAttribute::TEX_COORD);
    const draco::PointAttribute* color = mesh->GetNamedAttribute(draco::GeometryAttribute::COLOR);
    if (!position)
        return std::unexpected("Draco: no positions");
    // Normalised integer colours come out of ConvertValue in 0..1, raw ones in 0..255.
    const bool unitColor = color && (color->normalized() || color->data_type() == draco::DT_FLOAT32);

    RawGeometry raw;
    const std::uint32_t points = mesh->num_points();
    raw.vertices.resize(points);
    for (draco::PointIndex i(0); i < points; ++i) {
        RawVertex& v = raw.vertices[i.value()];
        auto p = attributeValue(*position, i);
        v.position = {p[0], p[1], p[2]};
        if (normal) {
            auto n = attributeValue(*normal, i);
            v.normal = {n[0], n[1], n[2]};
        }
        if (uv) {
            auto t = attributeValue(*uv, i);
            v.u = t[0];
            v.v = t[1];
        }
        if (color) {
            auto c = attributeValue(*color, i);
            const int components = color->num_components();
            for (int k = 0; k < std::min(components, 4); ++k)
                v.rgba[static_cast<std::size_t>(k)] = colorByte(c[static_cast<std::size_t>(k)], unitColor);
        }
    }

    raw.indices.reserve(static_cast<std::size_t>(mesh->num_faces()) * 3);
    for (draco::FaceIndex f(0); f < mesh->num_faces(); ++f) {
        const auto& face = mesh->face(f);
        for (std::size_t corner = 0; corner < 3; ++corner)
            raw.indices.push_back(face[corner].value());
    }
    return raw;
}

} // namespace detail

#else

bool meshDracoSupported()
{
    return false;
}

std::expected<detail::RawGeometry, std::string> detail::decodeDraco(std::string_view)
{
    return std::unexpected("Draco-compressed meshes need a build with LUNAR_WITH_DRACO");
}

#endif

} // namespace lunar::assets
