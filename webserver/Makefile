CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread
SRCS    = src/main.c src/http.c src/sbuf.c src/proxy.c
HDRS    = src/http.h src/sbuf.h src/proxy.h
TARGET  = server

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

run: $(TARGET)
	./$(TARGET) 8080 ./www

# 메모리 오류 검사용 빌드
asan: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $(TARGET) $(SRCS)

# 스레드 경쟁 상태(데이터 레이스) 검사용 빌드
tsan: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -fsanitize=thread -o $(TARGET) $(SRCS)

# 단위 테스트: 서버 없이 함수별로 검사 (main.c, sbuf.c는 필요 없음)
test: tests/test_main.c src/http.c src/proxy.c $(HDRS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o tests/run_tests tests/test_main.c src/http.c src/proxy.c
	./tests/run_tests

# 부하 테스트: 워커 수별 처리량 (ab 필요)
load: $(TARGET)
	./tests/load_test.sh

# 엣지 케이스: 서버를 띄워 이상한 요청을 보내 본다
edge: $(TARGET)
	./tests/edge_cases.sh

# 발표 데모: 백엔드 A·B + 프록시 서버를 한 번에 띄운다 (브라우저로 localhost:8080)
demo: $(TARGET)
	./demo/run_demo.sh

clean:
	rm -f $(TARGET) tests/run_tests

.PHONY: run asan tsan test edge load demo clean
