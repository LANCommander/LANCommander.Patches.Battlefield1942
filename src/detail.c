/* bf1942patch.meshLod / treeLod / terrainLod / terrainDetail: the level of detail of meshes, trees and terrain pushed out for today's machines (re/detail_notes.md).
 *
 * Only what decides how finely the things inside the view are drawn changes. The view distance and the fog
 * (game.setViewDistance, renderer.fogStart / fogEnd), the distance at which small objects are culled and every
 * objectTemplate.cullRadiusScale stay as the map and the server set them, so nobody sees farther or through
 * anything they could not see before.
 *
 * Meshes: the LOD selectors of the standard and the bundled meshes (0x5adfd0, 0x62b0c0, thiscall(mesh, camera,
 * &dist2) -> lod or -1 = culled) compare the squared camera distance with lodDistance[i]^2 * globalLodPercent (and
 * view factors), and cull a small object past (last lodDistance^2 + 2500) * globalLodPercent. MeshLod multiplies
 * the globalLodPercent the LOD loop reads (its 0x18c getter call), as the square of the distance factor; the cull
 * is decided by a first call with the map's own value and kept.
 *
 * Trees: GeometryTemplate.billboardDistance (the distance a TreeMesh turns into a flat billboard, 50 m in the
 * shipped objects) is multiplied as the object scripts set it (the console command's execute, vtable slot).
 *
 * Terrain (PatchTerrain, Init/Terrain.con of each map): GeometryTemplate.lodDistance (template +0xf4, at least
 * 335) sets where the terrain patches drop to their coarser levels: level = 3 * dist^2 / lodDistance^2, 0..3
 * (0x682ed0). GeometryTemplate.targetTriCount (template +0xac) is the triangle budget of the tessellation: the split
 * threshold follows it every frame (0x657600), out of a node pool of 20 x the budget made as the map loads
 * (0x658830). Both are multiplied as the map's script sets them. */
#include "bf1942patch.h"

typedef void *(__attribute__((thiscall)) *exec_fn)(void *cmd);   /* console command execute (vtable slot 18) */

#define CMD_ARGC 0x1c                /* 1 = set */
#define CMD_ARG  0x20                /* -> the parsed argument */

#define TERRAIN_LOD_SLOT   0x909448u /* GeometryTemplate.lodDistance (PatchTerrain), float */
#define TERRAIN_LOD_EXEC   0x5d2e40u
#define TERRAIN_TRIS_SLOT1 0x909030u /* GeometryTemplate.targetTriCount, int (both terrain template kinds) */
#define TERRAIN_TRIS_SLOT2 0x90a6a8u
#define TERRAIN_TRIS_EXEC  0x5d1bb0u
#define BILLBOARD_SLOT     0x906f88u /* GeometryTemplate.billboardDistance (TreeMesh), float */
#define BILLBOARD_EXEC     0x5c50a0u

#define MESH_SELECT1       0x5adfd0u
#define MESH_SELECT2       0x62b0c0u
#define RENDERER_GET_LOD   0x18c     /* renderer vtable: getGlobalLodPercent */

static float g_mesh = 1, g_billboard = 1, g_terrain_lod = 1, g_terrain_tris = 1;

static float percent(const char *name)
{
    int p = bf_setting_int(name);
    return (p < 100 ? 100 : p > 1000 ? 1000 : p) / 100.0f;
}

/* ---------------------------------------------------------------- the object scripts' values */

static void *__attribute__((thiscall)) hk_terrain_lod(void *cmd)
{
    BYTE *c = cmd;
    if (*(int *)(c + CMD_ARGC) == 1) {
        float *v = *(float **)(c + CMD_ARG);
        bf_log("detail: terrain lodDistance %.0f -> %.0f", *v, *v * g_terrain_lod);
        *v *= g_terrain_lod;
    }
    return ((exec_fn)BF_ADDR(TERRAIN_LOD_EXEC))(cmd);
}

static void *__attribute__((thiscall)) hk_terrain_tris(void *cmd)
{
    BYTE *c = cmd;
    if (*(int *)(c + CMD_ARGC) == 1) {
        int *v = *(int **)(c + CMD_ARG), n = (int)(*v * g_terrain_tris);
        n = n > 200000 ? 200000 : n;
        if (n > *v) {
            bf_log("detail: terrain targetTriCount %d -> %d", *v, n);
            *v = n;
        }
    }
    return ((exec_fn)BF_ADDR(TERRAIN_TRIS_EXEC))(cmd);
}

static void *__attribute__((thiscall)) hk_billboard(void *cmd)
{
    BYTE *c = cmd;
    if (*(int *)(c + CMD_ARGC) == 1) {
        **(float **)(c + CMD_ARG) *= g_billboard;
    }
    return ((exec_fn)BF_ADDR(BILLBOARD_EXEC))(cmd);
}

