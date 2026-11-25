#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    MAX_TARGET_SIZE = 4096, MAX_HEADER_SIZE = 8192, MAX_BODY_SIZE = 8192,
    RESPONSE_HEADER_SIZE = MAX_HEADER_SIZE + 256,
    FILE_CHUNK_SIZE = 4096, CONNECTION_QUEUE_CAPACITY = 8, POLL_CLIENT_CAPACITY = 64,
    THREAD_WORKER_COUNT = 2, PROXY_ROUTE_CAPACITY = 16, ROUTE_BACKEND_CAPACITY = 16,
    BACKEND_RESPONSE_LIMIT = 1024 * 1024, BACKEND_TIMEOUT_MS = 2000,
    CACHE_ENTRIES = 16, CACHE_BODY_LIMIT = 64 * 1024, CACHE_TTL_SECONDS = 60
};
enum { POLL_LISTENER, POLL_WAKEUP, POLL_FIRST_CLIENT };
enum server_mode { SERVER_SEQUENTIAL, SERVER_FORK, SERVER_THREADS, SERVER_POLL };

struct reverse_route {
    const char *prefix;
    size_t length;
    unsigned int ports[ROUTE_BACKEND_CAPACITY];
    size_t port_count;
    size_t next;
};

static struct {
    unsigned int port;
    const char *root;
    enum server_mode mode;
    bool forward;
    bool cache;
    size_t route_count;
    struct reverse_route routes[PROXY_ROUTE_CAPACITY];
} config = { .port = 8080, .root = "public" };

static pthread_mutex_t reverse_route_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t blocking_client = -1;
static int wakeup_read = -1;
static volatile sig_atomic_t wakeup_write = -1;

struct request_line {
    const char *method;
    const char *target;
    const char *version;
};

struct request_headers {
    const char *host;
    const char *connection;
    const char *content_type;
    bool has_content_length;
    size_t content_length;
    char relay_fields[MAX_HEADER_SIZE + 1];
};

struct response {
    const char *status;
    const char *content_type;
    const char *body;
    size_t length;
    int filefd;
    char *storage;
    const char *relay_fields;
    bool cache_forbidden;
    bool from_cache;
    unsigned int cached_age;
};

static const char header_name_chars[] =
    "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static ssize_t receive_bytes(int connfd, char *buffer, size_t length)
{
    ssize_t received;
    do {
        received = recv(connfd, buffer, length, 0);
    } while (received == -1 && errno == EINTR);
    if (received == -1) {
        perror("recv");
    }
    return received;
}

static const char *request_target_error(const char *buffer)
{
    const size_t method_length = strcspn(buffer, " \r\n");
    if (method_length == 0 || buffer[method_length] != ' ') {
        return NULL;
    }
    const char *target = buffer + method_length + 1;
    return strcspn(target, " \r\n") > MAX_TARGET_SIZE ? "414 URI Too Long" : NULL;
}

static ssize_t read_request_headers(int connfd, char *buffer, size_t capacity, char **body_start,
                                    const char **error)
{
    size_t used = 0;

    while (used < capacity - 1) {
        ssize_t received = receive_bytes(connfd, buffer + used, capacity - 1 - used);
        if (received <= 0) {
            return -1;
        }

        used += (size_t)received;
        buffer[used] = '\0';
        *error = request_target_error(buffer);
        if (*error != NULL) {
            return -1;
        }
        char *header_end = strstr(buffer, "\r\n\r\n");
        if (header_end != NULL) {
            *body_start = header_end + 4;
            return (ssize_t)used;
        }
        if (memchr(buffer, '\0', used) != NULL) {
            return -1;
        }
    }
    return -1;
}

static bool parse_request_line(char *buffer, struct request_line *request, char **fields)
{
    char *line_end = strstr(buffer, "\r\n");
    if (line_end == NULL) {
        return false;
    }
    *line_end = '\0';
    for (const unsigned char *byte = (const unsigned char *)buffer; *byte != '\0'; ++byte) {
        if (*byte < ' ' || *byte > '~') {
            return false;
        }
    }

    char *method_end = strchr(buffer, ' ');
    if (method_end == NULL || method_end == buffer) {
        return false;
    }
    char *target = method_end + 1;
    char *target_end = strchr(target, ' ');
    if (target_end == NULL || target_end == target) {
        return false;
    }
    char *version = target_end + 1;
    if (*version == '\0' || strchr(version, ' ') != NULL) {
        return false;
    }

    *method_end = '\0';
    *target_end = '\0';
    *request = (struct request_line){ .method = buffer, .target = target, .version = version };
    *fields = line_end + 2;
    return true;
}

/* Returns an HTTP error status, or NULL when the fields were read successfully. */
static const char *parse_header_fields(char *field, struct request_headers *headers)
{
    const char *bad_request = "400 Bad Request";
    while (strncmp(field, "\r\n", 2) != 0) {
        char *line_end = strstr(field, "\r\n");
        if (line_end == NULL) {
            return bad_request;
        }
        *line_end = '\0';
        char *colon = strchr(field, ':');
        if (colon == NULL || colon == field) {
            return bad_request;
        }
        *colon = '\0';
        if (strspn(field, header_name_chars) != strlen(field)) {
            return bad_request;
        }

        char *value = colon + 1;
        value += strspn(value, " \t");
        char *end = line_end;
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        for (const unsigned char *byte = (const unsigned char *)value; *byte != '\0'; ++byte) {
            if ((*byte < ' ' && *byte != '\t') || *byte == 127) {
                return bad_request;
            }
        }

        if (strcasecmp(field, "Content-Length") == 0) {
            if (headers->has_content_length || *value == '\0' ||
                strspn(value, "0123456789") != strlen(value)) {
                return bad_request;
            }
            errno = 0;
            unsigned long length = strtoul(value, NULL, 10);
            if (errno == ERANGE || length > MAX_BODY_SIZE) {
                return "413 Content Too Large";
            }
            headers->has_content_length = true;
            headers->content_length = (size_t)length;
        } else if (strcasecmp(field, "Host") == 0) {
            if (headers->host != NULL || *value == '\0' || strpbrk(value, " \t") != NULL) {
                return bad_request;
            }
            headers->host = value;
        } else if (strcasecmp(field, "Connection") == 0) {
            headers->connection = value;
        } else if (strcasecmp(field, "Content-Type") == 0) {
            headers->content_type = value;
        } else if (strcasecmp(field, "Transfer-Encoding") == 0) {
            return bad_request;
        }
        field = line_end + 2;
    }
    return NULL;
}

static bool receive_body(int connfd, char *body, size_t remaining)
{
    while (remaining > 0) {
        ssize_t received = receive_bytes(connfd, body, remaining);
        if (received <= 0) {
            return false;
        }
        body += received;
        remaining -= (size_t)received;
    }
    return true;
}

static const char *parse_request(char *buffer, struct request_line *request,
                                 struct request_headers *headers)
{
    char *fields;
    if (!parse_request_line(buffer, request, &fields)) {
        return "400 Bad Request";
    }
    const bool http_1_1 = strcmp(request->version, "HTTP/1.1") == 0;
    if (!http_1_1 && strcmp(request->version, "HTTP/1.0") != 0) {
        return "505 HTTP Version Not Supported";
    }
    if ((config.forward || config.route_count != 0) && strncmp(fields, "\r\n", 2) != 0) {
        const char *end = strstr(fields, "\r\n\r\n");
        if (end == NULL) {
            return "400 Bad Request";
        }
        const size_t length = (size_t)(end + 2 - fields);
        memcpy(headers->relay_fields, fields, length);
        headers->relay_fields[length] = '\0';
    }
    const char *error = parse_header_fields(fields, headers);
    if (error != NULL) {
        return error;
    }
    if (http_1_1 && headers->host == NULL) {
        return "400 Bad Request";
    }

    printf("Request: method=%s target=%s version=%s\n",
           request->method, request->target, request->version);
    printf("Headers: Host=%s Content-Length=%zu Connection=%s\n",
           headers->host != NULL ? headers->host : "(none)", headers->content_length,
           headers->connection != NULL ? headers->connection : "(none)");
    fflush(stdout);
    if (strcmp(request->method, "POST") == 0 && !headers->has_content_length) {
        return "411 Length Required";
    }
    return NULL;
}

