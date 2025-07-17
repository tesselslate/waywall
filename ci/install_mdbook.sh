#!/usr/bin/env sh

mkdir mdbook
curl -sSL "https://github.com/rust-lang/mdbook/releases/download/v0.4.52/mdbook-v0.4.52-x86_64-unknown-linux-gnu.tar.gz" \
    | tar -xz --directory=./mdbook
echo $(pwd)/mdbook >> $GITHUB_PATH
