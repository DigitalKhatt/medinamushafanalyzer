"""Create and install dependencies into a named project environment with uv."""
import shutil
import subprocess


def install_requirements(environment, requirements, *, only_binary=False):
    uv = shutil.which('uv')
    if uv is None:
        raise ValueError('Install uv and make it available on PATH, then rerun setup.')
    python = environment / 'bin' / 'python'
    try:
        if not python.is_file():
            subprocess.run([uv, 'venv', '--python', '3.14', str(environment)], check=True)
        command = [uv, 'pip', 'install', '--python', str(python), '-r', str(requirements)]
        if only_binary:
            command.extend(['--only-binary', ':all:'])
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as error:
        raise ValueError(f'uv could not set up {environment} (exit {error.returncode}).') from error
    return python
