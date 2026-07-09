# -*- mode: python ; coding: utf-8 -*-
from PyInstaller.utils.hooks import collect_all

datas = [('assets', 'assets'), ('config', 'config'), ('src\\ui\\themes', 'src\\ui\\themes')]
binaries = [('E:\\anaconda\\envs\\minist\\Library\\bin\\pyside6.cp310-win_amd64.dll', '.'), ('E:\\anaconda\\envs\\minist\\Library\\bin\\pyside6qml.cp310-win_amd64.dll', '.'), ('E:\\anaconda\\envs\\minist\\Library\\bin\\shiboken6.cp310-win_amd64.dll', '.')]
hiddenimports = ['pyqtgraph.opengl', 'OpenGL.GL', 'trimesh.exchange.gltf', 'trimesh.exchange.obj', 'trimesh.exchange.stl', 'pandas', 'openpyxl', 'numpy']
tmp_ret = collect_all('pyqtgraph')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]
tmp_ret = collect_all('OpenGL')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]
tmp_ret = collect_all('trimesh')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]


a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='FaWave',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['assets\\app_icon.ico'],
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='FaWave',
)
