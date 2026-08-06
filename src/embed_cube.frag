// AIO Graphics Test - offscreen embed cube fragment shader (flat vertex color).
// See embed_cube.vert. Used only by the Vulkan embed path (aio_vk_embed_*).
#version 450

layout(location = 0) in vec3 vCol;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vCol, 1.0);
}