static bool send_all(int connfd, const char *bytes, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t written = send(connfd, bytes + sent, length - sent, 0);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("send");
            return false;
        }
        if (written == 0) {
            return false;
        }
        sent += (size_t)written;
    }
    return true;
}

static size_t format_response_headers(char headers[RESPONSE_HEADER_SIZE],
                                      const struct response *response)
{
    const bool bodyless = strncmp(response->status, "204", 3) == 0 || strncmp(response->status, "304", 3) == 0;
    char content_length[64] = "";
    if (!bodyless) {
        snprintf(content_length, sizeof(content_length), "Content-Length: %zu\r\n", response->length);
    }
    char age[32] = "";
    if (response->from_cache) {
        snprintf(age, sizeof(age), "Age: %u\r\n", response->cached_age);
    }
    const int length = snprintf(headers, RESPONSE_HEADER_SIZE,
                                 "HTTP/1.1 %s%s\r\nContent-Type: %s\r\n%sConnection: close\r\n%s%s\r\n",
                                 response->status, strlen(response->status) == 3 ? " " : "",
                                 response->content_type, content_length, age,
                                 response->relay_fields != NULL ? response->relay_fields : "");
    return length > 0 && length < RESPONSE_HEADER_SIZE ? (size_t)length : 0;
}

static struct response text_response(const char *status, const char *body, size_t length)
{
    return (struct response){ .status = status, .content_type = "text/plain", .body = body, .length = length, .filefd = -1 };
}

static struct response prepare_reverse_response(unsigned int port, const struct request_line *request,
                                                const struct request_headers *headers, const char *body);
static struct response prepare_forward_response(const struct request_line *request,
                                                const struct request_headers *headers, const char *body);
static unsigned int parse_port(const char *value);

