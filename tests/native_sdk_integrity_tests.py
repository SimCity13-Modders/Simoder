"""@summary Exercise native SDK integrity gates using isolated copies and a local exact-byte checkout."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

def main():
    """@summary Verify valid, tampered, missing, dirty and wrong-revision consumers. @return Zero on exact outcomes."""
    parser = argparse.ArgumentParser()
    parser.add_argument('--work-dir', required=True)
    parser.add_argument('--sdk-source', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    work = Path(args.work_dir).resolve()
    work.mkdir(parents=True, exist_ok=False)
    env = {**os.environ, 'PYTHONDONTWRITEBYTECODE': '1', 'GIT_OPTIONAL_LOCKS': '0'}
    records = []

    def run(name, arguments, succeeds, diagnostic=None):
        """@summary Record a real command and require its outcome. @param name Evidence key. @param arguments Vector. @param succeeds Expected success. @param diagnostic Required failure text."""
        command = ['rtk', 'proxy', *arguments]
        result = subprocess.run(command, env=env, capture_output=True, timeout=60)
        output = result.stdout + result.stderr
        (work / (name + '.log')).write_bytes(output)
        records.append({'name': name, 'command': command, 'exit': result.returncode, 'expected_success': succeeds})
        (work / 'results.json').write_text(json.dumps(records, indent=2))
        assert (result.returncode == 0) == succeeds, name
        if diagnostic:
            assert diagnostic.encode() in output, name

    clone = work / 'sdk'
    run('checkout', ['git', '-c', 'core.autocrlf=true', 'clone', '--no-hardlinks', str(Path(args.sdk_source).resolve()), str(clone)], True)
    run('checkout-contract', [sys.executable, str(clone / 'tests/test_sdk.py'), 'contract', '--source', str(clone)], True)

    def fixture(name):
        """@summary Copy only the integrity gate and canonical headers. @param name Unique case. @return Project path."""
        project = work / name
        for file in ['cmake/VerifyNativeSdk.cmake', 'cmake/simoder-native-sdk.lock.cmake',
                     'src/mods/native/contract.h', 'src/mods/native/abi_v1.h']:
            target = project / file
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / file, target)
        (project / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.24)\nproject(NativeIntegrity LANGUAGES NONE)\ninclude(cmake/VerifyNativeSdk.cmake)\n')
        return project

    for name in ['valid', 'tampered', 'missing', 'wrong-revision', 'dirty']:
        project = fixture(name)
        if name == 'tampered':
            with (project / 'src/mods/native/contract.h').open('ab') as file:
                file.write(b'changed')
        if name == 'missing':
            (project / 'src/mods/native/contract.h').unlink()
        if name == 'wrong-revision':
            lock = project / 'cmake/simoder-native-sdk.lock.cmake'
            text = lock.read_text()
            import re
            text = re.sub(r'(SIMODER_NATIVE_SDK_REVISION ")[0-9a-f]{40}', r'\g<1>' + '0' * 40, text)
            lock.write_text(text)
        if name == 'dirty':
            (clone / 'unexpected-native-integrity-test.txt').write_text('untracked fixture only')
        command = ['cmake', '-S', str(project), '-B', str(work / (name + '-build')),
                   '-DSIMODER_NATIVE_SDK_SOURCE_DIR=' + str(clone)]
        diagnostic = {'tampered': 'Native SDK declaration drift', 'missing': 'Missing or aliased native SDK',
                      'wrong-revision': 'Native SDK requires exact clean revision', 'dirty': 'Native SDK requires exact clean revision'}.get(name)
        run(name, command, name == 'valid', diagnostic)
    print('PASS: exact-byte checkout and five real configure integrity cases')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
