from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from time import perf_counter
from typing import Any

import numpy as np


@dataclass
class MeshPart:
    vertices: np.ndarray
    faces: np.ndarray
    color: tuple[float, float, float, float]
    name: str = "part"


@dataclass
class ModelLoadResult:
    success: bool
    model_type: str
    model_path: str
    parts: list[MeshPart] = field(default_factory=list)
    message: str = ""
    fallback: bool = False
    duration_ms: float = 0.0
    logs: list[str] = field(default_factory=list)


class ModelLoader:
    """Load GLB/GLTF/OBJ/STL with priority and safe fallback."""

    PRIORITY = [
        ("glb", "assembly_model.glb"),
        ("gltf", "assembly_model.gltf"),
        ("obj", "assembly_model.obj"),
        ("stl", "assembly_model.stl"),
        ("stl", "device_model.stl"),
    ]

    DEFAULT_COLOR = (0.72, 0.78, 0.86, 1.0)

    def __init__(self, model_root: str | None = None) -> None:
        self.model_root = Path(model_root or "assets/models")

    def load_best_available_model(self) -> ModelLoadResult:
        t0 = perf_counter()
        logs: list[str] = []
        for model_type, file_name in self.PRIORITY:
            path = self.model_root / file_name
            if not path.exists():
                # Suppress normal fallback checks from filling logs, only log on real loads
                pass
                continue
            size_mb = path.stat().st_size / (1024 * 1024)
            if size_mb > 20:
                logs.append(f"模型较大（{size_mb:.2f} MB），可能影响加载速度：{path.as_posix()}")
            try:
                if model_type in {"glb", "gltf"}:
                    parts = self._load_scene(path)
                elif model_type == "obj":
                    parts = self._load_obj(path, logs)
                else:
                    parts = self._load_stl(path)
                if parts:
                    dt = (perf_counter() - t0) * 1000
                    logs.append(f"三维模型加载耗时：{dt:.1f} ms")
                    return ModelLoadResult(True, model_type, path.as_posix(), parts, f"{model_type.upper()} 模型加载成功", False, dt, logs)
                logs.append(f"{path.as_posix()} 解析后无可渲染 mesh，继续尝试下一个格式")
            except ModuleNotFoundError as exc:
                logs.append(f"缺少依赖：{exc}，请安装 trimesh")
            except Exception as exc:
                logs.append(f"{model_type.upper()} 模型解析失败：{exc}，继续尝试下一个格式")

        dt = (perf_counter() - t0) * 1000
        logs.append("未找到可用三维模型，已使用简化模型")
        logs.append(f"三维模型加载耗时：{dt:.1f} ms")
        return ModelLoadResult(False, "fallback", "", [], "未找到可用三维模型，已使用简化模型", True, dt, logs)

    def _load_scene(self, path: Path) -> list[MeshPart]:
        import trimesh

        scene_or_mesh = trimesh.load(path, force="scene")
        if isinstance(scene_or_mesh, trimesh.Scene):
            scene = scene_or_mesh
        else:
            scene = trimesh.Scene(scene_or_mesh)

        parts: list[MeshPart] = []
        for node_name in scene.graph.nodes_geometry:
            try:
                transform, geom_name = scene.graph.get(node_name)
            except Exception:  # noqa: BLE001
                continue

            geom = scene.geometry.get(geom_name)
            if geom is None or not hasattr(geom, "faces"):
                continue

            try:
                mesh_obj = geom.copy()
                mesh_obj.apply_transform(transform)
                color = self._extract_color(mesh_obj)
                parts.append(
                    MeshPart(
                        vertices=np.asarray(mesh_obj.vertices, dtype=float),
                        faces=np.asarray(mesh_obj.faces, dtype=np.int32),
                        color=color,
                        name=f"{node_name}:{geom_name}",
                    )
                )
            except Exception:
                continue
        return parts

    def _load_obj(self, path: Path, logs: list[str]) -> list[MeshPart]:
        mtl = path.with_suffix(".mtl")
        if not mtl.exists():
            logs.append("OBJ 模型加载成功，但未找到 MTL 文件，已使用默认颜色")
        return self._load_scene(path)

    def _load_stl(self, path: Path) -> list[MeshPart]:
        import trimesh

        mesh = trimesh.load(path, force="mesh")
        if isinstance(mesh, trimesh.Scene):
            return self._load_scene(path)
        return [
            MeshPart(
                vertices=np.asarray(mesh.vertices, dtype=float),
                faces=np.asarray(mesh.faces, dtype=np.int32),
                color=self.DEFAULT_COLOR,
                name=path.stem,
            )
        ]

    def _extract_color(self, mesh: Any) -> tuple[float, float, float, float]:
        color = None
        visual = getattr(mesh, "visual", None)
        if visual is not None:
            face_colors = getattr(visual, "face_colors", None)
            if face_colors is not None and len(face_colors) > 0:
                color = np.asarray(face_colors[0], dtype=float)
            if color is None:
                material = getattr(visual, "material", None)
                if material is not None:
                    for attr in ("baseColorFactor", "diffuse", "main_color"):
                        value = getattr(material, attr, None)
                        if value is not None:
                            color = np.asarray(value, dtype=float)
                            break
        if color is None or len(color) < 3:
            return self.DEFAULT_COLOR
        if color.max() > 1.0:
            color = color / 255.0
        if len(color) == 3:
            color = np.append(color, 1.0)
        return tuple(float(x) for x in color[:4])
