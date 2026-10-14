"""@summary Verify forwarding and integrated runtime startup without extracting binaries."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

## @summary Start an unsupported SimCity fixture with only the integrated DLL beside its executable.
def main():
    probe, dll = map(Path, sys.argv[1:3])
    temporary_root = str(Path(tempfile.gettempdir()).resolve())
    temporary_root = '\\\\?\\UNC\\' + temporary_root[2:] if temporary_root.startswith('\\\\') else '\\\\?\\' + temporary_root
    with tempfile.TemporaryDirectory(prefix='simoder-single-start-', dir=temporary_root) as temporary:
        root = Path(temporary)
        game = root / 'SimCity'
        game.mkdir()
        executable = game / 'SimCity.exe'
        shutil.copy2(probe, executable)
        shutil.copy2(dll, game / 'd3d9.dll')
        assert not (root / 'simoder').exists()
        environment = dict(os.environ, LOCALAPPDATA=str(root))
        report = root / 'report.txt'
        result = subprocess.run([str(executable), str(report)], env=environment, timeout=45)
        output = report.read_text() if report.exists() else 'Missing graphics report'
        assert result.returncode == 0, output
        assert all(f'{key}=1' in output for key in ('exports', 'creation', 'invalidMatches', 'statusMatches', 'extendedMatches', 'consoleUnchanged')), output
        data = root / 'simoder'
        log = (data / 'logs' / 'proxy-bootstrap.log').read_text()
        assert log.count('bootstrap=proxy') == 1 and log.count('runtime.initialize=14') == 1, log
        assert 'services stopped' in log, log
        assert (data / 'config.toon').is_file() and not (data / 'sc13modloader.dll').exists()
        assert not (root / 'Simoder' / 'bundles').exists()
        assert not list(root.rglob('*.exe')) == [], 'Fixture executable must exist'
        assert list(root.rglob('*.exe')) == [executable], 'Runtime must not extract executable helpers'
        assert list(root.rglob('*.dll')) == [game / 'd3d9.dll'], 'Runtime must not extract another DLL'
        runtime_log = (data / 'logs' / 'sc13modloader.log').read_text()
        assert 'No hooks or mod services started' in runtime_log, runtime_log
    print('Integrated DLL: graphics, no binary extraction, generated config and unsupported-build rejection passed.')

if __name__ == '__main__':
    main()
