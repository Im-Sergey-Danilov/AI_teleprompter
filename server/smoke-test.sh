#!/usr/bin/env bash
# End-to-end check of the deployed endpoint:
#   1. the gateway rejects a request with no / wrong token
#   2. a correct token transcribes the bundled jfk.wav sample
#   3. the GPU was actually used
set -euo pipefail
cd "$(dirname "$0")"

[[ -f .env ]] || { echo "no .env - run ./setup.sh first" >&2; exit 1; }
set -a; . ./.env; set +a

URL="http://${WHISPER_BIND:-127.0.0.1}:${WHISPER_PORT:-8910}/v1/audio/transcriptions"
WAV=$(mktemp --suffix=.wav)
trap 'rm -f "$WAV"' EXIT

echo "endpoint: $URL"

echo -n "1. unauthenticated request is rejected ... "
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -F file=@/dev/null || true)
[[ "$code" == "401" ]] && echo "ok (401)" || { echo "FAIL (got $code, expected 401)"; exit 1; }

echo -n "2. wrong token is rejected ............... "
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" \
        -H "Authorization: Bearer definitely-not-the-token" -F file=@/dev/null || true)
[[ "$code" == "401" ]] && echo "ok (401)" || { echo "FAIL (got $code, expected 401)"; exit 1; }

docker compose exec -T whisper cat /opt/jfk.wav > "$WAV"

echo -n "3. transcribing jfk.wav .................. "
start=$(date +%s%3N)
body=$(curl -s --fail-with-body -X POST "$URL" \
        -H "Authorization: Bearer $API_TOKEN" \
        -F "file=@$WAV" \
        -F "response_format=json" \
        -F "model=whisper-1" \
        -F "language=en")   # the bundled sample is English; the server default is $WHISPER_LANG
took_ms=$(( $(date +%s%3N) - start ))

# NB: `cmd | grep -q` under `set -o pipefail` reports failure, because grep exits
# on the first match and the producer dies of SIGPIPE. Match against a variable.
if grep -qi "ask not what your country" <<<"$body"; then
    echo "ok (${took_ms} ms for 11 s of audio)"
else
    echo "FAIL"
    echo "  response: $body"
    exit 1
fi
echo "   -> $(echo "$body" | sed 's/.*"text"[[:space:]]*:[[:space:]]*"//; s/"[[:space:]]*}[[:space:]]*$//' | cut -c1-90)"

echo -n "4. CUDA backend in use ................... "
wlogs=$(docker compose logs whisper 2>&1 || true)
if grep -qiE "using CUDA[0-9]+ backend|found [0-9]+ CUDA devices" <<<"$wlogs"; then
    echo "ok"
    grep -iE "Device [0-9]:|using CUDA[0-9]+ backend|CUDA[0-9]+ total size" <<<"$wlogs" \
        | head -3 | sed 's/^.*| /   /'
else
    echo "WARNING - no CUDA line in the logs; it may be running on CPU"
fi

echo
echo "All good. Configure the Windows app with:"
echo "  Engine   Remote"
echo "  URL      $URL"
echo "  API key  $API_TOKEN"