static int hex_value(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

static bool read_static_path(const char *target, char *path, size_t capacity)
{
    size_t length = strcspn(target, "?");
    if (target[0] != '/' || length >= capacity) {
        return false;
    }

    size_t written = 0;
    for (size_t i = 1; i < length; ++i) {
        unsigned char byte = (unsigned char)target[i];
        if (byte == '#') {
            return false;
        }
        if (byte == '%') {
            if (length - i < 3) {
                return false;
            }
            int high = hex_value((unsigned char)target[i + 1]);
            int low = hex_value((unsigned char)target[i + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            byte = (unsigned char)(high * 16 + low);
            if (byte == '/') {
                return false;
            }
            i += 2;
        }
        if (byte < ' ' || byte == 127 || byte == '\\') {
            return false;
        }
        path[written++] = (char)byte;
    }
    path[written] = '\0';
    return true;
}

static const char *static_content_type(const char *path)
{
    const char *filename = strrchr(path, '/');
    const char *extension = strrchr(filename != NULL ? filename + 1 : path, '.');
    if (extension != NULL) {
        if (strcasecmp(extension, ".html") == 0 || strcasecmp(extension, ".htm") == 0) {
            return "text/html";
        }
        if (strcasecmp(extension, ".css") == 0) {
            return "text/css";
        }
        if (strcasecmp(extension, ".js") == 0) {
            return "text/javascript";
        }
        if (strcasecmp(extension, ".png") == 0) {
            return "image/png";
        }
        if (strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0) {
            return "image/jpeg";
        }
        if (strcasecmp(extension, ".txt") == 0) {
            return "text/plain";
        }
    }
    return "application/octet-stream";
}

static const char *static_open_error(int error)
{
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        return "404 Not Found";
    case EACCES:
    case EPERM:
    case ELOOP:
        return "403 Forbidden";
    default:
        return "500 Internal Server Error";
    }
}

static int open_static_file(char *path, const char **error_status)
{
    int directory = open(config.root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory == -1) {
        perror("open static root");
        *error_status = "500 Internal Server Error";
        return -1;
    }

    /* Open each component relative to the preceding, verified directory. */
    for (char *component = path; ; ) {
        char *slash = strchr(component, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
        if (*component == '\0' || strcmp(component, ".") == 0 ||
            strcmp(component, "..") == 0) {
            close(directory);
            *error_status = "403 Forbidden";
            return -1;
        }

        const int mode = O_RDONLY | O_NOFOLLOW | (slash != NULL ? O_DIRECTORY : O_NONBLOCK);
        int descriptor = openat(directory, component, mode);
        const int open_error = errno;
        close(directory);
        if (descriptor == -1) {
            *error_status = static_open_error(open_error);
            return -1;
        }
        if (slash == NULL) {
            return descriptor;
        }
        directory = descriptor;
        component = slash + 1;
    }
}

static struct response prepare_static_response(const char *target)
{
    char path[MAX_HEADER_SIZE];
    if (!read_static_path(target, path, sizeof(path))) {
        return text_response("400 Bad Request", "", 0);
    }
    if (*path == '\0') {
        return text_response("200 OK", "hello", sizeof("hello") - 1);
    }
    const char *content_type = static_content_type(path);
    const char *error_status = NULL;
    int filefd = open_static_file(path, &error_status);
    if (filefd == -1) {
        return text_response(error_status, "", 0);
    }

    struct stat file_info;
    if (fstat(filefd, &file_info) == -1 || file_info.st_size < 0 ||
        (uintmax_t)file_info.st_size > SIZE_MAX) {
        close(filefd);
        return text_response("500 Internal Server Error", "", 0);
    }
    if (!S_ISREG(file_info.st_mode)) {
        close(filefd);
        return text_response("403 Forbidden", "", 0);
    }
    return (struct response){ .status = "200 OK", .content_type = content_type,
                              .length = (size_t)file_info.st_size, .filefd = filefd };
}

static unsigned int reverse_port(const char *target)
{
    struct reverse_route *matched = NULL;
    for (size_t i = 0; i < config.route_count; ++i) {
        struct reverse_route *route = &config.routes[i];
        if ((matched == NULL || route->length > matched->length) &&
            strncmp(target, route->prefix, route->length) == 0) {
            matched = route;
        }
    }
    if (matched == NULL) {
        return 0;
    }
    if (matched->port_count == 1) {
        return matched->ports[0];
    }

    pthread_mutex_lock(&reverse_route_lock);
    const unsigned int port = matched->ports[matched->next];
    matched->next = (matched->next + 1) % matched->port_count;
    pthread_mutex_unlock(&reverse_route_lock);
    return port;
}

static struct response prepare_cached_proxy_response(bool forward, const struct request_line *request,
                                                     const struct request_headers *headers, const char *body,
                                                     bool *proxied);

static struct response prepare_response(const struct request_line *request,
                                        const struct request_headers *headers, const char *body)
{
    const bool is_get = strcmp(request->method, "GET") == 0;
    if (!is_get && strcmp(request->method, "POST") != 0) {
        return text_response("501 Not Implemented", "", 0);
    }
    const bool forward = config.forward && request->target[0] != '/';
    if (config.cache && (forward || config.route_count != 0)) {
        bool proxied;
        struct response response = prepare_cached_proxy_response(forward, request, headers, body, &proxied);
        if (proxied) {
            return response;
        }
    } else if (forward) {
        return prepare_forward_response(request, headers, body);
    } else {
        const unsigned int backend = reverse_port(request->target);
        if (backend != 0) {
            return prepare_reverse_response(backend, request, headers, body);
        }
    }
    if (is_get) {
        return prepare_static_response(request->target);
    }
    if (strcmp(request->target, "/echo") != 0) {
        return text_response("404 Not Found", "", 0);
    }
    printf("Body: received %zu bytes\n", headers->content_length);
    fflush(stdout);
    return text_response("200 OK", body, headers->content_length);
}

static ssize_t read_file_chunk(int filefd, char chunk[FILE_CHUNK_SIZE], size_t remaining)
{
    const size_t length = remaining < FILE_CHUNK_SIZE ? remaining : FILE_CHUNK_SIZE;
    ssize_t count;
    do {
        count = read(filefd, chunk, length);
    } while (count == -1 && errno == EINTR);
    if (count == -1) {
        perror("read static file");
    }
    return count;
}

static void send_response(int connfd, struct response response)
{
    char headers[RESPONSE_HEADER_SIZE];
    const size_t header_length = format_response_headers(headers, &response);
    if (header_length > 0 && send_all(connfd, headers, header_length)) {
        if (response.filefd == -1) {
            send_all(connfd, response.body, response.length);
        } else {
            char chunk[FILE_CHUNK_SIZE];
            size_t remaining = response.length;
            while (remaining > 0) {
                const ssize_t count = read_file_chunk(response.filefd, chunk, remaining);
                if (count <= 0 || !send_all(connfd, chunk, (size_t)count)) {
                    break;
                }
                remaining -= (size_t)count;
            }
        }
    }
    if (response.filefd != -1) {
        close(response.filefd);
    }
    free(response.storage);
}

static void handle_request(int connfd)
{
    char buffer[MAX_HEADER_SIZE];
    char *body_start;
    const char *error = NULL;
    ssize_t received = read_request_headers(connfd, buffer, sizeof(buffer), &body_start, &error);
    if (received < 0) {
        if (error != NULL) {
            send_response(connfd, text_response(error, "", 0));
        }
        return;
    }

    struct request_line request;
    struct request_headers headers = {0};
    error = parse_request(buffer, &request, &headers);
    if (error != NULL) {
        send_response(connfd, text_response(error, "", 0));
        return;
    }

    char body[MAX_BODY_SIZE];
    size_t body_received = (size_t)received - (size_t)(body_start - buffer);
    if (body_received > headers.content_length) {
        body_received = headers.content_length;
    }
    memcpy(body, body_start, body_received);
    if (!receive_body(connfd, body + body_received, headers.content_length - body_received)) {
        return;
    }

    send_response(connfd, prepare_response(&request, &headers, body));
}

struct poll_client {
    char request_bytes[MAX_HEADER_SIZE + MAX_BODY_SIZE];
    struct request_line request;
    struct request_headers headers;
    size_t received;
    size_t body_offset;
    size_t request_length;
    struct response response;
    char response_headers[RESPONSE_HEADER_SIZE];
    size_t header_length;
    size_t header_sent;
    size_t body_sent;
    char file_chunk[FILE_CHUNK_SIZE];
    size_t chunk_length;
    size_t chunk_sent;
};

static bool set_nonblocking(int fd, bool enabled)
{
    const int flags = fcntl(fd, F_GETFL);
    return flags != -1 && fcntl(fd, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) != -1;
}

static bool should_retry_io(void)
{
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

static int64_t monotonic_ms(void)
{
    struct timespec moment;
    if (clock_gettime(CLOCK_MONOTONIC, &moment) == -1) {
        return -1;
    }
    return (int64_t)moment.tv_sec * 1000 + moment.tv_nsec / 1000000;
}

static bool wait_backend(int fd, short events, int64_t deadline)
{
    struct pollfd socket = { .fd = fd, .events = events };
    for (;;) {
        const int64_t now = monotonic_ms();
        if (now == -1) {
            return false;
        }
        const int64_t remaining = deadline - now;
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return false;
        }
        const int ready = poll(&socket, 1, (int)remaining);
        if (ready > 0) {
            return true;
        }
        if (ready == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

static bool connect_backend(int fd, const struct sockaddr *address, socklen_t length, int64_t deadline)
{
    if (connect(fd, address, length) == 0) {
        return true;
    }
    if ((errno != EINPROGRESS && errno != EINTR) || !wait_backend(fd, POLLOUT, deadline)) {
        return false;
    }
    int error = 0;
    socklen_t size = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == -1) {
        return false;
    }
    errno = error;
    return error == 0;
}

static int open_backend_socket(const struct sockaddr *address, socklen_t length, int64_t deadline)
{
    const int fd = socket(address->sa_family, SOCK_STREAM, 0);
    if (fd != -1 && (!set_nonblocking(fd, true) || !connect_backend(fd, address, length, deadline))) {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static bool send_backend_bytes(int fd, const char *bytes, size_t length, int64_t deadline)
{
    size_t sent = 0;
    while (sent < length && wait_backend(fd, POLLOUT, deadline)) {
        const ssize_t count = send(fd, bytes + sent, length - sent, 0);
        if (count == -1 && should_retry_io()) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        sent += (size_t)count;
    }
    return sent == length;
}

static bool valid_backend_text(const char *value)
{
    for (const unsigned char *byte = (const unsigned char *)value; *byte != '\0'; ++byte) {
        if ((*byte < ' ' && *byte != '\t') || *byte == 127) {
            return false;
        }
    }
    return true;
}

static bool header_named(const char *start, size_t length, const char *name)
{
    return strlen(name) == length && strncasecmp(start, name, length) == 0;
}

static bool is_connection_option(const char *fields, const char *name, size_t length)
{
    for (const char *line = fields; *line != '\0';) {
        const char *end = strstr(line, "\r\n");
        const char *colon = strchr(line, ':');
        if (header_named(line, (size_t)(colon - line), "Connection")) {
            const char *option = colon + 1;
            while (option < end) {
                option += strspn(option, " ,\t");
                const size_t option_length = strcspn(option, " ,\t\r");
                if (option_length == length && strncasecmp(option, name, length) == 0) {
                    return true;
                }
                option += option_length;
            }
        }
        line = end + 2;
    }
    return false;
}

static void copy_end_to_end_headers(const char *fields, char output[MAX_HEADER_SIZE + 1])
{
    static const char *const handled[] = {
        "Host", "Content-Length", "Content-Type", "Connection", "Proxy-Connection", "Keep-Alive",
        "Transfer-Encoding", "TE", "Trailer", "Upgrade", "Proxy-Authenticate", "Proxy-Authorization", "Expect",
    };
    size_t used = 0;
    /* The caller has validated that each stored line ends in CRLF and contains a colon. */
    for (const char *line = fields; *line != '\0';) {
        const char *end = strstr(line, "\r\n") + 2;
        const size_t name_length = (size_t)(strchr(line, ':') - line);
        bool skip = false;
        for (size_t i = 0; i < sizeof(handled) / sizeof(handled[0]); ++i) {
            if (header_named(line, name_length, handled[i])) {
                skip = true;
                break;
            }
        }
        if (!skip && !is_connection_option(fields, line, name_length)) {
            const size_t length = (size_t)(end - line);
            memcpy(output + used, line, length);
            used += length;
        }
        line = end;
    }
    output[used] = '\0';
}

static bool parse_backend_headers(char *buffer, char *end, struct response *response, bool *framed)
{
    char *line_end = strstr(buffer, "\r\n");
    if (line_end == NULL || line_end - buffer < 12 ||
        (strncmp(buffer, "HTTP/1.0 ", 9) != 0 && strncmp(buffer, "HTTP/1.1 ", 9) != 0)) {
        return false;
    }
    char fields[MAX_HEADER_SIZE + 1];
    const size_t field_length = end == line_end ? 0 : (size_t)(end - line_end);
    memcpy(fields, line_end + 2, field_length);
    fields[field_length] = '\0';
    *line_end = '\0';
    const char *status = buffer + 9;
    if (status[0] < '2' || status[0] > '5' || strspn(status, "0123456789") != 3 ||
        (status[3] != '\0' && status[3] != ' ') || !valid_backend_text(status)) {
        return false;
    }
    response->status = status;
    const bool bodyless = strncmp(status, "204", 3) == 0 || strncmp(status, "304", 3) == 0;
    for (char *field = line_end + 2; field < end;) {
        char *next = strstr(field, "\r\n");
        if (next == NULL) {
            return false;
        }
        *next = '\0';
        char *colon = strchr(field, ':');
        if (colon == NULL || colon == field) {
            return false;
        }
        *colon = '\0';
        if (strspn(field, header_name_chars) != strlen(field)) {
            return false;
        }
        char *value = colon + 1 + strspn(colon + 1, " \t");
        char *tail = next;
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) {
            *--tail = '\0';
        }
        if (!valid_backend_text(value)) {
            return false;
        }
        if (strcasecmp(field, "Content-Length") == 0) {
            if (*framed || *value == '\0' || strspn(value, "0123456789") != strlen(value)) {
                return false;
            }
            errno = 0;
            const unsigned long long length = strtoull(value, NULL, 10);
            if (errno == ERANGE || (!bodyless && length > BACKEND_RESPONSE_LIMIT)) {
                return false;
            }
            *framed = true;
            response->length = bodyless ? 0 : (size_t)length;
        } else if (strcasecmp(field, "Content-Type") == 0) {
            if (*value == '\0' || !valid_backend_text(value)) {
                return false;
            }
            response->content_type = value;
        } else if (strcasecmp(field, "Transfer-Encoding") == 0) {
            /* This first HTTP/1.0 upstream hop supports fixed lengths and close-delimited bodies. */
            return false;
        }
        if (strcasecmp(field, "Set-Cookie") == 0 || strcasecmp(field, "Vary") == 0 ||
            strcasecmp(field, "Content-Range") == 0) {
            response->cache_forbidden = true;
        }
        field = next + 2;
    }
    char *relay_fields = response->storage + MAX_HEADER_SIZE + BACKEND_RESPONSE_LIMIT + 1;
    copy_end_to_end_headers(fields, relay_fields);
    response->relay_fields = relay_fields;
    if (is_connection_option(fields, "Content-Type", sizeof("Content-Type") - 1)) {
        response->content_type = "application/octet-stream";
    }
    *framed |= bodyless;
    return true;
}

static struct response backend_failure(int error)
{
    return text_response(error == ETIMEDOUT ? "504 Gateway Timeout" : "502 Bad Gateway", "", 0);
}

static struct response read_backend_response(int fd, int64_t deadline)
{
    const size_t capacity = MAX_HEADER_SIZE + BACKEND_RESPONSE_LIMIT;
    struct response response = {
        .content_type = "application/octet-stream", .filefd = -1, .storage = malloc(capacity + MAX_HEADER_SIZE + 2),
    };
    if (response.storage == NULL) {
        return text_response("500 Internal Server Error", "", 0);
    }
    size_t used = 0;
    size_t body_offset = 0;
    bool framed = false;
    int error = 0;
    for (;;) {
        if (body_offset != 0 && framed && used - body_offset >= response.length) {
            break;
        }
        const size_t limit = body_offset == 0 ? MAX_HEADER_SIZE
            : body_offset + (framed ? response.length : BACKEND_RESPONSE_LIMIT + 1);
        if (used >= limit) {
            break;
        }
        if (!wait_backend(fd, POLLIN, deadline)) {
            error = errno;
            break;
        }
        const ssize_t count = recv(fd, response.storage + used, limit - used, 0);
        if (count == -1 && should_retry_io()) {
            continue;
        }
        if (count <= 0) {
            if (count == 0 && body_offset != 0 && !framed && used - body_offset <= BACKEND_RESPONSE_LIMIT) {
                framed = true;
                response.length = used - body_offset;
            }
            break;
        }
        used += (size_t)count;
        if (body_offset == 0) {
            response.storage[used] = '\0';
            char *end = strstr(response.storage, "\r\n\r\n");
            if (end != NULL) {
                if (!parse_backend_headers(response.storage, end, &response, &framed)) {
                    break;
                }
                body_offset = (size_t)(end - response.storage) + 4;
            } else if (memchr(response.storage, '\0', used) != NULL) {
                break;
            }
        }
    }
    if (body_offset != 0 && framed && used - body_offset >= response.length) {
        char header_check[RESPONSE_HEADER_SIZE];
        response.body = response.storage + body_offset;
        if (format_response_headers(header_check, &response) > 0) {
            return response;
        }
    }
    free(response.storage);
    return backend_failure(error);
}

static struct response relay_proxy_request(int fd, int64_t deadline, const char *host,
                                           const struct request_line *request,
                                           const struct request_headers *headers, const char *body)
{
    char message[MAX_HEADER_SIZE + 256];
    char relay_fields[MAX_HEADER_SIZE + 1];
    copy_end_to_end_headers(headers->relay_fields, relay_fields);
    const char *content_type = is_connection_option(headers->relay_fields, "Content-Type", sizeof("Content-Type") - 1)
        ? NULL : headers->content_type;
    const int length = snprintf(message, sizeof(message),
        "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nContent-Length: %zu\r\n%s%s%s%s\r\n",
        request->method, request->target, host, headers->content_length,
        content_type != NULL ? "Content-Type: " : "", content_type != NULL ? content_type : "",
        content_type != NULL ? "\r\n" : "", relay_fields);
    if (length <= 0 || (size_t)length >= sizeof(message)) {
        return text_response("400 Bad Request", "", 0);
    }
    if (!send_backend_bytes(fd, message, (size_t)length, deadline) ||
        !send_backend_bytes(fd, body, headers->content_length, deadline)) {
        return backend_failure(errno);
    }
    return read_backend_response(fd, deadline);
}

static struct response prepare_reverse_response(unsigned int port, const struct request_line *request,
                                                const struct request_headers *headers, const char *body)
{
    const int64_t start = monotonic_ms();
    if (start == -1) {
        return backend_failure(errno);
    }
    const int64_t deadline = start + BACKEND_TIMEOUT_MS;
    const struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    const int fd = open_backend_socket((const struct sockaddr *)&address, sizeof(address), deadline);
    if (fd == -1) {
        return backend_failure(errno);
    }
    char backend_host[32];
    snprintf(backend_host, sizeof(backend_host), "127.0.0.1:%u", port);
    const char *host = headers->host != NULL ? headers->host : backend_host;
    const struct response response = relay_proxy_request(fd, deadline, host, request, headers, body);
    close(fd);
    return response;
}

struct forward_target {
    char host[256];
    char authority[264];
    unsigned int port;
    const char *path;
};

static bool parse_forward_target(const char *url, struct forward_target *target)
{
    if (strncasecmp(url, "http://", 7) != 0 || strchr(url, '#') != NULL) {
        return false;
    }
    const char *authority = url + 7;
    const size_t length = strcspn(authority, "/?");
    if (length == 0 || length >= sizeof(target->authority)) {
        return false;
    }
    memcpy(target->authority, authority, length);
    target->authority[length] = '\0';
    target->path = authority + length;
    target->port = 80;

    const bool ipv6 = target->authority[0] == '[';
    const char *host = target->authority + ipv6;
    const char *host_end = strchr(host, ipv6 ? ']' : ':');
    const char *port = host_end;
    if (ipv6) {
        if (host_end == NULL) {
            return false;
        }
        port = host_end + 1;
    } else if (host_end == NULL) {
        host_end = target->authority + length;
    }
    const size_t host_length = (size_t)(host_end - host);
    if (host_length == 0 || host_length >= sizeof(target->host)) {
        return false;
    }
    memcpy(target->host, host, host_length);
    target->host[host_length] = '\0';
    if (ipv6) {
        struct in6_addr address;
        if (inet_pton(AF_INET6, target->host, &address) != 1) {
            return false;
        }
    } else if (strspn(target->host, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-") != host_length) {
        return false;
    }
    if (port != NULL && *port != '\0') {
        if (*port != ':') {
            return false;
        }
        target->port = parse_port(port + 1);
    }
    return target->port != 0;
}

static bool is_own_forward_address(const struct addrinfo *info, unsigned int port)
{
    if (port != config.port) {
        return false;
    }
    if (info->ai_family == AF_INET) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)info->ai_addr;
        return address->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    }
    if (info->ai_family == AF_INET6) {
        const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)info->ai_addr;
        const uint32_t loopback = htonl(INADDR_LOOPBACK);
        return IN6_IS_ADDR_V4MAPPED(&address->sin6_addr) &&
            memcmp(address->sin6_addr.s6_addr + 12, &loopback, sizeof(loopback)) == 0;
    }
    return false;
}

static struct response prepare_forward_response(const struct request_line *request,
                                                const struct request_headers *headers, const char *body)
{
    struct forward_target target;
    if (!parse_forward_target(request->target, &target)) {
        return text_response("400 Bad Request", "", 0);
    }
    char query_path[MAX_HEADER_SIZE];
    struct request_line outgoing = *request;
    outgoing.target = *target.path == '\0' ? "/" : target.path;
    if (*target.path == '?') {
        snprintf(query_path, sizeof(query_path), "/%s", target.path);
        outgoing.target = query_path;
    }

    char service[6];
    snprintf(service, sizeof(service), "%u", target.port);
    const struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV };
    struct addrinfo *addresses;
    if (getaddrinfo(target.host, service, &hints, &addresses) != 0) {
        return backend_failure(0);
    }
    const int64_t start = monotonic_ms();
    if (start == -1) {
        freeaddrinfo(addresses);
        return backend_failure(0);
    }
    const int64_t deadline = start + BACKEND_TIMEOUT_MS;
    int fd = -1;
    int error = 0;
    bool points_to_self = false;
    for (const struct addrinfo *info = addresses; info != NULL; info = info->ai_next) {
        if (is_own_forward_address(info, target.port)) {
            points_to_self = true;
            continue;
        }
        fd = open_backend_socket(info->ai_addr, info->ai_addrlen, deadline);
        if (fd != -1) {
            break;
        }
        error = errno;
        if (error == ETIMEDOUT) {
            break;
        }
    }
    freeaddrinfo(addresses);
    if (fd == -1) {
        return points_to_self && error != ETIMEDOUT ? text_response("400 Bad Request", "", 0) : backend_failure(error);
    }
    const struct response response = relay_proxy_request(fd, deadline, target.authority, &outgoing, headers, body);
    close(fd);
    return response;
}

struct proxy_cache_entry {
    char host[MAX_HEADER_SIZE + 1];
    char target[MAX_TARGET_SIZE + 1];
    struct response response;
    int64_t stored_at;
    int64_t initial_age;
    int64_t expires_at;
};

static struct proxy_cache_entry proxy_cache[CACHE_ENTRIES];
static pthread_mutex_t proxy_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t proxy_cache_victim;
static uint64_t proxy_cache_generation;

static bool cache_request_allowed(const struct request_line *request, const struct request_headers *headers)
{
    if (strcmp(request->method, "GET") != 0 || headers->content_length != 0) {
        return false;
    }
    static const char *const allowed[] = {
        "Host", "User-Agent", "Accept", "Connection", "Proxy-Connection", "Content-Length"
    };
    for (const char *line = headers->relay_fields; *line != '\0'; line = strstr(line, "\r\n") + 2) {
        const char *colon = strchr(line, ':');
        const size_t name_length = (size_t)(colon - line);
        bool accepted = false;
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
            if (header_named(line, name_length, allowed[i])) {
                accepted = true;
                break;
            }
        }
        if (!accepted && header_named(line, name_length, "Accept-Encoding")) {
            const char *value = colon + 1 + strspn(colon + 1, " \t");
            if (strncasecmp(value, "identity", 8) == 0 &&
                value[8 + strspn(value + 8, " \t")] == '\r') {
                accepted = true;
            }
        }
        if (!accepted) {
            return false;
        }
    }
    return true;
}

static bool cache_seconds(const char *value, const char *limit, unsigned int *seconds)
{
    if (*value < '0' || *value > '9') {
        return false;
    }
    char *end;
    const unsigned long number = strtoul(value, &end, 10);
    if (end + strspn(end, " \t") != limit || number > INT32_MAX) {
        return false;
    }
    *seconds = (unsigned int)number;
    return true;
}

static unsigned int cache_lifetime(const char *value, const char *end)
{
    bool public = false;
    unsigned int max_age = 0, shared_age = 0;
    bool has_max_age = false, has_shared_age = false;
    while (value < end) {
        value += strspn(value, " \t");
        const char *comma = memchr(value, ',', (size_t)(end - value));
        const char *limit = comma != NULL ? comma : end;
        const size_t length = strcspn(value, " =,\t\r");
        const char *argument = value + length + strspn(value + length, " \t");
        if (header_named(value, length, "public") && argument == limit) {
            public = true;
        } else if (header_named(value, length, "max-age") && !has_max_age && *argument == '=' &&
                   cache_seconds(argument + 1 + strspn(argument + 1, " \t"), limit, &max_age)) {
            has_max_age = true;
        } else if (header_named(value, length, "s-maxage") && !has_shared_age && *argument == '=' &&
                   cache_seconds(argument + 1 + strspn(argument + 1, " \t"), limit, &shared_age)) {
            has_shared_age = true;
        } else {
            return 0;
        }
        value = comma != NULL ? comma + 1 : end;
    }
    const unsigned int age = has_shared_age ? shared_age : max_age;
    return public ? (age < CACHE_TTL_SECONDS ? age : CACHE_TTL_SECONDS) : 0;
}

static int64_t cache_date_age(const char *value)
{
    struct tm stamp = {0};
    const char *end = strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &stamp);
    const time_t now = time(NULL);
    if (end == NULL || *end != '\r' || stamp.tm_year < 70 || now == (time_t)-1) {
        return -1;
    }

    /* Gregorian days since 1970, using UTC without changing the process timezone. */
    const int year = stamp.tm_year + 1900;
    const int last = year - 1;
    const int base_year = 1969;
    const int leaps = last / 4 - last / 100 + last / 400
        - (base_year / 4 - base_year / 100 + base_year / 400);
    const int64_t days = (year - 1970) * INT64_C(365) + leaps + stamp.tm_yday;
    const int64_t seconds = days * 86400 + stamp.tm_hour * 3600 + stamp.tm_min * 60 + stamp.tm_sec;
    return now > seconds ? ((int64_t)now - seconds) * 1000 : 0;
}

static struct response copy_cached_response(const struct response *source, const char *age_field)
{
    const size_t status_bytes = strlen(source->status) + 1;
    const size_t type_bytes = strlen(source->content_type) + 1;
    const size_t field_bytes = strlen(source->relay_fields) + 1;
    char *storage = malloc(status_bytes + type_bytes + field_bytes + source->length);
    if (storage == NULL) {
        return (struct response){0};
    }
    char *type = storage + status_bytes;
    char *fields = type + type_bytes;
    char *body = fields + field_bytes;
    memcpy(storage, source->status, status_bytes);
    memcpy(type, source->content_type, type_bytes);
    memcpy(fields, source->relay_fields, field_bytes);
    memcpy(body, source->body, source->length);
    if (age_field != NULL) {
        char *start = fields + (age_field - source->relay_fields);
        const char *end = strstr(start, "\r\n") + 2;
        memmove(start, end, strlen(end) + 1);
    }
    return (struct response){ .status = storage, .content_type = type, .body = body,
        .length = source->length, .filefd = -1, .storage = storage, .relay_fields = fields };
}

static bool cached_key_matches(const struct proxy_cache_entry *entry, const char *host, const char *target)
{
    return entry->response.storage != NULL && strcmp(entry->host, host) == 0 &&
        strcmp(entry->target, target) == 0;
}

static struct response find_cached_response(const char *host, const char *target, int64_t now,
                                            uint64_t *generation)
{
    struct response hit = {0};
    pthread_mutex_lock(&proxy_cache_lock);
    *generation = proxy_cache_generation;
    for (size_t i = 0; i < CACHE_ENTRIES; ++i) {
        const struct proxy_cache_entry *entry = &proxy_cache[i];
        if (now < entry->expires_at && cached_key_matches(entry, host, target)) {
            hit = copy_cached_response(&entry->response, NULL);
            hit.from_cache = hit.storage != NULL;
            hit.cached_age = (unsigned int)((entry->initial_age + now - entry->stored_at) / 1000);
            break;
        }
    }
    pthread_mutex_unlock(&proxy_cache_lock);
    return hit;
}

static void store_cached_response(const char *host, const char *target, const struct response *response,
                                  int64_t started, uint64_t generation)
{
    if (response->relay_fields == NULL || response->cache_forbidden ||
        strncmp(response->status, "200", 3) != 0 || response->length > CACHE_BODY_LIMIT) {
        return;
    }
    const char *age_field = NULL;
    unsigned int upstream_age = 0;
    unsigned int lifetime = 0;
    int64_t apparent_age = 0;
    bool found_policy = false, found_date = false;
    for (const char *line = response->relay_fields; *line != '\0'; line = strstr(line, "\r\n") + 2) {
        const char *colon = strchr(line, ':');
        const size_t length = (size_t)(colon - line);
        const char *value = colon + 1 + strspn(colon + 1, " \t");
        const char *end = strstr(line, "\r\n");
        if (header_named(line, length, "Cache-Control")) {
            if (found_policy) {
                return;
            }
            found_policy = true;
            lifetime = cache_lifetime(value, end);
        } else if (header_named(line, length, "Age")) {
            if (age_field != NULL || !cache_seconds(value, end, &upstream_age)) {
                return;
            }
            age_field = line;
        } else if (header_named(line, length, "Date")) {
            if (found_date || (apparent_age = cache_date_age(value)) < 0) {
                return;
            }
            found_date = true;
        }
    }
    const int64_t now = monotonic_ms();
    if (now < started || lifetime == 0) {
        return;
    }
    const int64_t corrected_age = upstream_age * INT64_C(1000) + now - started;
    const int64_t initial_age = apparent_age > corrected_age ? apparent_age : corrected_age;
    const int64_t remaining = lifetime * INT64_C(1000) - initial_age;
    if (remaining <= 0) {
        return;
    }
    struct response copy = copy_cached_response(response, age_field);
    if (copy.storage == NULL) {
        return;
    }
    pthread_mutex_lock(&proxy_cache_lock);
    if (generation == proxy_cache_generation) {
        struct proxy_cache_entry *entry = &proxy_cache[proxy_cache_victim];
        for (size_t i = 0; i < CACHE_ENTRIES; ++i) {
            if (cached_key_matches(&proxy_cache[i], host, target)) {
                entry = &proxy_cache[i];
                break;
            }
        }
        if (entry == &proxy_cache[proxy_cache_victim]) {
            proxy_cache_victim = (proxy_cache_victim + 1) % CACHE_ENTRIES;
        }
        free(entry->response.storage);
        strcpy(entry->host, host);
        strcpy(entry->target, target);
        entry->response = copy;
        entry->stored_at = now;
        entry->initial_age = initial_age;
        entry->expires_at = now + remaining;
        copy.storage = NULL;
    }
    pthread_mutex_unlock(&proxy_cache_lock);
    free(copy.storage);
}

static void clear_proxy_cache(void)
{
    pthread_mutex_lock(&proxy_cache_lock);
    ++proxy_cache_generation;
    for (size_t i = 0; i < CACHE_ENTRIES; ++i) {
        free(proxy_cache[i].response.storage);
        proxy_cache[i].response.storage = NULL;
        proxy_cache[i].expires_at = 0;
    }
    pthread_mutex_unlock(&proxy_cache_lock);
}

static struct response prepare_cached_proxy_response(bool forward, const struct request_line *request,
                                                     const struct request_headers *headers, const char *body,
                                                     bool *proxied)
{
    const char *host = forward ? "" : headers->host;
    const int64_t started = monotonic_ms();
    const bool cacheable = started >= 0 && host != NULL && cache_request_allowed(request, headers);
    uint64_t generation = 0;
    if (cacheable) {
        struct response hit = find_cached_response(host, request->target, started, &generation);
        if (hit.storage != NULL) {
            *proxied = true;
            printf("Cache HIT: %s\n", request->target);
            return hit;
        }
    }
    const unsigned int backend = forward ? 0 : reverse_port(request->target);
    *proxied = forward || backend != 0;
    if (!*proxied) {
        return (struct response){0};
    }
    printf("Cache %s: %s\n", cacheable ? "MISS" : "BYPASS", request->target);
    struct response response = forward ? prepare_forward_response(request, headers, body)
        : prepare_reverse_response(backend, request, headers, body);
    if (cacheable) {
        store_cached_response(host, request->target, &response, started, generation);
    } else if (strcmp(request->method, "POST") == 0 &&
               (response.status[0] == '2' || response.status[0] == '3')) {
        clear_proxy_cache();
    }
    return response;
}

static void wake_main(void)
{
    const int saved_errno = errno;
    const char byte = 0;
    if (wakeup_write != -1) {
        while (write(wakeup_write, &byte, 1) == -1 && errno == EINTR) {
        }
        /* A full nonblocking pipe already has a wakeup waiting. */
    }
    errno = saved_errno;
}

static void on_server_signal(int signum)
{
    const int saved_errno = errno;
    if (signum != SIGCHLD) {
        stop_requested = 1;
        if (blocking_client != -1) {
            /* Wake serial and forked clients blocked in socket I/O. */
            shutdown(blocking_client, SHUT_RDWR);
        }
    }
    wake_main();
    errno = saved_errno;
}

static void close_wakeup_pipe(void)
{
    const int writer = wakeup_write;
    wakeup_write = -1;
    close(writer);
    close(wakeup_read);
}

static bool setup_server_signals(bool use_fork)
{
    int wakeup[2];
    if (pipe(wakeup) == -1) {
        perror("pipe");
        return false;
    }
    wakeup_read = wakeup[0];
    wakeup_write = wakeup[1];
    if (!set_nonblocking(wakeup_read, true) || !set_nonblocking(wakeup_write, true)) {
        perror("fcntl wakeup pipe");
        close_wakeup_pipe();
        return false;
    }
    struct sigaction action = { .sa_handler = on_server_signal };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 || sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction shutdown");
        close_wakeup_pipe();
        return false;
    }
    action.sa_flags = SA_NOCLDSTOP;
    if (use_fork && sigaction(SIGCHLD, &action, NULL) == -1) {
        perror("sigaction SIGCHLD");
        close_wakeup_pipe();
        return false;
    }
    return true;
}

