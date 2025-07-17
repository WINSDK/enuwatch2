#!/usr/bin/env python3

import os
import random
import shutil
import string
import subprocess
import sys
from pathlib import Path

MAX_DEPTH = 5
MAX_DIRS_PER_LEVEL = 10
MAX_FILES_PER_DIR = 20
MAX_FILE_SIZE = 65_536  # bytes

CLEANUP_PATHS = []

ALPHANUM = string.ascii_letters + string.digits
rng = random.Random()


def random_string(length: int = 8) -> str:
    return "".join(rng.choice(ALPHANUM) for _ in range(length))


def create_random_file(directory: Path):
    filename = directory / random_string(rng.randint(5, 14))
    file_size = rng.randint(1, MAX_FILE_SIZE)
    filename.write_bytes(os.urandom(file_size))


def create_random_structure(base_dir: Path, depth: int = 0):
    if depth >= MAX_DEPTH:
        return

    # Random files
    for _ in range(rng.randint(1, MAX_FILES_PER_DIR)):
        create_random_file(base_dir)

    # Random sub-directories
    for _ in range(rng.randint(1, MAX_DIRS_PER_LEVEL)):
        subdir = base_dir / random_string(rng.randint(4, 11))
        subdir.mkdir(parents=False, exist_ok=True)
        create_random_structure(subdir, depth + 1)


def count_tree(root: Path) -> tuple[int, int]:
    files = dirs = 0
    for _, dirnames, filenames in os.walk(root):
        dirs += len(dirnames)
        files += len(filenames)
    return files, dirs

def run_enu_under_lldb(enu_path: Path, args: list[str]) -> int:
    lldb = shutil.which("lldb")
    if not lldb:
        print("no lldb")
        exit(1)

    print("\nRunning enu under LLDB…")
    lldb_cmd = [
        lldb,
        "-o", "run",
        "-o", "bt",
        "--",
        str(enu_path),
        *args,
    ]
    return subprocess.run(lldb_cmd, check=False).returncode

def main():
    temp_name = f"test_{random_string(12)}"
    test_dir = Path("/tmp", temp_name)

    print(f"Creating random directory structure in: {test_dir}")
    test_dir.mkdir(parents=True, exist_ok=True)

    create_random_structure(test_dir)

    files, dirs = count_tree(test_dir)
    print("\nDirectory structure created successfully!")
    print(f"Files: {files}")
    print(f"Directories: {dirs}")

    print(f"\nRunning enu with path: {test_dir}")
    print(f"Output will be in: ~/{test_dir.name}")

    enu_path = Path("./build/src/enu")
    if not (enu_path.is_file() and os.access(enu_path, os.X_OK)):
        print("Error: ./build/src/enu not found or not executable")
        print("Please build the project first")
        sys.exit(1)

    enu_args = ["nicolas@localhost", "--path", str(test_dir)]

    lldb_status = run_enu_under_lldb(enu_path, enu_args)
    if lldb_status == -1:
        print("LLDB not found; running enu normally…")
        try:
            subprocess.run([str(enu_path), *enu_args], check=True)
        except subprocess.CalledProcessError as exc:
            print(f"enu exited with status {exc.returncode}", file=sys.stderr)
            sys.exit(exc.returncode)

    print("\nCleaning up…")
    shutil.rmtree(test_dir, ignore_errors=True)
    shutil.rmtree(Path.home() / temp_name, ignore_errors=True)
    print("Test directory removed")

    if lldb_status > 0:
        sys.exit(lldb_status)


if __name__ == "__main__":
    main()
