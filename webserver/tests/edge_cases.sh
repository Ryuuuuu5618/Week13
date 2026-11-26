#!/usr/bin/env bash
# 엣지 케이스 테스트: 서버를 직접 띄우고, 이상한 요청을 보내 응답과 생존 여부를 확인한다.
# 실행: make edge   (또는 webserver 폴더에서 ./tests/edge_cases.sh)
# 필요: curl, nc(netcat-openbsd)

cd "$(dirname "$0")/.." || exit 1

P=18080            # 테스트용 프록시 포트 (평소 쓰는 8080과 겹치지 않게)
A=18001; B=18002   # 테스트용 백엔드
SLOW=18005         # 연결은 받지만 응답을 안 하는 백엔드
DEAD=18009         # 아무도 없는 포트
TMP=$(mktemp -d)
PASS=0; FAIL=0

cleanup() {
    pkill -f "server $P " 2>/dev/null; pkill -f "server $A " 2>/dev/null; pkill -f "server $B " 2>/dev/null
    pkill -f "nc -l 127.0.0.1 $SLOW" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

check() {   # check "설명" 기대값 실제값
    if [ "$2" = "$3" ]; then PASS=$((PASS+1)); printf "  ✓ %s\n" "$1"
    else FAIL=$((FAIL+1)); printf "  ✗ %s  (기대: %s, 실제: %s)\n" "$1" "$2" "$3"; fi
}
code() { curl -s -o /dev/null -w "%{http_code}" --max-time 10 "$@"; }          # curl로 상태 코드만
raw()  { printf "$1" | nc -N -w 3 127.0.0.1 $P | head -1 | awk '{print $2}'; }  # 날 바이트를 보내고 상태 코드만
alive() { code "127.0.0.1:$P/"; }                                               # 서버가 살아 있나

# ── 준비: 정적 루트(큰 파일 포함), 라우팅 설정, 서버 3개 ──
cp -r www "$TMP/www"
head -c 20000000 /dev/zero > "$TMP/www/big.bin"           # 20MB 파일 (다운로드 도중 끊기용)
cat > "$TMP/routes.conf" <<EOF
/api/   127.0.0.1:$A 127.0.0.1:$B
/dead/  127.0.0.1:$DEAD
/slow/  127.0.0.1:$SLOW
EOF

make -s || exit 1
./server $A demo/backend_a 2 > /dev/null 2>&1 &
./server $B demo/backend_b 2 > /dev/null 2>&1 &
./server $P "$TMP/www" 4 "$TMP/routes.conf" > "$TMP/server.log" 2>&1 &
sleep 0.7

echo "[정상 동작]"
check "GET / → 200"                          200 "$(code 127.0.0.1:$P/)"
check "없는 파일 → 404"                       404 "$(code 127.0.0.1:$P/nope.html)"
check "프록시 /api/ → 백엔드 200"             200 "$(code 127.0.0.1:$P/api/hello.txt)"

echo "[잘못된 HTTP 요청]"
check "깨진 요청 라인 → 400"                  400 "$(raw 'HELLO\r\n\r\n')"
check "버전이 없음 → 400"                     400 "$(raw 'GET /\r\n\r\n')"
check "잘못된 버전 HTTP/2.0 → 505"            505 "$(raw 'GET / HTTP/2.0\r\n\r\n')"
check "지원 안 하는 메서드 DELETE → 501"      501 "$(code -X DELETE 127.0.0.1:$P/)"
check "Content-Length가 숫자가 아님 → 400"    400 "$(raw 'POST /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n')"
check "루트 밖 경로 ../ → 403"                403 "$(code --path-as-is 127.0.0.1:$P/../../etc/passwd)"

echo "[너무 큰 요청]"
LONG=$(head -c 2000 /dev/zero | tr '\0' 'a')
check "아주 긴 URI (2000자) → 400"           400 "$(code 127.0.0.1:$P/$LONG)"
BIGH=$(head -c 9000 /dev/zero | tr '\0' 'a')
check "헤더가 8KB 초과 → 431"                 431 "$(code -H "X-Big: $BIGH" 127.0.0.1:$P/)"
check "본문 2MB 선언 → 413"                   413 "$(raw 'POST /x HTTP/1.1\r\nContent-Length: 2000000\r\n\r\n')"

echo "[갑작스러운 연결 종료]"
printf 'GET / HTTP/1.1\r\nHost: a' | nc -N -w 1 127.0.0.1 $P > /dev/null
check "헤더를 보내다 끊어도 서버 생존"          200 "$(alive)"
printf 'POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc' | nc -N -w 1 127.0.0.1 $P > /dev/null
check "본문을 덜 보내고 끊어도 서버 생존"       200 "$(alive)"
curl -s 127.0.0.1:$P/big.bin | head -c 100 > /dev/null
sleep 0.5
check "20MB 다운로드 도중 끊어도 서버 생존 (SIGPIPE)" 200 "$(alive)"

echo "[keep-alive]"
reuse() { curl -sv -o /dev/null -o /dev/null "$@" 2>&1 | grep -c "Re-using"; }   # 연결을 재사용했나
check "HTTP/1.1 요청 2개는 연결 하나로 (재사용)"        1 "$(reuse 127.0.0.1:$P/ 127.0.0.1:$P/style.css)"
check "Connection: close를 보내면 재사용 안 함"         0 "$(reuse -H 'Connection: close' 127.0.0.1:$P/ 127.0.0.1:$P/style.css)"
check "HTTP/1.0은 기본으로 닫음"                        0 "$(reuse -0 127.0.0.1:$P/ 127.0.0.1:$P/style.css)"
PIPE=$(printf 'GET / HTTP/1.1\r\nHost: a\r\n\r\nGET /style.css HTTP/1.1\r\nHost: a\r\n\r\nGET /script.js HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n' | nc -w 3 127.0.0.1 $P | grep -c '^HTTP/1.1 200')
check "요청 3개를 한꺼번에 보내도 응답 3개 (파이프라이닝)" 3 "$PIPE"
PB=$(printf 'POST /e HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhelloGET /style.css HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n' | nc -w 3 127.0.0.1 $P | grep -ac 'HTTP/1.1 200')
check "POST 본문 바로 뒤의 다음 요청도 처리 (경계 유지)"  2 "$PB"
exec 3<>/dev/tcp/127.0.0.1/$P; printf 'GET / HTTP/1.1\r\nHost: a\r\n\r\n' >&3
T0=$(date +%s); timeout 9 cat <&3 > /dev/null; T1=$(date +%s); exec 3>&-
IDLE=$((T1 - T0)); [ $IDLE -ge 4 ] && [ $IDLE -le 7 ] && IDLE=ok
check "다음 요청이 없으면 약 5초 뒤 닫음 (유휴 타임아웃)" ok "$IDLE"

echo "[느린 손님]"
sleep 5 | nc 127.0.0.1 $P > /dev/null &                 # 연결만 하고 아무것도 안 보냄
sleep 0.3
check "느린 손님이 있어도 다른 요청은 바로 200" 200 "$(code --max-time 2 127.0.0.1:$P/)"

echo "[백엔드 문제 (Reverse Proxy)]"
check "백엔드가 없음 → 502"                   502 "$(code 127.0.0.1:$P/dead/x)"
(timeout 10 nc -l 127.0.0.1 $SLOW > /dev/null &) ; sleep 0.3
check "백엔드가 응답 안 함 → 5초 뒤 504"       504 "$(code 127.0.0.1:$P/slow/x)"
pkill -f "server $A " ; sleep 0.3
check "백엔드 하나가 죽어도 두 번 다 200 (살아 있는 쪽으로)" "200 200" "$(code 127.0.0.1:$P/api/hello.txt) $(code 127.0.0.1:$P/api/hello.txt)"

echo "[안전한 종료]"
pkill -INT -f "server $P "
for i in $(seq 30); do pgrep -f "server $P " > /dev/null || break; sleep 0.2; done
check "Ctrl+C(SIGINT) 후 정리 메시지를 남기고 종료" "1" "$(grep -c '안전하게 종료' "$TMP/server.log")"

echo
echo "결과: ${PASS}개 통과, ${FAIL}개 실패"
[ "$FAIL" -eq 0 ]
