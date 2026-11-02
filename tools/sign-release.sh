#!/bin/sh
# Signs a published release. Until this has run the release exists for people but not for the updater, which ignores anything unsigned.
#   sh tools/sign-release.sh v2.0.0-beta3 [KEY_FILE]
set -eu
cd "$(dirname "$0")/.."
tag=$1
key=${2:-$HOME/.config/skillhub-release/update-key.pem}
repo=nimonht/skillhub
work=$(mktemp -d)
trap 'rm -rf "${work:?}"' EXIT
gh release download "$tag" --repo "$repo" --pattern SHA256SUMS --dir "$work"
cat "$work/SHA256SUMS"
openssl pkey -in "$key" -pubout -out "$work/public.pem"
have=$(openssl pkey -pubin -in "$work/public.pem" -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n')
if [ "$have" != "$(tr -d ' \n' < data/update-key.pub)" ]; then
    echo "this is not the key in data/update-key.pub, which is the one installed copies trust" >&2
    exit 1
fi
openssl pkeyutl -sign -rawin -inkey "$key" -in "$work/SHA256SUMS" -out "$work/SHA256SUMS.sig"
openssl pkeyutl -verify -rawin -pubin -inkey "$work/public.pem" -in "$work/SHA256SUMS" -sigfile "$work/SHA256SUMS.sig"
gh release upload "$tag" "$work/SHA256SUMS.sig" --repo "$repo"
echo "$tag is signed: the updater will offer it"
