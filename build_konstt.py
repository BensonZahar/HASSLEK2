#!/usr/bin/env python3
"""
build_konstt.py — сборка Android NDK.

  Mods/*.c            -> libkonstt_mods_arm64.so / libkonstt_mods_arm32.so   (БАНДЛ: все моды в одной .so, скачивается игрой)
  loader/hassle_loader.c -> libkonstt_loader_arm64.so / libkonstt_loader_arm32.so   (ЗАГРУЗЧИК: вшивается в APK один раз через patch_apk.py)
  manifest.txt                                                              (sha256 + размер бандла, по нему загрузчик понимает, что есть новая сборка)

  python build_konstt.py [путь_к_NDK] [--url https://raw.githubusercontent.com/ВЛАДЕЛЕЦ/РЕПО/dist/] [--out out] [--no-loader] [--check]

Как добавить новый мод: положите файл Mods/hassle_что_угодно.c, запушьте. Список модулей в бандле собирается автоматически
(все Mods/*.c, кроме файлов на «_»). Файл должен начинаться с `#pragma once` и `#include "hassle_core.c"` (ядро; другие моды
включать нельзя — каждый мод обязан собираться с одним только ядром) — пример в Mods/_template.c. APK пересобирать не нужно: игра сама докачает бандл при следующем запуске.

  --check   только проверить, что бандл и загрузчик компилируются системным gcc/clang (без NDK) — для быстрой проверки в CI.
            Заодно каждый мод (и шаблон) собирается отдельно, вместе с одним только ядром: так видно, если мод начал зависеть от другого.
"""
import glob, hashlib, os, platform, subprocess, sys, time

here = os.path.dirname(os.path.abspath(__file__))
mods_dir = os.path.join(here, 'Mods')
loader_src = os.path.join(here, 'loader', 'hassle_loader.c')

args = sys.argv[1:]
def opt(name, default=None, flag=False):
    if name in args:
        i = args.index(name)
        if flag:
            args.pop(i); return True
        v = args[i + 1]; del args[i:i + 2]; return v
    return False if flag else default

out_dir = os.path.abspath(opt('--out', os.path.join(here, 'out')))
base_url = opt('--url', os.environ.get('HM_BASE_URL'))
no_loader = opt('--no-loader', flag=True)
check = opt('--check', flag=True)
ndk_arg = args[0] if args else None
os.makedirs(out_dir, exist_ok=True)

if not base_url and os.environ.get('GITHUB_REPOSITORY'):
    base_url = 'https://raw.githubusercontent.com/%s/dist/' % os.environ['GITHUB_REPOSITORY']
if base_url and not base_url.endswith('/'):
    base_url += '/'
if not no_loader and not check:
    if not base_url:
        sys.exit('Нужен адрес, откуда игра будет качать бандл: --url https://raw.githubusercontent.com/ВЛАДЕЛЕЦ/РЕПО/dist/  '
                 '(в GitHub Actions берётся автоматически из GITHUB_REPOSITORY). Либо --no-loader, если нужен только бандл.')
    if not base_url.startswith('https://'):
        sys.exit('Адрес должен быть https:// — загрузчик не принимает обычный http.')

# ── список модулей ──
mods = sorted(os.path.basename(f) for f in glob.glob(os.path.join(mods_dir, '*.c')) if not os.path.basename(f).startswith('_'))
if not mods:
    sys.exit('В папке Mods нет ни одного .c')
CORE = 'hassle_core.c'
if CORE not in mods:
    sys.exit('Mods/hassle_core.c — ядро, без него бандл не собрать')
mods.remove(CORE); mods.insert(0, CORE)      # ядро всегда первым; остальные включают его сами (#pragma once)
print('модули в бандле:', ', '.join(mods))

build_id = time.strftime('%Y%m%d-%H%M%S', time.gmtime()) + '-' + (os.environ.get('GITHUB_SHA', 'local')[:7])
bundle_c = os.path.join(out_dir, 'bundle_gen.c')
with open(bundle_c, 'w', encoding='utf-8') as f:
    f.write('/* создано build_konstt.py — не править. Все .c из Mods одним блоком (общие static-переменные). */\n')
    f.write('#define KONSTT_BUNDLE 1\n')
    for m in mods:
        f.write('#include "%s"\n' % m)

