/*
 * AIO Graphics Test - Vulkan Phong-lit fragment shader (native VK path).
 *
 * A full Phong material (ambient + diffuse + specular) on a solid color, lit on
 * the native Vulkan path (no DXVK). The face normal is reconstructed from the
 * screen-space derivatives of the interpolated position, so it needs no extra
 * vertex attributes and reuses the cube geometry + vertex shader. Licensed
 * Apache-2.0 (see LICENSE).
 */
#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (location = 0) in vec4 texcoord;  // unused here; kept for layout compatibility
layout (location = 1) in vec3 frag_pos;
layout (location = 0) out vec4 uFragColor;

const vec3 lightDir = vec3(0.424, 0.566, 0.707);  // normalized
const vec3 baseColor = vec3(0.85, 0.28, 0.24);

float linearToSrgb(float l) {
   return (l <= 0.0031308) ? l * 12.92 : (1.055 * pow(l, 1.0 / 2.4)) - 0.055;
}
vec3 linearToSrgb(vec3 l) {
   return vec3(linearToSrgb(l.r), linearToSrgb(l.g), linearToSrgb(l.b));
}

void main() {
   vec3 N = normalize(cross(dFdx(frag_pos), dFdy(frag_pos)));
   vec3 V = vec3(0.0, 0.0, 1.0);        // view direction (toward the camera)
   if (dot(N, V) < 0.0) N = -N;         // face the viewer
   float diff = max(0.0, dot(N, lightDir));
   vec3 H = normalize(lightDir + V);
   float spec = pow(max(0.0, dot(N, H)), 48.0);
   vec3 col = baseColor * (0.15 + 0.85 * diff) + vec3(1.0) * (spec * 0.8);
   uFragColor = vec4(linearToSrgb(col), 1.0);
}
