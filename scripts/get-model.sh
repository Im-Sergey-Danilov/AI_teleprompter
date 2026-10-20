#!/usr/bin/env sh
# Downloads a whisper.cpp ggml model into ./models.
#   scripts/get-model.sh [base.en|small|medium|large-v3-turbo|...]
set -eu
cd "$(dirname "$0")/.."
MODEL="${1:-base.en}"
mkdir -p models
DEST="models/ggml-$MODEL.bin"
[ -f "$DEST" ] && { echo "Already present: $DEST"; exit 0; }
URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$MODEL.bin"
echo "Downloading $URL"
curl -L --fail --progress-bar -o "$DEST" "$URL"
echo "Saved to $DEST"
