#!/usr/bin/env bash
set -euo pipefail

# Deterministic official nanopb 0.4.9.1 generator for Tesla BLE protocol schemas
# (pinned to teslamotors/vehicle-command @ 0a582da430c4aca6d397fb6cbed5b4d6bb380125).
#
# Environment configuration (no hardcoded personal absolute paths):
#   NANOPB_PYTHON     Python interpreter with pinned protobuf/grpcio-tools/nanopb
#                     (default: python3 on PATH)
#   NANOPB_GENERATOR  Path to nanopb_generator.py; if unset, invokes
#                     "${NANOPB_PYTHON} -m nanopb.generator.nanopb_generator"
#
# Usage:
#   ./tools/gen_tesla_proto.sh          # regenerate into components/tesla_key/proto/generated
#   ./tools/gen_tesla_proto.sh --check  # verify committed generated/ matches schema+options (CI gate)

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PROTO_ROOT="${REPO_ROOT}/components/tesla_key/proto"
SCHEMA_DIR="${PROTO_ROOT}/schema"
OPTIONS_DIR="${PROTO_ROOT}/options"
GEN_DIR="${PROTO_ROOT}/generated"

PYTHON_BIN="${NANOPB_PYTHON:-$(command -v python3 || true)}"
NANOPB_GEN="${NANOPB_GENERATOR:-}"

# Pinned dependency versions (nanopb 0.4.9.1, grpcio-tools 1.68.0, protobuf 5.28.3).
# protobuf 7.x changes only well-known timestamp.proto comments. Do not fall back
# to an unpinned interpreter.
PIN_PROTOBUF="5.28.3"
PIN_GRPCIO_TOOLS="1.68.0"
PIN_NANOPB="0.4.9.1"

if [[ -z "${PYTHON_BIN}" || ! -x "${PYTHON_BIN}" ]]; then
    echo "ERROR: pinned nanopb python not found (set NANOPB_PYTHON or ensure python3 is on PATH)" >&2
    echo "Recover by creating a virtualenv and running: pip install -r tools/tesla_proto_requirements.txt" >&2
    exit 1
fi

PROTO_VERS="$("${PYTHON_BIN}" - <<'PY'
import importlib.metadata as m
def ver(name):
    try:
        return m.version(name)
    except Exception:
        return "missing"
print(ver("protobuf"))
print(ver("grpcio-tools"))
print(ver("nanopb"))
PY
)"
GOT_PROTOBUF="$(printf '%s\n' "${PROTO_VERS}" | sed -n '1p')"
GOT_GRPCIO_TOOLS="$(printf '%s\n' "${PROTO_VERS}" | sed -n '2p')"
GOT_NANOPB="$(printf '%s\n' "${PROTO_VERS}" | sed -n '3p')"

if [[ "${GOT_PROTOBUF}" != "${PIN_PROTOBUF}" || "${GOT_GRPCIO_TOOLS}" != "${PIN_GRPCIO_TOOLS}" ]]; then
    echo "ERROR: proto generator dependency mismatch." >&2
    echo "  need protobuf==${PIN_PROTOBUF} grpcio-tools==${PIN_GRPCIO_TOOLS} nanopb==${PIN_NANOPB}" >&2
    echo "  have protobuf==${GOT_PROTOBUF} grpcio-tools==${GOT_GRPCIO_TOOLS} nanopb==${GOT_NANOPB}" >&2
    echo "  Install tools/tesla_proto_requirements.txt and point NANOPB_PYTHON to that interpreter." >&2
    exit 1
fi

if [[ -z "${NANOPB_GEN}" && "${GOT_NANOPB}" != "${PIN_NANOPB}" ]]; then
    echo "ERROR: nanopb version mismatch (need nanopb==${PIN_NANOPB}, have ${GOT_NANOPB}, and NANOPB_GENERATOR is unset)." >&2
    exit 1
fi
echo "Proto generator pin: protobuf ${GOT_PROTOBUF} grpcio-tools ${GOT_GRPCIO_TOOLS} nanopb ${GOT_NANOPB}"

CHECK_MODE=0
if [[ "${1:-}" == "--check" ]]; then
    CHECK_MODE=1
elif [[ -n "${1:-}" ]]; then
    echo "Usage: $0 [--check]" >&2
    exit 2
fi

if [[ -n "${NANOPB_GEN}" && ! -f "${NANOPB_GEN}" ]]; then
    echo "ERROR: nanopb_generator.py not found at ${NANOPB_GEN}" >&2
    exit 1
fi

TMP_WORK="$(mktemp -d /tmp/tesla-proto-gen.XXXXXX)"
trap 'rm -rf "${TMP_WORK}"' EXIT

mkdir -p "${TMP_WORK}/in" "${TMP_WORK}/out"
cp "${SCHEMA_DIR}"/*.proto "${TMP_WORK}/in/"
cp "${OPTIONS_DIR}"/*.options "${TMP_WORK}/in/"

PROTOS=(
    errors.proto
    keys.proto
    signatures.proto
    universal_message.proto
    vcsec.proto
    common.proto
    managed_charging.proto
    vehicle.proto
    car_server.proto
    google/protobuf/timestamp.proto
)

if [[ -n "${NANOPB_GEN}" ]]; then
    "${PYTHON_BIN}" "${NANOPB_GEN}" \
        --strip-path \
        --no-timestamp \
        -I "${TMP_WORK}/in" \
        -D "${TMP_WORK}/out" \
        "${PROTOS[@]}"
else
    "${PYTHON_BIN}" -m nanopb.generator.nanopb_generator \
        --strip-path \
        --no-timestamp \
        -I "${TMP_WORK}/in" \
        -D "${TMP_WORK}/out" \
        "${PROTOS[@]}"
fi

if [[ "${CHECK_MODE}" -eq 1 ]]; then
    if ! diff -ruN "${GEN_DIR}" "${TMP_WORK}/out"; then
        echo "ERROR: generated protobuf files have drifted from schema/options!" >&2
        exit 1
    fi
    echo "Proto generation consistency: PASS"
    exit 0
fi

rm -rf "${GEN_DIR}"
mkdir -p "${GEN_DIR}"
cp -R "${TMP_WORK}/out/." "${GEN_DIR}/"
echo "Proto generation complete: ${GEN_DIR}"
ls -lh "${GEN_DIR}"
