# =============================================================================
# blender_export_level.py - Exportador de nivel Liririandi v1.7 (consolidado)
# Salvar em tools/ no repo. Uso: Text Editor do Blender > Open > Run Script.
# (Window > Toggle System Console para ver as mensagens no Windows.)
#
# CONVENCOES: MESH comum -> box (caixa envolvente em MUNDO: Edit Mode reflete)
#   prop "glb" (string)     -> linha mesh    | prop "terrain" -> linha terrain
#   EMPTY "spawn"           -> linha spawn   | prop "color"   -> cor da caixa
#   Ocultos ignorados. Rotacao nao suportada em box (aviso).
# EIXOS: engine(x,y,z) = blender(x,z,y). CONTRATO: 1 cena de nivel por .blend
# (2+ cenas corrompem o glb em silencio - issue #1690 do glTF-Blender-IO).
# =============================================================================
import bpy
import os
from mathutils import Vector
from bpy.app.handlers import persistent

EXPORT_NAME = "palco.txt"
VERSION = "v1.7"


def to_engine(v):
    return (v[0], v[2], v[1])


def fmt(f):
    return f"{f:.3f}"


def obj_color(obj):
    if "color" in obj.keys():
        c = obj["color"]
        try:
            return (float(c[0]), float(c[1]), float(c[2]))
        except (TypeError, IndexError, ValueError):
            print(f"[AVISO] '{obj.name}': prop 'color' invalida; usando material/cinza.")
    mat = obj.active_material
    if mat is not None:
        if mat.use_nodes and mat.node_tree:
            for node in mat.node_tree.nodes:
                if node.type == 'BSDF_PRINCIPLED':
                    c = node.inputs["Base Color"].default_value
                    return (c[0], c[1], c[2])
        try:
            c = mat.diffuse_color
            return (c[0], c[1], c[2])
        except Exception:
            pass
    return (0.5, 0.5, 0.5)


def find_output_path():
    blend_dir = bpy.path.abspath("//")
    if not blend_dir:
        print("[ERRO] Export ABORTADO: salve o .blend primeiro.")
        return None
    for cand in (os.path.join(blend_dir, "levels"),
                 os.path.normpath(os.path.join(blend_dir, "..", "levels"))):
        if os.path.isdir(cand):
            return os.path.join(cand, EXPORT_NAME)
    return os.path.join(blend_dir, EXPORT_NAME)


def find_assets_dir():
    blend_dir = bpy.path.abspath("//")
    if not blend_dir:
        return None
    for cand in (os.path.join(blend_dir, "assets"),
                 os.path.normpath(os.path.join(blend_dir, "..", "assets"))):
        if os.path.isdir(cand):
            return cand
    return None


_exportando_glb = False


def auto_export_terrain_glb(obj, rel_path):
    global _exportando_glb
    if _exportando_glb:
        return
    assets_dir = find_assets_dir()
    if assets_dir is None:
        print("[AVISO] pasta assets/ nao encontrada; glb NAO exportado.")
        return
    out = os.path.join(assets_dir, rel_path)
    _exportando_glb = True
    prev_sel = [o for o in bpy.context.view_layer.objects if o.select_get()]
    prev_active = bpy.context.view_layer.objects.active
    try:
        for o in bpy.context.view_layer.objects:
            o.select_set(False)
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.export_scene.gltf(filepath=out, export_format='GLB',
                                  use_selection=True, export_apply=True)
        print(f"[TERRAIN] glb auto-exportado (Apply Modifiers ON): {out}")
    except Exception as e:
        print(f"[AVISO] auto-export do glb FALHOU: {e}")
    finally:
        for o in bpy.context.view_layer.objects:
            o.select_set(False)
        for o in prev_sel:
            o.select_set(True)
        bpy.context.view_layer.objects.active = prev_active
        _exportando_glb = False


def _liri_terrain_glb_timer():
    for _o in bpy.context.view_layer.objects:
        if "terrain" in _o.keys():
            auto_export_terrain_glb(_o, str(_o["terrain"]).replace("\\", "/"))
            break
    out = find_output_path()
    if out and os.path.isfile(out):
        os.utime(out, None)
        print("[TERRAIN] txt cutucado -> hot reload rele o glb fresco")
    return None