static void drain_wakeup_pipe(void)
{
    char bytes[32];
    while (read(wakeup_read, bytes, sizeof(bytes)) > 0) {
    }
}

static void handle_blocking_client(int connfd)
{
    blocking_client = connfd;
    if (!stop_requested) {
        handle_request(connfd);
    }
    blocking_client = -1;
}

static bool read_poll_request(int fd, struct poll_client *client)
{
    const size_t limit = client->body_offset == 0 ? MAX_HEADER_SIZE - 1 : client->request_length;
    const ssize_t count = recv(fd, client->request_bytes + client->received, limit - client->received, 0);
    if (count <= 0) {
        return count == -1 && should_retry_io();
    }
    client->received += (size_t)count;

    const char *error = NULL;
    if (client->body_offset == 0) {
        client->request_bytes[client->received] = '\0';
        error = request_target_error(client->request_bytes);
        if (error == NULL) {
            const char *header_end = strstr(client->request_bytes, "\r\n\r\n");
            if (header_end == NULL) {
                return client->received < limit && memchr(client->request_bytes, '\0', client->received) == NULL;
            }
            client->body_offset = (size_t)(header_end - client->request_bytes) + 4;
            error = parse_request(client->request_bytes, &client->request, &client->headers);
            client->request_length = client->body_offset + client->headers.content_length;
        }
    }
    if (error == NULL && client->received < client->request_length) {
        return true;
    }

    client->response = error != NULL ? text_response(error, "", 0)
        : prepare_response(&client->request, &client->headers, client->request_bytes + client->body_offset);
    client->header_length = format_response_headers(client->response_headers, &client->response);
    return client->header_length != 0;
}

