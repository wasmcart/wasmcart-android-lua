/* physics_stub.c — WITH_PHYSICS=0 builds.
 *
 * runtime.c calls wcl_open_physics() unconditionally to install Box2D as the
 * Lua global `b2` (prelude.lua builds love.physics on top). Carts that never
 * touch physics — every card game — do not need it, and leaving Box2D out
 * takes a meaningful bite out of the APK.
 *
 * Note for whoever re-enables it: physics.c calls b2Body_SetMotionLocks /
 * b2MotionLocks, which do NOT exist in any released Box2D tag (v3.1.1 is the
 * newest upstream; the engine's build.sh pins a v3.2.0 that upstream never
 * published). Re-enabling means pinning a Box2D main-branch commit that has
 * that API and recording it — not just bumping the tag.
 */
struct lua_State;

void wcl_open_physics(struct lua_State *L) { (void)L; }
