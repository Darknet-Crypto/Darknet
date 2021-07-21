#!/bin/sh

# Install patched libsodium 1.0.17.

export LC_ALL=C
set -e

if [ -z "${1}" ]; then
  echo "Usage: $0 <base-dir> [<extra-sodium-configure-flag> ...]"
  echo
  echo "Must specify a single argument: the directory in which sodium will be built."
  echo "This is probably \`pwd\` if you're at the root of the pivx repository."
  exit 1
fi

expand_path() {
  echo "$(cd "${1}" && pwd -P)"
}

ROOT_PREFIX="$(expand_path ${1})";
SODIUM_PREFIX="$ROOT_PREFIX/sodium"; shift;
echo $SODIUM_PREFIX
echo $ROOT_PREFIX
SODIUM_VERSION='1.0.17'
SODIUM_HASH='0cc3dae33e642cc187b5ceb467e0ad0e1b51dcba577de1190e9ffa17766ac2b1'
SODIUM_URL="https://download.libsodium.org/libsodium/releases/libsodium-${SODIUM_VERSION}.tar.gz"

check_exists() {
  which "$1" >/dev/null 2>&1
}

sha256_check() {
  # Args: <sha256_hash> <filename>
  #
  if check_exists sha256sum; then
    echo "${1}  ${2}" | sha256sum -c
  elif check_exists sha256; then
    if [ "$(uname)" = "FreeBSD" ]; then
      sha256 -c "${1}" "${2}"
    else
      echo "${1}  ${2}" | sha256 -c
    fi
  else
    echo "${1}  ${2}" | shasum -a 256 -c
  fi
}

http_get() {
  # Args: <url> <filename> <sha256_hash>
  #
  # It's acceptable that we don't require SSL here because we manually verify
  # content hashes below.
  #
  if [ -f "${2}" ]; then
    echo "File ${2} already exists; not downloading again"
  elif check_exists curl; then
    curl --insecure --retry 5 "${1}" -o "${2}"
  else
    wget --no-check-certificate "${1}" -O "${2}"
  fi

  sha256_check "${3}" "${2}"
}

mkdir -p "${SODIUM_PREFIX}"
http_get "${SODIUM_URL}" "libsodium-${SODIUM_VERSION}.tar.gz" "${SODIUM_HASH}"
tar -xzvf libsodium-${SODIUM_VERSION}.tar.gz -C "$SODIUM_PREFIX"
cd "${SODIUM_PREFIX}/libsodium-${SODIUM_VERSION}/"

# Apply patches
echo "Applying patches.."
patch -p1 < "${ROOT_PREFIX}/depends/patches/libsodium/1.0.15-pubkey-validation.diff" && \
patch -p1 < "${ROOT_PREFIX}/depends/patches/libsodium/1.0.15-signature-validation.diff" && \
patch -p1 < "${ROOT_PREFIX}/depends/patches/libsodium/1.0.15-library-version.diff" && \
DO_NOT_UPDATE_CONFIG_SCRIPTS=1 ./autogen.sh

# make
"${SODIUM_PREFIX}/libsodium-${SODIUM_VERSION}/configure" --enable-static --disable-shared
make && make check

make install

echo
echo "sodium build complete."
echo
