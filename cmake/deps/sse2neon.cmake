# sse2neon: single-header SSE -> NEON translation, used by the engine's SSE intrinsic code on arm64
# (xrCDB ray queries, xrParticles, xrRender ParticleEffect/DetailManager). Pinned as external/sse2neon.
#
# OpenXRay and the engine sources include it as "sse2neon/sse2neon.h", so the include root is a
# generated directory holding a link named sse2neon; the checkout's own layout stays untouched.
# Sources that include <xmmintrin.h>/<emmintrin.h>/<intrin.h> need no edit: src/Common/compat has forwarding
# headers of those names that land here (see Anomaly::EngineConfig).

set(sse2neon_include_dir "${CMAKE_BINARY_DIR}/generated/sse2neon_include")
file(MAKE_DIRECTORY "${sse2neon_include_dir}")
file(CREATE_LINK "${ANOMALY_EXTERNAL_DIR}/sse2neon" "${sse2neon_include_dir}/sse2neon" SYMBOLIC)

add_library(sse2neon INTERFACE)
target_include_directories(sse2neon SYSTEM INTERFACE "${sse2neon_include_dir}")
add_library(sse2neon::sse2neon ALIAS sse2neon)
