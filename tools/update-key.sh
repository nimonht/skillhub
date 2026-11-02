#!/bin/sh
# Makes the key that signs releases. Run it once, in a terminal, because it asks for a passphrase. The private key stays with you and
# never goes to GitHub: anyone who holds it can put code on every machine that runs Skill Hub. Back it up somewhere safe; if it is lost
# or leaked, a new key can only reach people through a release they install by hand.
#   sh tools/update-key.sh [KEY_FILE]      default: ~/.config/skillhub-release/update-key.pem
set -eu
cd "$(dirname "$0")/.."
key=${1:-$HOME/.config/skillhub-release/update-key.pem}
if [ -e "$key" ]; then
    echo "$key already exists, and a new key would strand every installed copy" >&2
    exit 1
fi
umask 077
mkdir -p "$(dirname "$key")"
openssl genpkey -algorithm ed25519 -aes-256-cbc -out "$key"
openssl pkey -in "$key" -pubout -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n' > data/update-key.pub
echo >> data/update-key.pub
echo "private key: $key   (back it up; it is not in the repository)"
echo "public key:  data/update-key.pub   (commit this)"