static bool write_poll_bytes(int fd, const char *buffer, size_t length, size_t *sent)
{
    const ssize_t count = send(fd, buffer + *sent, length - *sent, 0);
    if (count <= 0) {
        return count == -1 && should_retry_io();
    }
    *sent += (size_t)count;
    return true;
}

static bool write_poll_response(int fd, struct poll_client *client)
{
    if (client->header_sent < client->header_length) {
        return write_poll_bytes(fd, client->response_headers, client->header_length, &client->header_sent)
            && (client->header_sent < client->header_length || client->response.length != 0);
    }
    if (client->response.filefd == -1) {
        return write_poll_bytes(fd, client->response.body, client->response.length, &client->body_sent)
            && client->body_sent < client->response.length;
    }

    if (client->chunk_sent == client->chunk_length) {
        const size_t remaining = client->response.length - client->body_sent;
        const ssize_t count = read_file_chunk(client->response.filefd, client->file_chunk, remaining);
        if (count <= 0) {
            return false;
        }
        client->chunk_length = (size_t)count;
        client->chunk_sent = 0;
    }
    const size_t before = client->chunk_sent;
    const bool sent = write_poll_bytes(fd, client->file_chunk, client->chunk_length, &client->chunk_sent);
    client->body_sent += client->chunk_sent - before;
    return sent && client->body_sent < client->response.length;
}