def export_level():
    boxes, meshes, spawn = [], [], None
    warnings = 0
    tem_terrain = False

    objs = sorted(bpy.context.scene.objects, key=lambda o: o.name)
    for obj in objs:
        if not obj.visible_get():
            continue

        loc = to_engine(obj.matrix_world.translation)

        if obj.type == 'EMPTY' and obj.name.lower() == "spawn":
            spawn = loc
            continue

        if "terrain" in obj.keys():
            path = str(obj["terrain"]).replace("\\", "/")
            meshes.append(f"terrain {path} {fmt(loc[0])} {fmt(loc[1])} {fmt(loc[2])}")
            print(f"[TERRAIN] '{obj.name}' -> terrain {path}")
            tem_terrain = True
            continue

        if "glb" in obj.keys():
            path = str(obj["glb"]).replace("\\", "/")
            meshes.append(f"mesh {path} {fmt(loc[0])} {fmt(loc[1])} {fmt(loc[2])}")
            continue

        if obj.type == 'MESH':
            eul = obj.matrix_world.to_euler()
            if any(abs(a) > 1e-3 for a in eul):
                print(f"[AVISO] '{obj.name}': rotacao nao suportada - caixa envolvente inflada.")
                warnings += 1
            ws = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
            lo = (min(v.x for v in ws), min(v.y for v in ws), min(v.z for v in ws))
            hi = (max(v.x for v in ws), max(v.y for v in ws), max(v.z for v in ws))
            cen_b = tuple((lo[i] + hi[i]) * 0.5 for i in range(3))
            half_b = tuple(max((hi[i] - lo[i]) * 0.5, 0.0005) for i in range(3))
            if min(half_b) * 2.0 < 0.01:
                print(f"[AVISO] '{obj.name}': espessura quase zero - z-fighting; use cubo fino (>=0.05).")
                warnings += 1
            cx, cy, cz = to_engine(cen_b)
            hx, hy, hz = to_engine(half_b)
            r, g, b = obj_color(obj)
            boxes.append(
                f"box {fmt(cx)} {fmt(cy)} {fmt(cz)} "
                f"{fmt(hx)} {fmt(hy)} {fmt(hz)} {fmt(r)} {fmt(g)} {fmt(b)}"
            )

    if spawn is None:
        print("[AVISO] EMPTY 'spawn' ausente - usando spawn 0 2 0.")
        spawn = (0.0, 2.0, 0.0)
        warnings += 1

    out = find_output_path()
    if out is None:
        return
    lines = [f"# Exportado do Blender: {os.path.basename(bpy.data.filepath) or '(nao salvo)'}",
             f"# {len(boxes)} caixas, {len(meshes)} meshes"]
    lines += boxes + meshes
    lines.append(f"spawn {fmt(spawn[0])} {fmt(spawn[1])} {fmt(spawn[2])}")

    with open(out, "w", encoding="ascii", errors="replace") as f:
        f.write("\n".join(lines) + "\n")

    print(f"[OK] Exportado: {out} ({len(boxes)} caixas, {len(meshes)} meshes, {warnings} avisos)")

    if tem_terrain and not bpy.app.timers.is_registered(_liri_terrain_glb_timer):
        bpy.app.timers.register(_liri_terrain_glb_timer, first_interval=0.2)


@persistent
def _liri_save_handler(*args):
    export_level()


_NOMES_ANTIGOS = {"_liri_save_handler", "on_save", "save_handler", "liri_on_save"}
for _h in list(bpy.app.handlers.save_post):
    if getattr(_h, "__name__", "") in _NOMES_ANTIGOS:
        bpy.app.handlers.save_post.remove(_h)
bpy.app.handlers.save_post.append(_liri_save_handler)

print(f"[OK] EXPORTADOR {VERSION} | Auto-export ligado: Ctrl+S exporta txt + glb do terreno.")
export_level()