if check:
    cc = os.environ.get('CC', 'cc')
    cflags = [cc, '-shared', '-fPIC', '-O1', '-Wall', '-Wno-unused-function', '-DHM_BUILD_ID="check"', '-I', mods_dir]
    jobs = [('бандл (все моды вместе)', cflags + [bundle_c])]
    # каждый мод отдельно, вместе с одним ядром (и «одно ядро», и шаблон): моды не должны зависеть друг от друга
    solo = [(None, 'одно ядро')] + [(m, m) for m in mods if m != CORE]
    if os.path.exists(os.path.join(mods_dir, '_template.c')):
        solo.append(('_template.c', 'шаблон _template.c'))
    for m, title in solo:
        p = os.path.join(out_dir, 'solo_gen.c')
        with open(p, 'w', encoding='utf-8') as f:
            f.write('#define KONSTT_BUNDLE 1\n#include "%s"\n' % CORE)
            if m: f.write('#include "%s"\n' % m)
        jobs.append(('ядро + ' + title if m else title, cflags + [p]))
    jobs.append(('загрузчик', [cc, '-shared', '-fPIC', '-O1', '-Wall', '-DHM_BASE_URL="https://example.invalid/"', loader_src]))
    for name, cmd in jobs:
        print('> check', name)
        if subprocess.run(cmd + ['-o', os.path.join(out_dir, 'check.so')]).returncode:
            sys.exit('проверка: %s не компилируется' % name)
    for f in ('check.so', 'solo_gen.c'):
        if os.path.exists(os.path.join(out_dir, f)): os.remove(os.path.join(out_dir, f))
    print('проверка пройдена'); sys.exit(0)

def find_ndk():
    if ndk_arg: return ndk_arg
    for k in ('ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT', 'ANDROID_NDK'):
        if os.environ.get(k): return os.environ[k]
    sdks = [os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'),
            os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'),
            os.path.expanduser('~/Android/Sdk'), os.path.expanduser('~/Library/Android/sdk')]
    for s in sdks:
        if s:
            c = sorted(glob.glob(os.path.join(s, 'ndk', '*')))
            if c: return c[-1]
    sys.exit('NDK not found. Pass the path as an argument.')

ndk = find_ndk()
host = {'Windows': 'windows-x86_64', 'Linux': 'linux-x86_64', 'Darwin': 'darwin-x86_64'}[platform.system()]
clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
if not os.path.exists(clang):
    sys.exit('clang not found: ' + clang)

targets = [
    ('arm64', 'aarch64-linux-android21', ['-Wl,-z,max-page-size=16384']),
    ('arm32', 'armv7a-linux-androideabi21', []),
]
common = ['-O2', '-shared', '-fPIC', '-fvisibility=hidden', '-fno-stack-protector', '-Wall', '-Wno-unused-function', '-Wl,--build-id=none', '-s']

def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

manifest = ['loader_min 1', 'build ' + build_id]
for name, tgt, extra in targets:
    bundle = os.path.join(out_dir, 'libkonstt_mods_%s.so' % name)
    cmd = [clang, '--target=' + tgt] + common + ['-Wl,-soname,libHassleMods.so', '-DHM_BUILD_ID="%s"' % build_id, '-I', mods_dir, bundle_c, '-o', bundle] + extra
    print('> бандл', name)
    if subprocess.run(cmd).returncode: sys.exit('сборка бандла %s не удалась' % name)
    size = os.path.getsize(bundle)
    print('   ', bundle, size, 'байт')
    manifest.append('%s %s %d %s' % (name, sha256(bundle), size, os.path.basename(bundle)))

    if not no_loader:
        so = os.path.join(out_dir, 'libkonstt_loader_%s.so' % name)
        cmd = [clang, '--target=' + tgt] + common + ['-Wl,-soname,libHassleExtended.so', '-DHM_BASE_URL="%s"' % base_url, loader_src, '-o', so] + extra
        print('> загрузчик', name)
        if subprocess.run(cmd).returncode: sys.exit('сборка загрузчика %s не удалась' % name)
        print('   ', so, os.path.getsize(so), 'байт')

with open(os.path.join(out_dir, 'manifest.txt'), 'w', newline='\n') as f:
    f.write('\n'.join(manifest) + '\n')
print('manifest.txt:\n' + '\n'.join('  ' + l for l in manifest))
print('done. сборка', build_id)
