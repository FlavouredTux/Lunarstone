// How a terrain material looks: see render::TerrainLayer.
struct TerrainLayer {
    vec4 color;  // linear material colour, w: 1 when the colour map's alpha is a tint mask
    uvec4 maps;  // texture indices: colour, normal
    vec4 tiling; // tiles per stud on tops, on sides; dominance; edge noise
    vec4 blend;  // height to blend weight: scale, offset; blend width; noise features per stud
                 // (water: ripple frame blend, swell phase and height)
};

// Per-frame values shared by the mesh shaders. Colours are linear.
layout(set = 0, binding = 0, std140) uniform Frame {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 sunDirection; // towards the sun (or the moon at night)
    vec4 sunColor;     // the client's Lamp0Color
    vec4 ambient;      // rgb: AmbientColor; w = exposure
    vec4 skyAmbient;   // SkyAmbient: what the open sky adds
    vec4 sky;
    vec4 fogColor;
    vec4 fogRange; // start, end
    mat4 shadowViewProjection;
    vec4 shadow; // enabled, texel size in uv, normal offset in studs, ShadowSoftness
    uvec4 surfaceTextures; // texture index for Studs, Inlet; 0 draws them procedurally
    // The view ray through the corners of the screen: forward plus right
    // and up scaled to the screen's half width and height at distance 1.
    vec4 skyForward;
    vec4 skyRight;
    vec4 skyUp;
    vec4 skyRotation[3]; // rows of Sky.SkyboxOrientation's rotation, world to sky
    uvec4 skyFaces0;     // texture indices of the Sky faces Rt, Lf, Up, Dn...
    uvec4 skyFaces1;     // ...Bk, Ft, then the sun and moon sprites
    vec4 skyTint;        // what the time of day multiplies the skybox by; w: star brightness
    vec4 sunSprite;      // towards the sun; w: tan of half its angular size
    vec4 moonSprite;
    vec4 skyParams;      // star count, celestial bodies shown, sun and moon brightness
    vec4 skyAverage[6];  // mean colour of each face: +X, -X, +Y, -Y, +Z, -Z
    vec4 atmosphereColor;  // rgb; w: density per stud, 0 without an Atmosphere
    vec4 atmosphereDecay;  // rgb; w: Offset
    vec4 atmosphereParams; // Glare, Haze
    vec4 backColor;          // the client's Lamp1Color: lights faces turned from the sun
    vec4 skyGradientTop;     // rgb; w: EnvironmentDiffuseScale
    vec4 skyGradientBottom;  // rgb; w: EnvironmentSpecularScale
    vec4 ambientCube[6];     // the skybox's light by world axis: +X, -X, +Y, -Y, +Z, -Z
    vec4 colorCorrection[3]; // rows of the ColorCorrection matrix, on sRGB colour
    vec4 depthParams;        // near, far plane; seconds of game time
    vec4 clouds;             // Terrain.Clouds: Cover, Density, 1 when shown
    vec4 cloudColor;         // linear
    TerrainLayer terrainLayers[23];
    // The light grid (render::LightGrid): where it is in studs (w: 1 when
    // it's on), and world x, z, y to its texture's coordinates.
    vec4 lightGridMin;
    vec4 lightGridMax;
    vec4 lightGridScale;
    vec4 quality; // x: 1 for the sharp skybox in reflections (GraphicsSettings), y: 1 for ray traced sun shadows,
                  // z: 1 for ray traced reflections, w: 1 when everything reflects
    // Local lights (lights.glsl): how many there are, 1 when they light
    // each pixel near the camera (Future, or ray traced), and from how far
    // out and how fast that fades into the light map (localLightDist,
    // hybridLerpSlope).
    vec4 localLights;
    vec4 lightClusters; // tiles across, down, pixels per tile; depth slices
    // x: 1 when the light map is on; y: the cap on ambient plus local light
    // (the client's LightConfig0.w, 0 for none); z: 1 when the shadows of
    // lights with Shadows are ray traced.
    vec4 lightMap;
    vec4 occlusion; // x: 1 for ray traced ambient occlusion (--ambient-occlusion); y: how far its rays go, studs
    vec4 tone;      // x: 1 for the client's Retro tone curve (tone.glsl), 0 for Default; y: 1 for Potato (mesh.frag)
    // x: 1 when the decal layer (decal_layer.frag) has this frame's decals on
    // solid parts in it; y, z: the specular and shininess they're lit with.
    vec4 decals;
} frame;

// Every texture, indexed per instance; 0 is white. Sampler 0 repeats, 1 clamps.
layout(constant_id = 1) const int textureCount = 4096;
layout(set = 0, binding = 2) uniform texture2D textures[textureCount];
layout(set = 0, binding = 3) uniform sampler samplers[2];

// How each instance shades, indexed by instance.
struct Shade {
    vec4 surface;     // specular, shininess, emissive, reflectance
    vec4 pattern;     // kind, features per stud, strength, studs per material texture tile
    vec4 tint;        // linear texture tint
    uvec4 maps;       // texture indices: colour, normal, roughness, metalness
    uvec4 extra;      // material colour and normal textures, flags, surfaces
    vec4 uvTransform; // scale u, v, offset u, v
    vec4 rotation;    // object to world as a quaternion, for material normal maps
};
layout(set = 0, binding = 4, std430) readonly buffer Shades {
    Shade shades[];
};

// The light grid (render::LightGrid) at a world point inside it: sky and
// sun. The sun is filtered across voxels. The sky, within each voxel, is the
// light coming in at its top (red) above what blocks it and what leaves at
// its bottom (blue) below, changing over a stud around the height its
// blockers sit at (alpha), so a point under a roof doesn't take in the sky
// the roof's own voxel still has above it. Across x and z it's filtered too.
vec2 sampleLightGrid(sampler3D grid, vec3 p)
{
    const float voxelStuds = 4.0;
    float v = p.y / voxelStuds;
    float k = floor(v);
    vec4 voxel = texture(grid, vec3(p.x, p.z, (k + 0.5) * voxelStuds) * frame.lightGridScale.xyz);
    float sun = texture(grid, p.xzy * frame.lightGridScale.xyz).g;
    float above = clamp((v - k - voxel.a) * voxelStuds + 0.5, 0.0, 1.0);
    return vec2(mix(voxel.b, voxel.r, above), sun);
}