static void close_poll_client(struct pollfd *socket, struct poll_client *client)
{
    if (client->response.filefd != -1) {
        close(client->response.filefd);
    }
    free(client->response.storage);
    close(socket->fd);
    socket->fd = -1;
}

static int run_poll_server(int listenfd)
{
    struct poll_client *clients = calloc(POLL_CLIENT_CAPACITY, sizeof(*clients));
    if (clients == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }
    struct pollfd sockets[POLL_FIRST_CLIENT + POLL_CLIENT_CAPACITY] = {
        [POLL_LISTENER] = { .fd = listenfd },
        [POLL_WAKEUP] = { .fd = wakeup_read, .events = POLLIN },
    };
    const size_t socket_count = sizeof(sockets) / sizeof(sockets[0]);
    for (size_t i = POLL_FIRST_CLIENT; i < socket_count; ++i) {
        sockets[i].fd = -1;
    }
    size_t active = 0;

    while (!stop_requested) {
        sockets[POLL_LISTENER].events = active < POLL_CLIENT_CAPACITY ? POLLIN : 0;
        if (poll(sockets, socket_count, -1) == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (stop_requested) {
            break;
        }
        if (sockets[POLL_LISTENER].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "poll: listening socket failed\n");
            break;
        }
        for (size_t i = POLL_FIRST_CLIENT; i < socket_count && !stop_requested; ++i) {
            struct pollfd *socket = &sockets[i];
            struct poll_client *client = &clients[i - POLL_FIRST_CLIENT];
            const short ready = socket->revents;
            if (ready == 0) {
                continue;
            }
            bool keep = false;
            if (!(ready & (POLLERR | POLLNVAL))) {
                if (socket->events == POLLIN && (ready & (POLLIN | POLLHUP))) {
                    keep = read_poll_request(socket->fd, client);
                    if (client->header_length > 0) {
                        socket->events = POLLOUT;
                    }
                } else if (socket->events == POLLOUT && (ready & POLLOUT)) {
                    keep = write_poll_response(socket->fd, client);
                }
            }
            if (!keep) {
                close_poll_client(socket, client);
                --active;
            }
        }
        if (stop_requested || !(sockets[POLL_LISTENER].revents & POLLIN)) {
            continue;
        }
        const int connfd = accept(listenfd, NULL, NULL);
        if (connfd == -1) {
            if (should_retry_io() || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            break;
        }
        if (!set_nonblocking(connfd, true)) {
            perror("fcntl connfd");
            close(connfd);
            continue;
        }
        for (size_t i = POLL_FIRST_CLIENT; i < socket_count; ++i) {
            if (sockets[i].fd == -1) {
                sockets[i] = (struct pollfd){ .fd = connfd, .events = POLLIN };
                clients[i - POLL_FIRST_CLIENT] = (struct poll_client){ .response.filefd = -1 };
                ++active;
                printf("poll: watching connfd=%d\n", connfd);
                fflush(stdout);
                break;
            }
        }
    }

    for (size_t i = POLL_FIRST_CLIENT; i < socket_count; ++i) {
        if (sockets[i].fd != -1) {
            close_poll_client(&sockets[i], &clients[i - POLL_FIRST_CLIENT]);
        }
    }
    free(clients);
    return stop_requested ? EXIT_SUCCESS : EXIT_FAILURE;
}

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    int connections[CONNECTION_QUEUE_CAPACITY];
    size_t first;
    size_t count;
    bool stopping;
} connection_queue = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
};

