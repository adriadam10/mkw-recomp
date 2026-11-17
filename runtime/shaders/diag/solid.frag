// Diagnosis (MKW_FS_SOLID, vk_pipeline.c): stands in for a fragment shader and writes magenta to its first two outputs,
// so a render target that stays zero is a pipeline or attachment problem, not the shader's arithmetic.
#version 460
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
void main() { o0 = vec4(1.0, 0.0, 1.0, 1.0); o1 = vec4(1.0, 0.0, 1.0, 1.0); }