/* ---------------------------------------------------------------- mesh LOD */

typedef int (__attribute__((thiscall)) *select_fn)(void *mesh, void *camera, float *dist2);
static select_fn g_select1, g_select2;
static float g_lod_scale = 1;        /* what the LOD loop's globalLodPercent is multiplied by in this call */

/* in place of the selectors' call to renderer->getGlobalLodPercent (ecx = the renderer, result in st(0)) */
static float __attribute__((fastcall)) lod_percent(void *renderer)
{
    float (__attribute__((thiscall)) *get)(void *) = (*(void ***)renderer)[RENDERER_GET_LOD / 4];
    return get(renderer) * g_lod_scale;
}

/* the map's own LOD first: a culled object stays culled, an object at its finest level stays there; anything else
 * gets the level it has with the scaled distances */
static int select_lod(select_fn orig, void *mesh, void *camera, float *dist2)
{
    int stock = orig(mesh, camera, dist2), lod;
    if (stock <= 0) {
        return stock;
    }
    g_lod_scale = g_mesh * g_mesh;
    lod = orig(mesh, camera, dist2);
    g_lod_scale = 1;
    return lod < 0 ? stock : lod;
}

static int __attribute__((thiscall)) hk_select1(void *mesh, void *camera, float *dist2)
{
    return select_lod(g_select1, mesh, camera, dist2);
}

static int __attribute__((thiscall)) hk_select2(void *mesh, void *camera, float *dist2)
{
    return select_lod(g_select2, mesh, camera, dist2);
}

static int mesh_install(void)
{
    /* push ecx; push esi; mov esi, ecx; mov eax, [esi+0x134] */
    static const BYTE head[10] = {0x51, 0x56, 0x8b, 0xf1, 0x8b, 0x86, 0x34, 0x01, 0x00, 0x00};
    static const BYTE get_eax[6] = {0xff, 0x90, 0x8c, 0x01, 0x00, 0x00};   /* call [eax+0x18c] */
    /* the LOD loop's getter call in each selector (the cull test after it keeps the real value) */
    static const DWORD loop_calls[2] = {0x5ae0f2u, 0x62b1e2u};
    int i;
    if (!bf_expect(MESH_SELECT1, head, sizeof head) || !bf_expect(MESH_SELECT2, head, sizeof head)) {
        return 0;
    }
    for (i = 0; i < 2; i++) {
        if (!bf_expect(loop_calls[i], get_eax, sizeof get_eax)) {
            return 0;
        }
    }
    for (i = 0; i < 2; i++) {
        bf_replace_call(loop_calls[i], get_eax, sizeof get_eax, lod_percent);
    }
    g_select1 = (select_fn)bf_detour(MESH_SELECT1, head, sizeof head, hk_select1);
    g_select2 = (select_fn)bf_detour(MESH_SELECT2, head, sizeof head, hk_select2);
    return g_select1 && g_select2;
}

/* ---------------------------------------------------------------- install */

void bf_detail_install(void)
{
    g_mesh = percent("meshLod");
    g_billboard = percent("treeLod");
    g_terrain_lod = percent("terrainLod");
    g_terrain_tris = percent("terrainDetail");
    if (g_mesh > 1) {
        if (mesh_install()) {
            bf_log("detail: mesh LOD distances x%.2f", g_mesh);
        } else {
            bf_log("detail: the mesh LOD code differs, left alone");
        }
    }
    if (g_billboard > 1 && !bf_hook_slot(BILLBOARD_SLOT, BILLBOARD_EXEC, hk_billboard)) {
        bf_log("detail: GeometryTemplate.billboardDistance differs, left alone");
    }
    if (g_terrain_lod > 1 && !bf_hook_slot(TERRAIN_LOD_SLOT, TERRAIN_LOD_EXEC, hk_terrain_lod)) {
        bf_log("detail: GeometryTemplate.lodDistance differs, left alone");
    }
    if (g_terrain_tris > 1 && (!bf_hook_slot(TERRAIN_TRIS_SLOT1, TERRAIN_TRIS_EXEC, hk_terrain_tris)
                               || !bf_hook_slot(TERRAIN_TRIS_SLOT2, TERRAIN_TRIS_EXEC, hk_terrain_tris))) {
        bf_log("detail: GeometryTemplate.targetTriCount differs, left alone");
    }
    bf_log("detail: tree billboards x%.2f, terrain LOD distance x%.2f, terrain triangles x%.2f", g_billboard,
           g_terrain_lod, g_terrain_tris);
}
