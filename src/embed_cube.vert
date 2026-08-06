// AIO Graphics Test - offscreen embed cube vertex shader (position + color, MVP).
// Compiled to SPIR-V (embed_cube.vert.inc) by CI and used only by the Vulkan
// embed path in cube.c (aio_vk_embed_*), which renders a solid-colored cube to an
// offscreen image the shell reads back. Deliberately simple (no texture, no
// lighting) so it needs no descriptors beyond a single MVP uniform.
#version 450

layout(std140, binding = 0) uniform buf {
    mat4 MVP;
} ubuf;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inCol;

layout(location = 0) out vec3 vCol;

void main() {
    vCol = inCol;
    gl_Position = ubuf.MVP * vec4(inPos, 1.0);
}
