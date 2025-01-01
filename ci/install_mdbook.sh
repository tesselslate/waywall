#!/usr/bin/env sh

MDBOOK_TAG=$(curl "https://api.github.com/repos/rust-lang/mdbook/releases/latest" | jq -r ".tag_name")
MDBOOK_URL="https://github.com/rust-lang/mdbook/releases/download/$MDBOOK_TAG/mdbook-$MDBOOK_TAG-x86_64-unknown-linux-gnu.tar.gz"
mkdir mdbook
curl -sSL $MDBOOK_URL | tar -xz --directory=./mdbook
echo $(pwd)/mdbook >> $GITHUB_PATH
