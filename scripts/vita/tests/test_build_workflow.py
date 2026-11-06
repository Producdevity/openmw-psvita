#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class DockerBuildTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='openmw-build-test-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.sdk = self.base / 'sdk'
        self.build = self.base / 'build'
        self.gl = self.base / 'gl'
        self.bin = self.base / 'bin'
        for path in (self.sdk / 'bin', self.build, self.gl / 'source', self.bin):
            path.mkdir(parents=True)
        for name in ('arm-vita-eabi-gcc', 'arm-vita-eabi-g++'):
            path = self.sdk / 'bin' / name
            path.write_text('#!/bin/sh\nexit 0\n')
            path.chmod(0o755)
        (self.gl / 'libvitaGL.a').write_bytes(b'library fixture')
        (self.gl / 'source/vitaGL.h').write_text('header fixture\n')
        self.log = self.base / 'docker.jsonl'
        docker = self.bin / 'docker'
        docker.write_text('#!/usr/bin/env python3\nimport json, os, sys\n'
                          'with open(os.environ["BUILD_TEST_LOG"], "a") as f:\n'
                          '    f.write(json.dumps(sys.argv[1:]) + "\\n")\n')
        docker.chmod(0o755)
        self.env = dict(os.environ,
                        PATH=str(self.bin) + os.pathsep + os.environ['PATH'],
                        OPENMW_VITA_SDK=str(self.sdk), VITAGL_SRC=str(self.gl),
                        VITAGL_DIR='/cache/vitagl', OPENMW_VITA_BUILD_DIR=str(self.build),
                        OPENMW_VITA_CACHE_DIR=str(self.base / 'cache'),
                        OPENMW_VITA_BAKE_MODS='ON', BUILD_TEST_LOG=str(self.log))
        for key in ('ICU_HOST_BUILD_DIR', 'OPENMW_VITA_FETCH_ROOT'):
            self.env.pop(key, None)

    def invoke(self, **overrides):
        self.log.unlink(missing_ok=True)
        return subprocess.run(['bash', str(ROOT / 'scripts/vita/docker-build.sh')],
                              env=dict(self.env, **overrides), capture_output=True, text=True)

    def command(self):
        calls = [json.loads(line) for line in self.log.read_text().splitlines()]
        return next(call for call in calls if call[0] == 'run')[-1]

    def configured(self, bake='ON'):
        identity = {'sdk_root': str(self.sdk.resolve()), 'compiler_sha256': {
            name: hashlib.sha256((self.sdk / 'bin' / name).read_bytes()).hexdigest()
            for name in ('arm-vita-eabi-gcc', 'arm-vita-eabi-g++')}}
        (self.build / '.openmw-vita-sdk.json').write_text(json.dumps(identity))
        (self.build / 'Makefile').write_text('')
        cache = ['VITAGL_DIR:PATH=/cache/vitagl', f'OPENMW_VITA_BAKE_MODS:BOOL={bake}',
                 'ICU_HOST_BUILD_DIR:STRING=/src/icu-host']
        for dependency in ('BULLET', 'MYGUI', 'OSG', 'RECASTNAVIGATION', 'SQLITE3', 'YAML-CPP'):
            cache.append(f'FETCHCONTENT_SOURCE_DIR_{dependency}:PATH=/src/sources/{dependency.lower()}')
        (self.build / 'CMakeCache.txt').write_text('\n'.join(cache) + '\n')

    def test_fresh_build_and_matching_incremental_build(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.command(), 'scripts/vita/build.sh')
        self.configured()
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.command(), 'scripts/vita/build-fast.sh')

    def test_asset_configuration_changes_force_reconfigure(self):
        for cached, requested in (('ON', 'OFF'), ('OFF', 'ON')):
            with self.subTest(cached=cached, requested=requested):
                self.configured(cached)
                result = self.invoke(OPENMW_VITA_BAKE_MODS=requested)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.command(), 'scripts/vita/build.sh')

    def test_explicit_dependency_changes_force_reconfigure(self):
        self.configured()
        for change in ({'ICU_HOST_BUILD_DIR': '/src/other-host'},
                       {'OPENMW_VITA_FETCH_ROOT': '/src/other-sources'},
                       {'VITAGL_DIR': '/src/other-gl'}):
            with self.subTest(change=change):
                result = self.invoke(**change)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.command(), 'scripts/vita/build.sh')

    def test_compiler_change_rejects_existing_objects(self):
        self.configured()
        (self.sdk / 'bin/arm-vita-eabi-g++').write_text('changed compiler\n')
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SDK/compiler identity', result.stderr)
        self.assertFalse(self.log.exists())

    def test_unrecorded_sdk_rejects_existing_objects(self):
        self.configured()
        (self.build / '.openmw-vita-sdk.json').unlink()
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SDK/compiler identity', result.stderr)
        self.assertFalse(self.log.exists())

    def test_failed_generator_clean_preserves_unrelated_files(self):
        notes = self.build / 'unrelated-notes'
        notes.mkdir()
        sentinel = notes / 'keep.txt'
        sentinel.write_text('local work\n')
        (self.build / 'CMakeCache.txt').write_text(f'CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}\n')
        cmake = self.bin / 'cmake'
        cmake.write_text('#!/bin/sh\nexit 67\n')
        cmake.chmod(0o755)
        result = subprocess.run(['bash', str(ROOT / 'scripts/vita/build.sh'), 'clean'],
                                env=dict(self.env, VITASDK=str(self.sdk), VITAGL_DIR=str(self.gl)),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 67, result.stdout + result.stderr)
        self.assertEqual(sentinel.read_text(), 'local work\n')

    def test_relative_fast_build_path_is_exported_absolute(self):
        repo = self.base / 'fixture-repo'
        scripts = repo / 'scripts/vita'
        scripts.mkdir(parents=True)
        out = repo / 'out'
        out.mkdir()
        (out / 'Makefile').write_text('')
        shutil.copyfile(ROOT / 'scripts/vita/build-fast.sh', scripts / 'build-fast.sh')
        (scripts / 'build-id.sh').write_text('#!/bin/sh\nprintf fixture-id\n')
        (scripts / 'archive-build.sh').write_text(
            '#!/bin/sh\ncase "$OPENMW_VITA_BUILD_DIR" in /*) exit 0;; *) exit 68;; esac\n')
        make = self.bin / 'make'
        make.write_text('#!/bin/sh\nexit 0\n')
        make.chmod(0o755)
        result = subprocess.run(['bash', str(scripts / 'build-fast.sh')], cwd=repo,
                                env=dict(self.env, OPENMW_VITA_BUILD_DIR='out',
                                         VITASDK=str(self.sdk), VITAGL_DIR=str(self.gl)),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cached_luajit_marker_rejects_unversioned_source(self):
        work = self.base / 'lua-work'
        source = work / 'LuaJIT-Vita/src'
        source.mkdir(parents=True)
        (work / 'luajit_done').write_text('')
        for name in ('luajit.h', 'lua.h', 'lualib.h', 'lauxlib.h', 'lua.hpp',
                     'luaconf.h', 'lj_arch.h', 'libluajit.a'):
            (source / name).write_text('unverified artifact\n')
        for name in ('include', 'lib'):
            (self.sdk / 'arm-vita-eabi' / name).mkdir(parents=True)
        result = subprocess.run(['bash', str(ROOT / 'scripts/vita-deps/build-luajit.sh'), str(work)],
                                env=dict(self.env, VITASDK=str(self.sdk)),
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must be clean at', result.stderr)
        self.assertFalse((self.sdk / 'arm-vita-eabi/lib/libluajit.a').exists())

    def test_ffmpeg_marker_does_not_skip_configuration_for_empty_sdk(self):
        work = self.base / 'ffmpeg-work'
        source = work / 'ffmpeg-6.1'
        source.mkdir(parents=True)
        (work / 'ffmpeg_done').write_text('')
        (work / 'ffmpeg-6.1.tar.bz2').write_bytes(b'archive fixture')
        configure = source / 'configure'
        configure.write_text('#!/bin/sh\nexit 69\n')
        configure.chmod(0o755)
        result = subprocess.run(['bash', str(ROOT / 'scripts/vita-deps/build-ffmpeg.sh'), str(work)],
                                env=dict(self.env, VITASDK=str(self.sdk)),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 69, result.stdout + result.stderr)
        self.assertFalse((work / 'ffmpeg_done').exists())

    def lz4_fixture(self):
        work = self.base / 'lz4-work'
        source = work / 'lz4/lib'
        source.mkdir(parents=True)
        for name in ('lz4.c', 'lz4.h', 'lz4hc.h', 'lz4frame.h', 'lz4frame_static.h'):
            (source / name).write_text('source fixture\n')
        for name in ('include', 'lib'):
            (self.sdk / 'arm-vita-eabi' / name).mkdir(parents=True)
        git = self.bin / 'git'
        git.write_text(
            '#!/usr/bin/env python3\nimport os, sys\n'
            'if "rev-parse" in sys.argv:\n'
            '    print("ebb370ca83af193212df4dcbadcc5d87bc0de2f0")\n'
            'elif "status" in sys.argv and os.environ.get("BUILD_TEST_DIRTY"):\n'
            '    print(" M lib/lz4.c")\n')
        git.chmod(0o755)
        log = self.base / 'make.jsonl'
        make = self.bin / 'make'
        make.write_text(
            '#!/usr/bin/env python3\nimport json, os, pathlib, sys\n'
            'args = sys.argv[1:]\n'
            'with open(os.environ["BUILD_TEST_MAKE_LOG"], "a") as f:\n'
            '    f.write(json.dumps(args) + "\\n")\n'
            'library = pathlib.Path(args[args.index("-C") + 1]) / "lib/liblz4.a"\n'
            'if "clean" in args:\n'
            '    library.unlink(missing_ok=True)\n'
            'else:\n'
            '    library.write_bytes(b"built fixture library")\n')
        make.chmod(0o755)
        return work, log

    def invoke_lz4(self, work, log, **overrides):
        return subprocess.run(['bash', str(ROOT / 'scripts/vita-deps/build-lz4.sh'), str(work)],
                              env=dict(self.env, VITASDK=str(self.sdk),
                                       BUILD_TEST_MAKE_LOG=str(log), **overrides),
                              capture_output=True, text=True)

    def test_lz4_refuses_tracked_edits_without_changing_them(self):
        work, log = self.lz4_fixture()
        edited = work / 'lz4/lib/lz4.c'
        edited.write_text('local edits\n')
        result = self.invoke_lz4(work, log, BUILD_TEST_DIRTY='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must be clean at', result.stderr)
        self.assertEqual(edited.read_text(), 'local edits\n')
        self.assertFalse(log.exists())

    def test_lz4_cleans_on_compiler_or_archive_change(self):
        work, log = self.lz4_fixture()

        def run_and_check_clean(expected):
            log.unlink(missing_ok=True)
            result = self.invoke_lz4(work, log)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            commands = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual(any('clean' in command for command in commands), expected)

        run_and_check_clean(True)
        run_and_check_clean(False)
        (self.sdk / 'bin/arm-vita-eabi-gcc').write_text('different compiler\n')
        run_and_check_clean(True)
        (work / 'lz4/lib/liblz4.a').write_bytes(b'replaced cached library')
        run_and_check_clean(True)


if __name__ == '__main__':
    unittest.main()
