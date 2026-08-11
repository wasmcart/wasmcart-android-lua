/* physics_stub.c — WITH_PHYSICS=0 builds.
 *
 * runtime.c calls wcl_open_physics() unconditionally to install Box2D as the
 * Lua global `b2` (prelude.lua builds love.physics on top). Carts that never
 * touch physics — every card game — do not need it, and leaving Box2D out
 * takes a meaningful bite out of the APK.
 *
 * WITH_PHYSICS=1 builds them in for real: Box2D and Box3D, both pinned by
 * SHA (no released Box2D tag has the API physics.c calls, and Box3D has no
 * releases at all), with host SIMD and the shared worker pool.
 */
struct lua_State;

/* Both openers must exist: runtime.c calls them unconditionally, and a
 * cart that never touches physics should not drag Box2D and Box3D into
 * the binary just to satisfy a symbol. */
void wcl_open_physics(struct lua_State *L)   { (void)L; }
void wcl_open_physics3d(struct lua_State *L) { (void)L; }