struct worker {
    pthread_t thread;
    int number;
    int connfd;
};

static struct worker workers[THREAD_WORKER_COUNT];
static size_t workers_started;

static bool queue_has_room(void)
{
    pthread_mutex_lock(&connection_queue.mutex);
    const bool has_room = connection_queue.count < CONNECTION_QUEUE_CAPACITY;
    pthread_mutex_unlock(&connection_queue.mutex);
    return has_room;
}

static void queue_connection(int connfd)
{
    pthread_mutex_lock(&connection_queue.mutex);
    const size_t end = (connection_queue.first + connection_queue.count) % CONNECTION_QUEUE_CAPACITY;
    connection_queue.connections[end] = connfd;
    connection_queue.count++;
    pthread_cond_signal(&connection_queue.not_empty);
    pthread_mutex_unlock(&connection_queue.mutex);
}

static void *thread_worker(void *argument)
{
    struct worker *worker = argument;
    for (;;) {
        pthread_mutex_lock(&connection_queue.mutex);
        while (!connection_queue.stopping && connection_queue.count == 0) {
            pthread_cond_wait(&connection_queue.not_empty, &connection_queue.mutex);
        }
        if (connection_queue.stopping) {
            pthread_mutex_unlock(&connection_queue.mutex);
            return NULL;
        }
        const bool was_full = connection_queue.count == CONNECTION_QUEUE_CAPACITY;
        const int connfd = connection_queue.connections[connection_queue.first];
        connection_queue.first = (connection_queue.first + 1) % CONNECTION_QUEUE_CAPACITY;
        connection_queue.count--;
        worker->connfd = connfd;
        pthread_mutex_unlock(&connection_queue.mutex);
        if (was_full) {
            wake_main();
        }

        /* Waiting for this client must not lock the shared queue. */
        printf("Worker %d handles connfd=%d\n", worker->number, connfd);
        fflush(stdout);
        handle_request(connfd);
        pthread_mutex_lock(&connection_queue.mutex);
        close(connfd);
        worker->connfd = -1;
        pthread_mutex_unlock(&connection_queue.mutex);
    }
}

static bool start_thread_pool(void)
{
    sigset_t stop_signals, previous_mask;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &stop_signals, &previous_mask);
    if (error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
        return false;
    }
    /* Workers inherit this mask so only the main thread handles shutdown signals. */
    while (workers_started < THREAD_WORKER_COUNT) {
        struct worker *worker = &workers[workers_started];
        worker->number = (int)workers_started + 1;
        worker->connfd = -1;
        error = pthread_create(&worker->thread, NULL, thread_worker, worker);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            break;
        }
        ++workers_started;
    }
    error = pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
        return false;
    }
    return workers_started == THREAD_WORKER_COUNT;
}

static void stop_thread_pool(void)
{
    pthread_mutex_lock(&connection_queue.mutex);
    connection_queue.stopping = true;
    for (size_t i = 0; i < connection_queue.count; ++i) {
        close(connection_queue.connections[(connection_queue.first + i) % CONNECTION_QUEUE_CAPACITY]);
    }
    connection_queue.count = 0;
    for (size_t i = 0; i < workers_started; ++i) {
        if (workers[i].connfd != -1) {
            shutdown(workers[i].connfd, SHUT_RDWR);
        }
    }
    pthread_cond_broadcast(&connection_queue.not_empty);
    pthread_mutex_unlock(&connection_queue.mutex);
    for (size_t i = 0; i < workers_started; ++i) {
        pthread_join(workers[i].thread, NULL);
    }
}

struct child_process {
    pid_t pid;
    struct child_process *next;
};

static void start_connection_child(int listenfd, int connfd, struct child_process **children)
{
    struct child_process *child = malloc(sizeof(*child));
    if (child == NULL) {
        perror("malloc child");
        return;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        close(listenfd);
        close_wakeup_pipe();
        printf("Child pid=%ld handles connfd=%d\n", (long)getpid(), connfd);
        fflush(stdout);
        handle_blocking_client(connfd);
        close(connfd);
        _exit(EXIT_SUCCESS);
    }
    if (pid == -1) {
        perror("fork");
        free(child);
        return;
    }
    *child = (struct child_process){ .pid = pid, .next = *children };
    *children = child;
}

