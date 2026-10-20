#!/usr/bin/env sh
# Downloads a whisper.cpp ggml model into server/models.
#   ./get-model.sh [large-v3-turbo|medium|small|base.en|...]
set -eu
cd "$(dirname "$0")"
MODEL="${1:-large-v3-turbo}"
mkdir -p models
DEST="models/ggml-$MODEL.bin"
[ -f "$DEST" ] && { echo "Already present: $DEST"; exit 0; }
echo "Downloading ggml-$MODEL.bin ..."
curl -L --fail --progress-bar \
     -o "$DEST" \
     "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$MODEL.bin"
echo "Saved $DEST"
