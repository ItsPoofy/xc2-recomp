// Shader-pair interpolation repair. No Vulkan dependency, so synthetic SPIR-V can exercise the pass.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Shared with vk_exec_draw.c; appended after the translator's support-buffer fields.
#define VK_VARYING_VIEWPORT_OFFSET 1408u
#define VK_VARYING_SUPPORT_BYTES 1424u

// Rebuild smooth fragment inputs from the original triangle, before host clipping loses precision.
// Returns the number of repaired inputs, or 0 when the pair cannot be transformed safely.
// Successful outputs are malloc-owned. Failure leaves both outputs NULL and the inputs untouched.
int vk_varying_rewrite(const uint32_t *vs, size_t vn, const uint32_t *fs, size_t fn, unsigned locations,
                       uint32_t **vout, size_t *vwords, uint32_t **fout, size_t *fwords);
