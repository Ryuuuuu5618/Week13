#!/usr/bin/env bash
# 요청 실험실 데모: 백엔드 A(8001), B(8002)를 뒤에 띄우고, 프록시 서버(8080)를 앞에 띄운다.
# 사용법: webserver 폴더에서  ./demo/run_demo.sh   (또는 make demo)
# 끝낼 때는 Ctrl+C 한 번. 백엔드도 같이 꺼진다.

set -u
cd "$(dirname "$0")/.."

make -s server || exit 1

LOG=/tmp/week13_demo
mkdir -p "$LOG"

./server 8001 demo/backend_a 2 > "$LOG/backend_a.log" 2>&1 &
A=$!
./server 8002 demo/backend_b 2 > "$LOG/backend_b.log" 2>&1 &
B=$!

cleanup() {
    kill "$A" "$B" 2> /dev/null
    wait 2> /dev/null
}
trap cleanup EXIT

sleep 0.3
for p in "$A:8001" "$B:8002"; do
    if ! kill -0 "${p%%:*}" 2> /dev/null; then
        echo "백엔드 ${p##*:}번을 띄우지 못했습니다. 포트가 이미 쓰이는지 확인하세요: ss -ltnp | grep ${p##*:}"
        exit 1
    fi
done

cat << EOF
────────────────────────────────────────────────────────────
 요청 실험실 데모
   브라우저:   http://localhost:8080
   백엔드 A:   8001 (PID $A)   → 끄기: kill $A
   백엔드 B:   8002 (PID $B)   → 끄기: kill $B
   A 다시 켜기: ./server 8001 demo/backend_a 2 &
   백엔드 로그: $LOG/backend_a.log, $LOG/backend_b.log
 아래부터는 8080 서버 로그입니다. 끝내려면 Ctrl+C
────────────────────────────────────────────────────────────
EOF

./server 8080 ./www 8 demo/routes.conf
