// check_models.c - load each Helldrop GLB exactly the way the game will (raylib 6.2, the vendored
// static lib), print one JSON line per model and save a lit screenshot strip: idle frame 0 plus the
// LAST frame of every clip (the death one is the corpse pose).  Runs hidden: no window, no Dock tile,
// no focus (kit_background.h).  CPU skinning is forced (the shader has no bone attributes) so the
// posed vertices can be measured.
//
//   cc tools/models/check_models.c -Ivendor/raylib-src/src vendor/raylib-src/src/libraylib.mac.a \
//      -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL -framework QuartzCore \
//      -o build/check_models
//   build/check_models <out_dir> a.glb [b.glb ...]
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raylib.h"
#define KIT_BACKGROUND_IMPLEMENTATION
#include "kit_background.h"

static const char *VS =
    "#version 330\n"
    "in vec3 vertexPosition; in vec3 vertexNormal; in vec2 vertexTexCoord;\n"
    "uniform mat4 mvp; uniform mat4 matModel; uniform mat4 matNormal;\n"
    "out vec3 fragNormal; out vec2 fragUV;\n"
    "void main() { fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, 0.0))); fragUV = vertexTexCoord;\n"
    "  gl_Position = mvp*vec4(vertexPosition, 1.0); }\n";
static const char *FS =
    "#version 330\n"
    "in vec3 fragNormal; in vec2 fragUV; uniform sampler2D texture0; uniform vec4 colDiffuse; out vec4 finalColor;\n"
    "void main() { vec3 n = normalize(fragNormal);\n"
    "  vec3 base = pow(texture(texture0, fragUV).rgb, vec3(2.2))*colDiffuse.rgb;\n"
    "  float d = max(dot(n, normalize(vec3(-0.5, 0.8, 0.6))), 0.0)*1.0 + max(dot(n, normalize(vec3(0.7, 0.3, -0.4))), 0.0)*0.3;\n"
    "  finalColor = vec4(pow(base*(0.22 + d), vec3(1.0/2.2)), 1.0); }\n";

