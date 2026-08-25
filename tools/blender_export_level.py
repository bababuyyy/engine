# =============================================================================
# blender_export_level.py — Exportador de nivel Liririandi v0 (padrao "Dropper")
# Salvar em tools/ no repo. Uso: abrir no Text Editor do Blender > Run Script.
# (Window > Toggle System Console para ver avisos no Windows.)
#
# CONVENCOES DE AUTORIA:
#   - Objeto MESH comum           -> linha `box` (usa posicao + dimensions/2)
#   - Objeto com prop custom "glb" (string, ex. "models/poco.glb")
#                                 -> linha `mesh` na posicao do objeto
#   - EMPTY chamado "spawn"       -> linha `spawn`
#   - Prop custom "color" (3 floats 0-1) em box -> cor; senao base color do
#     material; senao cinza 0.5
#   - Objetos OCULTOS no viewport sao IGNORADOS (esconda rascunhos)
#   - ROTACAO NAO E SUPORTADA pelo formato box do engine: rotacao != 0 gera
#     AVISO com o nome do objeto e e ignorada (asse a rotacao na malha se
#     precisar, ou use glb)
#
# EIXOS: Blender = Z-up destro; engine = Y-up canhoto (LH_ZO).
#   Conversao: engine(x, y, z) = blender(x, z, y)   [swap y<->z]
#   Teste de sanidade: objeto ao NORTE (+Y do Blender) deve aparecer em +Z
#   no engine.
#
# SAIDA: procura a pasta `levels/` ao lado do .blend ou um nivel acima
# (layout do repo); senao escreve ao lado do .blend. Ordem deterministica
# (por nome), floats com 3 casas — diffs limpos no git.
# =============================================================================
import bpy
import os

EXPORT_NAME = "hearthbell.txt"   # <- mude aqui por cena, se quiser


def to_engine(v):
    """Blender Z-up RH -> engine Y-up LH: (x, y, z) -> (x, z, y)."""
    return (v[0], v[2], v[1])


def fmt(f):
    return f"{f:.3f}"


def obj_color(obj):
    if "color" in obj.keys():
        c = obj["color"]
        try:
            return (float(c[0]), float(c[1]), float(c[2]))
        except (TypeError, IndexError, ValueError):
            print(f"[AVISO] '{obj.name}': prop 'color' invalida (esperado 3 floats); usando material/cinza.")
    mat = obj.active_material
    if mat is not None:
        try:
            c = mat.diffuse_color
            return (c[0], c[1], c[2])
        except Exception:
            pass
    return (0.5, 0.5, 0.5)


def find_output_path():
    blend_dir = bpy.path.abspath("//")
    if not blend_dir:
        blend_dir = os.getcwd()
        print("[AVISO] .blend nao salvo; exportando no diretorio de trabalho.")
    for cand in (os.path.join(blend_dir, "levels"),
                 os.path.normpath(os.path.join(blend_dir, "..", "levels"))):
        if os.path.isdir(cand):
            return os.path.join(cand, EXPORT_NAME)
    return os.path.join(blend_dir, EXPORT_NAME)


def export_level():
    boxes, meshes, spawn = [], [], None
    warnings = 0

    objs = sorted(bpy.context.scene.objects, key=lambda o: o.name)
    for obj in objs:
        if not obj.visible_get():
            continue

        loc = to_engine(obj.matrix_world.translation)

        # spawn: EMPTY chamado exatamente "spawn"
        if obj.type == 'EMPTY' and obj.name.lower() == "spawn":
            spawn = loc
            continue

        # mesh glb: qualquer objeto com a prop "glb"
        if "glb" in obj.keys():
            path = str(obj["glb"]).replace("\\", "/")
            meshes.append(f"mesh {path} {fmt(loc[0])} {fmt(loc[1])} {fmt(loc[2])}")
            continue

        # box: objetos MESH comuns
        if obj.type == 'MESH':
            eul = obj.matrix_world.to_euler()
            if any(abs(a) > 1e-3 for a in eul):
                print(f"[AVISO] '{obj.name}': rotacao ignorada (formato box nao suporta rotacao).")
                warnings += 1
            d = obj.dimensions  # dims em espaco de mundo (inclui scale)
            hx, hy, hz = to_engine((d[0] / 2.0, d[1] / 2.0, d[2] / 2.0))
            r, g, b = obj_color(obj)
            boxes.append(
                f"box {fmt(loc[0])} {fmt(loc[1])} {fmt(loc[2])} "
                f"{fmt(hx)} {fmt(hy)} {fmt(hz)} {fmt(r)} {fmt(g)} {fmt(b)}"
            )

    if spawn is None:
        print("[AVISO] EMPTY 'spawn' ausente — usando spawn 0 2 0.")
        spawn = (0.0, 2.0, 0.0)
        warnings += 1

    out = find_output_path()
    lines = [f"# Exportado do Blender: {os.path.basename(bpy.data.filepath) or '(nao salvo)'}",
             f"# {len(boxes)} caixas, {len(meshes)} meshes"]
    lines += boxes + meshes
    lines.append(f"spawn {fmt(spawn[0])} {fmt(spawn[1])} {fmt(spawn[2])}")

    with open(out, "w", encoding="ascii", errors="replace") as f:
        f.write("\n".join(lines) + "\n")

    print(f"[OK] Exportado: {out} ({len(boxes)} caixas, {len(meshes)} meshes, {warnings} avisos)")


export_level()
