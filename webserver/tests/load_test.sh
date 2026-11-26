#!/usr/bin/env bash
# 부하 테스트: 워커 스레드 수를 바꿔 가며 ab(Apache Bench)로 처리량을 잰다.
# 실행: make load   (또는 webserver 폴더에서 ./tests/load_test.sh)
# 필요: ab (apache2-utils, Dockerfile에 포함)

cd "$(dirname "$0")/.." || exit 1
command -v ab > /dev/null || { echo "ab가 없습니다: apt-get install -y apache2-utils"; exit 1; }

P=18080
N=${N:-5000}          # 총 요청 수
C=${C:-50}            # 동시에 보내는 요청 수
URL="http://127.0.0.1:$P/style.css"

make -s || exit 1
echo "요청 ${N}개, 동시 ${C}개, 대상 ${URL}"
echo
printf "| 워커 스레드 | 처리량 (요청/초) | 평균 응답 시간 (ms) | 실패 |\n"
printf "| --- | --- | --- | --- |\n"

for T in 1 2 4 8 16; do
    ./server $P ./www $T > /dev/null 2>&1 &
    sleep 0.5
    OUT=$(ab -q -n "$N" -c "$C" "$URL" 2>&1)
    RPS=$(echo "$OUT" | awk '/Requests per second/ {print $4}')
    TPR=$(echo "$OUT" | awk '/Time per request/ {print $4; exit}')
    FAILED=$(echo "$OUT" | awk '/Failed requests/ {print $3}')
    printf "| %s | %s | %s | %s |\n" "$T" "${RPS:-?}" "${TPR:-?}" "${FAILED:-?}"
    pkill -INT -f "server $P "
    sleep 0.5
done

# keep-alive 비교: 워커 8개. 동시 연결 수가 워커보다 적을 때(8)와 많을 때(50)
# 많을 때는 적응형 keep-alive가 줄 선 연결에게 워커를 양보하므로 close 방식에 가까워진다.
echo
echo "keep-alive 비교 (워커 8개, 요청 ${N}개)"
printf "| 동시 연결 | 방식 | 처리량 (요청/초) | 평균 응답 시간 (ms) | 가장 느린 응답 (ms) | 실패 |\n"
printf "| --- | --- | --- | --- | --- | --- |\n"
for CC in 8 50; do
    for MODE in close keepalive; do
        ./server $P ./www 8 > /dev/null 2>&1 &
        sleep 0.5
        if [ "$MODE" = keepalive ]; then OPT="-k"; NAME="keep-alive"; else OPT=""; NAME="요청마다 새 연결"; fi
        OUT=$(ab -q $OPT -n "$N" -c "$CC" "$URL" 2>&1)
        RPS=$(echo "$OUT" | awk '/Requests per second/ {print $4}')
        TPR=$(echo "$OUT" | awk '/Time per request/ {print $4; exit}')
        MAX=$(echo "$OUT" | awk '/100%/ {print $2}')
        FAILED=$(echo "$OUT" | awk '/Failed requests/ {print $3}')
        printf "| %s | %s | %s | %s | %s | %s |\n" "$CC" "$NAME" "${RPS:-?}" "${TPR:-?}" "${MAX:-?}" "${FAILED:-?}"
        pkill -INT -f "server $P "
        sleep 0.5
    done
done
