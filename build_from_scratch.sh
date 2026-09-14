#!/usr/bin/env bash

set -u

CONFIG="Release"
BUILD_DIR="build-linux"

print_usage() {
    cat <<EOF
Usage: $(basename "$0") [--config Debug|Release] [--build-dir path]

  --config     Build configuration. Defaults to Release.
  --build-dir  CMake build directory. Defaults to build-linux.
  --help       Show this help message.
EOF
}

usage_error() {
    printf 'Error: %s\n' "$1" >&2
    print_usage >&2
    exit 2
}

while (( $# > 0 )); do
    case "$1" in
        --config)
            (( $# >= 2 )) || usage_error "--config requires a value."
            CONFIG="$2"
            shift 2
            ;;
        --build-dir)
            (( $# >= 2 )) || usage_error "--build-dir requires a value."
            BUILD_DIR="$2"
            shift 2
            ;;
        --help|-h)
            print_usage
            exit 0
            ;;
        *)
            usage_error "unknown parameter \"$1\"."
            ;;
    esac
done

case "$CONFIG" in
    Debug|Release) ;;
    *) usage_error "--config must be Debug or Release." ;;
esac

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || {
    echo "Error: unable to locate the repository directory." >&2
    exit 1
}
cd -- "$SCRIPT_DIR" || {
    echo "Error: unable to enter the repository directory." >&2
    exit 1
}

if ! command -v cmake >/dev/null 2>&1; then
    echo "Error: cmake was not found on PATH." >&2
    exit 1
fi

echo "Configuring a fresh $CONFIG build in \"$BUILD_DIR\"..."
cmake --fresh -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CONFIG"
result=$?
if (( result != 0 )); then
    echo "Build failed with exit code $result." >&2
    exit "$result"
fi

echo "Building all targets..."
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel --clean-first
result=$?
if (( result != 0 )); then
    echo "Build failed with exit code $result." >&2
    exit "$result"
fi

echo "Build completed successfully."