static void posed_bounds(Model m, Vector3 *mn, Vector3 *mx)
{
    *mn = (Vector3){ FLT_MAX, FLT_MAX, FLT_MAX };
    *mx = (Vector3){ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (int i = 0; i < m.meshCount; i++) {
        float *v = m.meshes[i].animVertices ? m.meshes[i].animVertices : m.meshes[i].vertices;
        for (int k = 0; k < m.meshes[i].vertexCount; k++) {
            Vector3 p = { v[3*k], v[3*k+1], v[3*k+2] };
            mn->x = fminf(mn->x, p.x); mn->y = fminf(mn->y, p.y); mn->z = fminf(mn->z, p.z);
            mx->x = fmaxf(mx->x, p.x); mx->y = fmaxf(mx->y, p.y); mx->z = fmaxf(mx->z, p.z);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <out_dir> model.glb...\n", argv[0]); return 2; }
    const char *outdir = argv[1];
    kit_background(1);
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(kit_window_flags(0, argc, argv));
    InitWindow(320, 240, "check_models");
    kit_background_apply();
    Shader sh = LoadShaderFromMemory(VS, FS);
    int bad = 0;
    for (int a = 2; a < argc; a++) {
        const char *path = argv[a];
        Model m = LoadModel(path);
        int tris = 0, verts = 0;
        for (int i = 0; i < m.meshCount; i++) { tris += m.meshes[i].triangleCount; verts += m.meshes[i].vertexCount; }
        Texture2D tx = m.meshCount ? m.materials[m.meshMaterial[0]].maps[MATERIAL_MAP_ALBEDO].texture : (Texture2D){ 0 };
        for (int i = 0; i < m.materialCount; i++) m.materials[i].shader = sh;
        for (int i = 0; i < m.materialCount; i++) {
            Texture2D t = m.materials[i].maps[MATERIAL_MAP_ALBEDO].texture;
            if (t.id > 1) { GenTextureMipmaps(&m.materials[i].maps[MATERIAL_MAP_ALBEDO].texture);
                SetTextureFilter(m.materials[i].maps[MATERIAL_MAP_ALBEDO].texture, TEXTURE_FILTER_TRILINEAR); }
        }
        int na = 0;
        ModelAnimation *an = LoadModelAnimations(path, &na);
        Vector3 rmn, rmx;
        posed_bounds(m, &rmn, &rmx);
        printf("{\"file\":\"%s\",\"meshes\":%d,\"materials\":%d,\"verts\":%d,\"tris\":%d,\"tex\":[%d,%d],\"bones\":%d,"
               "\"rest\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f],\"anims\":{",
               path, m.meshCount, m.materialCount, verts, tris, tx.width, tx.height, m.skeleton.boneCount,
               rmn.x, rmn.y, rmn.z, rmx.x, rmx.y, rmx.z);
        // strip: rest/idle0 + last frame of each clip
        int cells = 1 + na, CW = 300, CH = 360;
        RenderTexture2D rt = LoadRenderTexture(CW * cells, CH);
        float h = rmx.y - rmn.y, wdt = fmaxf(rmx.x - rmn.x, rmx.z - rmn.z), span = fmaxf(h, wdt) * 1.25f;
        Camera3D cam = { 0 };
        cam.up = (Vector3){ 0, 1, 0 };
        cam.projection = CAMERA_ORTHOGRAPHIC;
        cam.fovy = span;
        BeginTextureMode(rt);
        ClearBackground((Color){ 52, 54, 60, 255 });
        EndTextureMode();
        for (int c = 0; c < cells; c++) {
            Vector3 mn = rmn, mx = rmx;
            if (c == 0 && na > 0) UpdateModelAnimation(m, an[0], 0);
            if (c > 0) {
                ModelAnimation A = an[c - 1];
                Vector3 m0, x0;
                UpdateModelAnimation(m, A, 0);
                posed_bounds(m, &m0, &x0);
                UpdateModelAnimation(m, A, (float)(A.keyframeCount - 1));
                posed_bounds(m, &mn, &mx);
                printf("%s\"%s\":{\"frames\":%d,\"valid\":%d,\"first\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f],"
                       "\"last\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]}", c > 1 ? "," : "", A.name, A.keyframeCount,
                       IsModelAnimationValid(m, A) ? 1 : 0, m0.x, m0.y, m0.z, x0.x, x0.y, x0.z, mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
            }
            Vector3 mid = { (rmn.x + rmx.x) / 2, (rmn.y + rmx.y) / 2, (rmn.z + rmx.z) / 2 };
            float dist = span * 3;
            cam.target = mid;
            cam.position = (Vector3){ mid.x + dist * 0.62f, mid.y + dist * 0.55f, mid.z + dist * 0.62f };
            RenderTexture2D cell = LoadRenderTexture(CW, CH);
            BeginTextureMode(cell);
            ClearBackground((Color){ 52, 54, 60, 255 });
            BeginMode3D(cam);
            DrawGrid(10, span / 5);
            DrawModel(m, (Vector3){ 0, 0, 0 }, 1.0f, WHITE);
            EndMode3D();
            DrawText(c == 0 ? "rest/idle0" : an[c - 1].name, 6, 6, 20, RAYWHITE);
            EndTextureMode();
            BeginTextureMode(rt);
            DrawTextureRec(cell.texture, (Rectangle){ 0, 0, (float)CW, (float)-CH }, (Vector2){ (float)(c * CW), 0 }, WHITE);
            EndTextureMode();
            UnloadRenderTexture(cell);
        }
        printf("},\"animCount\":%d}\n", na);
        Image img = LoadImageFromTexture(rt.texture);
        ImageFlipVertical(&img);
        const char *base = GetFileNameWithoutExt(path);
        char out[1024];
        snprintf(out, sizeof out, "%s/%s_raylib.png", outdir, base);
        ExportImage(img, out);
        UnloadImage(img);
        UnloadRenderTexture(rt);
        if (na) UnloadModelAnimations(an, na);
        UnloadModel(m);
        if (m.meshCount == 0) bad++;
        fflush(stdout);
    }
    UnloadShader(sh);
    CloseWindow();
    return bad ? 1 : 0;
}