static void reap_children(struct child_process **children)
{
    pid_t finished;
    while ((finished = waitpid(-1, NULL, WNOHANG)) != 0) {
        if (finished == -1) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        struct child_process **entry = children;
        while (*entry != NULL && (*entry)->pid != finished) {
            entry = &(*entry)->next;
        }
        if (*entry != NULL) {
            struct child_process *child = *entry;
            *entry = child->next;
            free(child);
        }
    }
}

static void stop_children(struct child_process *children)
{
    for (struct child_process *child = children; child != NULL; child = child->next) {
        kill(child->pid, SIGTERM);
    }
    while (children != NULL) {
        struct child_process *child = children;
        while (waitpid(child->pid, NULL, 0) == -1 && errno == EINTR) {
        }
        children = child->next;
        free(child);
    }
}

static int run_accept_loop(int listenfd, bool use_fork, bool use_thread_pool)
{
    struct child_process *children = NULL;
    struct pollfd sockets[] = {
        [POLL_LISTENER] = { .fd = listenfd, .events = POLLIN },
        [POLL_WAKEUP] = { .fd = wakeup_read, .events = POLLIN },
    };
    while (!stop_requested) {
        sockets[POLL_LISTENER].events = !use_thread_pool || queue_has_room() ? POLLIN : 0;
        if (poll(sockets, sizeof(sockets) / sizeof(sockets[0]), -1) == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (sockets[POLL_WAKEUP].revents & POLLIN) {
            drain_wakeup_pipe();
            if (use_fork) {
                reap_children(&children);
            }
        }
        if (stop_requested) {
            break;
        }
        if (sockets[POLL_LISTENER].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "poll: listening socket failed\n");
            break;
        }
        if (!(sockets[POLL_LISTENER].revents & POLLIN)) {
            continue;
        }

        const int connfd = accept(listenfd, NULL, NULL);
        if (connfd == -1) {
            if (should_retry_io() || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            break;
        }
        if (stop_requested || !set_nonblocking(connfd, false)) {
            if (!stop_requested) {
                perror("fcntl connfd");
            }
            close(connfd);
            continue;
        }
        printf("Accepted connection: connfd=%d\n", connfd);
        fflush(stdout);
        if (use_thread_pool) {
            queue_connection(connfd);
            continue;
        }
        if (use_fork) {
            start_connection_child(listenfd, connfd, &children);
        } else {
            handle_blocking_client(connfd);
        }
        close(connfd);
    }
    close(listenfd);
    stop_children(children);
    if (use_thread_pool) {
        stop_thread_pool();
    }
    return stop_requested ? EXIT_SUCCESS : EXIT_FAILURE;
}

static unsigned int parse_port(const char *value)
{
    if (*value == '\0' || strspn(value, "0123456789") != strlen(value)) {
        return 0;
    }
    const unsigned long port = strtoul(value, NULL, 10);
    return port > 0 && port <= UINT16_MAX ? (unsigned int)port : 0;
}

static bool parse_reverse_ports(const char *value, struct reverse_route *route)
{
    for (;;) {
        if (route->port_count == ROUTE_BACKEND_CAPACITY || *value < '0' || *value > '9') {
            return false;
        }
        char *end;
        const unsigned long port = strtoul(value, &end, 10);
        if (port == 0 || port > UINT16_MAX || (*end != ',' && *end != '\0')) {
            return false;
        }
        for (size_t i = 0; i < route->port_count; ++i) {
            if (route->ports[i] == port) {
                return false;
            }
        }
        route->ports[route->port_count++] = (unsigned int)port;
        if (*end == '\0') {
            return true;
        }
        value = end + 1;
    }
}

static bool add_reverse_route(const char *value)
{
    const char *equals = strrchr(value, '=');
    if (config.route_count == PROXY_ROUTE_CAPACITY || value[0] != '/' || equals == NULL ||
        equals == value || equals[-1] != '/' || strcspn(value, "=?# \t\r\n") != (size_t)(equals - value)) {
        return false;
    }
    const size_t length = (size_t)(equals - value);
    for (size_t i = 0; i < config.route_count; ++i) {
        if (config.routes[i].length == length && strncmp(config.routes[i].prefix, value, length) == 0) {
            return false;
        }
    }
    struct reverse_route route = { .prefix = value, .length = length };
    if (!parse_reverse_ports(equals + 1, &route)) {
        return false;
    }
    config.routes[config.route_count++] = route;
    return true;
}

static bool parse_options(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const char *option = argv[i];
        if (strcmp(option, "--port") == 0 && i + 1 < argc) {
            config.port = parse_port(argv[++i]);
            if (config.port == 0) {
                return false;
            }
        } else if (strcmp(option, "--root") == 0 && i + 1 < argc) {
            config.root = argv[++i];
            if (*config.root == '\0') {
                return false;
            }
        } else if (strcmp(option, "--reverse") == 0 && i + 1 < argc) {
            if (!add_reverse_route(argv[++i])) {
                return false;
            }
        } else if (strcmp(option, "--forward") == 0) {
            config.forward = true;
        } else if (strcmp(option, "--cache") == 0) {
            config.cache = true;
        } else if (config.mode != SERVER_SEQUENTIAL) {
            return false;
        } else if (strcmp(option, "--fork") == 0) {
            config.mode = SERVER_FORK;
        } else if (strcmp(option, "--pthread") == 0) {
            config.mode = SERVER_THREADS;
        } else if (strcmp(option, "--poll") == 0) {
            config.mode = SERVER_POLL;
        } else {
            return false;
        }
    }
    if (config.cache && (config.mode == SERVER_FORK || (!config.forward && config.route_count == 0))) {
        fprintf(stderr, "--cache requires a forward or reverse proxy in one server process.\n");
        return false;
    }
    for (size_t i = 0; i < config.route_count; ++i) {
        const struct reverse_route *route = &config.routes[i];
        if (config.mode == SERVER_FORK && route->port_count > 1) {
            fprintf(stderr, "Round robin requires a single server process. Use --pthread, --poll, or no mode flag.\n");
            return false;
        }
        for (size_t j = 0; j < route->port_count; ++j) {
            if (route->ports[j] == config.port) {
                fprintf(stderr, "A reverse route must point to a port other than the listening port.\n");
                return false;
            }
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    if (!parse_options(argc, argv)) {
        fprintf(stderr, "Usage: %s [--fork|--pthread|--poll] [--port PORT] [--root DIR] [--forward] [--reverse /prefix/=PORT[,PORT...]]... [--cache]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const bool use_fork = config.mode == SERVER_FORK;
    const bool use_thread_pool = config.mode == SERVER_THREADS;
    const bool use_poll = config.mode == SERVER_POLL;

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    const int reuse_address = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) == -1) {
        perror("setsockopt");
        close(listenfd);
        return EXIT_FAILURE;
    }

    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)config.port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(listenfd, (const struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        close(listenfd);
        return EXIT_FAILURE;
    }

    if (listen(listenfd, 8) == -1) {
        perror("listen");
        close(listenfd);
        return EXIT_FAILURE;
    }

    if (!set_nonblocking(listenfd, true)) {
        perror("fcntl listenfd");
        close(listenfd);
        return EXIT_FAILURE;
    }
    if (!setup_server_signals(use_fork)) {
        close(listenfd);
        return EXIT_FAILURE;
    }
    if (use_thread_pool && !start_thread_pool()) {
        close(listenfd);
        stop_thread_pool();
        close_wakeup_pipe();
        return EXIT_FAILURE;
    }

    printf("Listening on 127.0.0.1:%u (listenfd=%d, pid=%ld)\n", config.port, listenfd, (long)getpid());
    fflush(stdout);
    int status;
    if (use_poll) {
        status = run_poll_server(listenfd);
        close(listenfd);
    } else {
        status = run_accept_loop(listenfd, use_fork, use_thread_pool);
    }
    close_wakeup_pipe();
    if (config.cache) {
        clear_proxy_cache();
    }
    if (status == EXIT_SUCCESS) {
        puts("Server stopped.");
    }
    return status;
}
