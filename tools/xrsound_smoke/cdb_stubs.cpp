// Stand-ins for the few xrCDB entry points xrSound links against (occlusion and environment geometry queries). Used
// only while the xrCDB module is not built; the smoke test never loads geometry.
#include "xrCore/xrCore.h"
#include "xrCDB/xrCDB.h"

namespace CDB
{
MODEL::MODEL() {}
MODEL::~MODEL() {}
void MODEL::build(Fvector*, int, TRI*, int, build_callback*, void*) {}
COLLIDER::COLLIDER() {}
COLLIDER::~COLLIDER() {}
void COLLIDER::ray_query(const MODEL*, const Fvector&, const Fvector&, float) {}
void Collector::add_face_packed_D(const Fvector&, const Fvector&, const Fvector&, u32, float) {}
} // namespace CDB
