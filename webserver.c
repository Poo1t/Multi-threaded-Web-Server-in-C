#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>  // Added for inet_ntop declaration
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <semaphore.h>
#include <sys/select.h> // For fd_set and select()
#include <sys/time.h>   // For struct timeval

// Configuration
#define PORT 8080
#define BUFFER_SIZE 8192
#define MAX_PENDING_CONNECTIONS 2
#define THREAD_POOL_SIZE 2
#define SERVER_ROOT "./www"
#define LOG_FILE "server.log"
#define DEFAULT_FILE "index.html"
#define CLIENT_QUEUE_SIZE 2

// Thread pool
typedef struct
{
    pthread_t thread_id;
    int is_busy;
} thread_pool_t;

// Request structure
typedef struct
{
    char method[10];
    char path[256];
    char protocol[20];
    char query_string[256];
} http_request_t;

// MIME types
typedef struct
{
    const char *extension;
    const char *mime_type;
} mime_type_t;

// Forward declarations of all functions (function prototypes)
void *thread_function(void *arg);
void handle_client(int client_socket, int thread_id);
void parse_http_request(char *buffer, http_request_t *request);
void log_request(const char *client_ip, const http_request_t *request, int status_code, int thread_id);
void send_response(int client_socket, const char *content_type, char *content, size_t content_length, int status_code);
void send_file(int client_socket, const char *path, const http_request_t *request);
void send_directory_listing(int client_socket, const char *path, const http_request_t *request);
void send_error(int client_socket, int status_code);
const char *get_mime_type(const char *filename);
void signal_handler(int sig);
void enqueue_client(int client_socket);
int dequeue_client();

// Global variables
thread_pool_t thread_pool[THREAD_POOL_SIZE];
pthread_mutex_t thread_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
int server_running = 1;
FILE *log_file = NULL;
int client_queue[CLIENT_QUEUE_SIZE];
int queue_front = 0, queue_rear = 0, queue_count = 0;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t queue_sem;

// Add these globals for queue position tracking
int queue_number = 0;
pthread_mutex_t queue_number_mutex = PTHREAD_MUTEX_INITIALIZER;

// MIME type mapping
mime_type_t mime_types[] = {
    {".html", "text/html"},
    {".htm", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".json", "application/json"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".png", "image/png"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
    {".txt", "text/plain"},
    {".pdf", "application/pdf"},
    {NULL, "application/octet-stream"} // Default
};

// Enqueue client function
void enqueue_client(int client_socket) {
    pthread_mutex_lock(&queue_mutex);
    if (queue_count < CLIENT_QUEUE_SIZE) {
        // Get queue position for this client
        pthread_mutex_lock(&queue_number_mutex);
        int position = ++queue_number;
        pthread_mutex_unlock(&queue_number_mutex);
        
        // If all threads are busy (queue_count > 0), send a waiting page
        if (queue_count > 0) {
            char waiting_page[BUFFER_SIZE];
            int page_len = snprintf(waiting_page, BUFFER_SIZE,
                "<html><head><title>Server Busy</title>"
                "<meta http-equiv=\"refresh\" content=\"5\">"
                "<style>body{font-family:Arial,sans-serif;margin:40px;text-align:center;}"
                "h1{color:#e74c3c;}p{font-size:1.2rem;}.loader{width:50px;height:50px;"
                "border:5px solid #f3f3f3;border-top:5px solid #3498db;border-radius:50%%;"
                "animation:spin 2s linear infinite;margin:20px auto;}"
                "@keyframes spin{0%%{transform:rotate(0deg);}100%%{transform:rotate(360deg);}}"
                "</style></head><body>"
                "<h1>Server is Busy</h1>"
                "<div class=\"loader\"></div>"
                "<p>Your request is in queue position: %d</p>"
                "<p>This page will refresh automatically...</p>"
                "<p><small>The server has %d worker threads and is currently processing %d requests</small></p>"
                "</body></html>",
                queue_count, THREAD_POOL_SIZE, THREAD_POOL_SIZE);
            
            char header[BUFFER_SIZE];
            int header_len = snprintf(header, BUFFER_SIZE,
                "HTTP/1.1 200 OK\r\n"
                "Server: C Multithreaded Server\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %d\r\n"
                "Connection: keep-alive\r\n"
                "Retry-After: 5\r\n\r\n", 
                page_len);
                
            // Send the waiting page immediately
            write(client_socket, header, header_len);
            write(client_socket, waiting_page, page_len);
        }
        
        // Add to the queue as before
        client_queue[queue_rear] = client_socket;
        queue_rear = (queue_rear + 1) % CLIENT_QUEUE_SIZE;
        queue_count++;
        sem_post(&queue_sem);
    } else {
        // Queue full, send a 503 Service Unavailable response
        char error_page[BUFFER_SIZE];
        int page_len = snprintf(error_page, BUFFER_SIZE,
            "<html><head><title>503 Service Unavailable</title>"
            "<style>body{font-family:Arial,sans-serif;margin:40px;text-align:center;}"
            "h1{color:#e74c3c;}</style></head><body>"
            "<h1>503 Service Unavailable</h1>"
            "<p>The server is currently overloaded. Please try again later.</p>"
            "</body></html>");
            
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Server: C Multithreaded Server\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "Retry-After: 30\r\n\r\n", 
            page_len);
            
        write(client_socket, header, header_len);
        write(client_socket, error_page, page_len);
        close(client_socket);
    }
    pthread_mutex_unlock(&queue_mutex);
}

int dequeue_client() {
    while (1) {
        if (sem_wait(&queue_sem) == -1) {
            if (errno == EINTR) continue; // Retry if interrupted
        }
        pthread_mutex_lock(&queue_mutex);
        int client_socket = -1;
        if (queue_count > 0) {
            client_socket = client_queue[queue_front];
            queue_front = (queue_front + 1) % CLIENT_QUEUE_SIZE;
            queue_count--;
            pthread_mutex_unlock(&queue_mutex);
            return client_socket;
        }
        pthread_mutex_unlock(&queue_mutex);
        if (!server_running) {
            return -1;
        }
    }
}

// Handle client request - Updated function signature
void handle_client(int client_socket, int thread_id)
{
    char buffer[BUFFER_SIZE] = {0};
    char client_ip[INET_ADDRSTRLEN] = "unknown";  // Default value if IP extraction fails
    http_request_t request;
    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(struct sockaddr_in);

    // Get client IP with better error handling
    if (getpeername(client_socket, (struct sockaddr *)&addr, &addr_size) == 0) {
        const char *result = inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
        if (result == NULL) {
            strcpy(client_ip, "unknown");  // Fallback if inet_ntop fails
        }
    }

    // Read client's request
    int bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read <= 0)
    {
        if (bytes_read < 0) {
            perror("Error reading from client socket");
        }
        close(client_socket);
        printf("Thread %d: client socket closed early (read error or disconnect)\n", thread_id);
        return;
    }

    // Parse HTTP request
    parse_http_request(buffer, &request);

    // Log information about which thread is handling this request
    printf("Thread %s%d handling request: %s %s\n", 
           thread_id == -1 ? "main-" : "", 
           thread_id == -1 ? 0 : thread_id,
           request.method, request.path);

    // Construct full path with safe buffer handling
    char full_path[512];
    if (snprintf(full_path, sizeof(full_path), "%s%s", SERVER_ROOT, request.path) >= (int)sizeof(full_path)) {
        // Path too long, return error
        send_error(client_socket, 414); // URI Too Long
        log_request(client_ip, &request, 414, thread_id);
        close(client_socket);
        printf("Thread %d: URI too long, closing client socket\n", thread_id);
        return;
    }

    // Check if file exists
    struct stat path_stat;
    if (stat(full_path, &path_stat) == 0)
    {
        if (S_ISREG(path_stat.st_mode))
        {
            // It's a regular file
            send_file(client_socket, full_path, &request);
            log_request(client_ip, &request, 200, thread_id);
        }
        else if (S_ISDIR(path_stat.st_mode))
        {
            // It's a directory
            // Check for index.html with safe buffer handling
            char index_path[512];
            if (snprintf(index_path, sizeof(index_path), "%s/%s", full_path, DEFAULT_FILE) >= (int)sizeof(index_path)) {
                // Path too long
                send_error(client_socket, 500);
                log_request(client_ip, &request, 500, thread_id);
                printf("Thread %d: index.html path too long\n", thread_id);
            }
            else if (stat(index_path, &path_stat) == 0 && S_ISREG(path_stat.st_mode))
            {
                // Serve index.html
                send_file(client_socket, index_path, &request);
                log_request(client_ip, &request, 200, thread_id);
            }
            else
            {
                // Generate directory listing
                send_directory_listing(client_socket, full_path, &request);
                log_request(client_ip, &request, 200, thread_id);
            }
        }
        else
        {
            // Not a regular file or directory
            send_error(client_socket, 403);
            log_request(client_ip, &request, 403, thread_id);
            printf("Thread %d: Not a regular file or directory\n", thread_id);
        }
    }
    else
    {
        // File not found
        send_error(client_socket, 404);
        log_request(client_ip, &request, 404, thread_id);
        printf("Thread %d: File not found: %s\n", thread_id, full_path);
    }

    close(client_socket);
    printf("Thread %d: client socket closed after handling\n", thread_id);
}

// Parse HTTP request
void parse_http_request(char *buffer, http_request_t *request)
{
    // Initialize request
    memset(request, 0, sizeof(http_request_t));

    // Extract method, path, and protocol
    sscanf(buffer, "%9s %255s %19s", request->method, request->path, request->protocol);

    // Handle query string
    char *query_start = strchr(request->path, '?');
    if (query_start)
    {
        *query_start = '\0'; // Terminate path at the question mark
        strncpy(request->query_string, query_start + 1, sizeof(request->query_string) - 1);
    }

    // Handle URL decoding for path
    // This is a simple implementation - full URL decoding would be more complex
    char *src = request->path;
    char *dst = request->path;

    while (*src)
    {
        if (*src == '%' && src[1] && src[2])
        {
            // Convert hex to char
            char hex[3] = {src[1], src[2], 0};
            *dst = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else if (*src == '+')
        {
            *dst = ' ';
            src++;
        }
        else
        {
            *dst = *src;
            src++;
        }
        dst++;
    }
    *dst = '\0';
    // Default to / if path is empty
    if (request->path[0] == '\0')
    {
        strcpy(request->path, "/");
    }
}

// Log request to file - Updated to include thread_id
void log_request(const char *client_ip, const http_request_t *request, int status_code, int thread_id)
{
    pthread_mutex_lock(&log_mutex);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[30];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Add thread info to the log
    const char *thread_name = (thread_id == -1) ? "main" : "worker";
    int thread_num = (thread_id == -1) ? 0 : thread_id;
    
    fprintf(log_file, "[%s] %s - %s %s %s - %d (Thread: %s-%d)\n",
            time_str, client_ip, request->method, request->path,
            request->protocol, status_code, thread_name, thread_num);
    fflush(log_file);

    pthread_mutex_unlock(&log_mutex);
}

// Send HTTP response
void send_response(int client_socket, const char *content_type, char *content, size_t content_length, int status_code)
{
    char header[BUFFER_SIZE];
    char status_text[32];

    // Determine status text
    switch (status_code)
    {
    case 200:
        strcpy(status_text, "OK");
        break;
    case 404:
        strcpy(status_text, "Not Found");
        break;
    case 403:
        strcpy(status_text, "Forbidden");
        break;
    case 500:
        strcpy(status_text, "Internal Server Error");
        break;
    default:
        strcpy(status_text, "Unknown");
        break;
    }

    // Format header
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%a, %d %b %Y %H:%M:%S GMT", tm_info);

    int header_len = snprintf(header, BUFFER_SIZE,
                              "HTTP/1.1 %d %s\r\n"
                              "Server: C Multithreaded Server\r\n"
                              "Date: %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "X-Content-Type-Options: nosniff\r\n"
                              "X-Frame-Options: SAMEORIGIN\r\n"
                              "X-XSS-Protection: 1; mode=block\r\n"
                              "\r\n",
                              status_code, status_text, time_str, content_type, content_length);

    // Send header
    ssize_t header_written = write(client_socket, header, header_len);
    if (header_written < 0) {
        perror("Error writing HTTP header to client");
        return;
    }

    // Send content
    if (content && content_length > 0)
    {
        ssize_t content_written = write(client_socket, content, content_length);
        if (content_written < 0) {
            perror("Error writing HTTP content to client");
        }
    }
}

// Send file content
void send_file(int client_socket, const char *path, const http_request_t *request)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        send_error(client_socket, 404);
        return;
    }

    // Get file size
    struct stat stat_buf;
    fstat(fd, &stat_buf);
    off_t file_size = stat_buf.st_size;

    // Get MIME type
    const char *mime_type = get_mime_type(path);

    // Send headers
    char header[BUFFER_SIZE];
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%a, %d %b %Y %H:%M:%S GMT", tm_info);

    int header_len = snprintf(header, BUFFER_SIZE,
                              "HTTP/1.1 200 OK\r\n"
                              "Server: C Multithreaded Server\r\n"
                              "Date: %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %ld\r\n"
                              "Connection: close\r\n"
                              "X-Content-Type-Options: nosniff\r\n"
                              "X-Frame-Options: SAMEORIGIN\r\n"
                              "X-XSS-Protection: 1; mode=block\r\n"
                              "\r\n",
                              time_str, mime_type, file_size);

    write(client_socket, header, header_len);

    // Send file content using sendfile() for efficiency if available
    // As a simpler alternative, we'll use read() and write()
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    while ((bytes_read = read(fd, buffer, BUFFER_SIZE)) > 0)
    {
        write(client_socket, buffer, bytes_read);
    }

    close(fd);
}

// Send directory listing
void send_directory_listing(int client_socket, const char *path, const http_request_t *request)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir(path);
    if (!dir)
    {
        send_error(client_socket, 500);
        return;
    }

    // Buffer for HTML content
    char *content = malloc(BUFFER_SIZE * 8);
    if (!content)
    {
        closedir(dir);
        send_error(client_socket, 500);
        return;
    }

    // Create directory listing HTML
    int content_len = 0;
    content_len += sprintf(content + content_len,
                           "<html><head><title>Directory listing for %s</title>"
                           "<style>body{font-family:Arial,sans-serif;margin:20px;}"
                           "h1{color:#333;}table{border-collapse:collapse;width:100%%}"
                           "th,td{padding:8px;text-align:left;border-bottom:1px solid #ddd;}"
                           "tr:hover{background-color:#f5f5f5}</style></head>"
                           "<body><h1>Directory listing for %s</h1><table>"
                           "<tr><th>Name</th><th>Size</th><th>Last Modified</th></tr>"
                           "<tr><td><a href=\"..\">..</a></td><td>-</td><td>-</td></tr>",
                           request->path, request->path);

    struct stat stat_buf;
    char full_path[512];
    char time_str[64];
    struct tm *tm_info;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue; // Skip . and ..
        }

        // Get file info
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (stat(full_path, &stat_buf) != 0)
        {
            continue; // Skip if stat fails
        }

        // Format time
        tm_info = localtime(&stat_buf.st_mtime);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

        // Add entry to table
        if (S_ISDIR(stat_buf.st_mode))
        {
            content_len += sprintf(content + content_len,
                                   "<tr><td><a href=\"%s/\">%s/</a></td><td>-</td><td>%s</td></tr>",
                                   entry->d_name, entry->d_name, time_str);
        }
        else
        {
            content_len += sprintf(content + content_len,
                                   "<tr><td><a href=\"%s\">%s</a></td><td>%ld KB</td><td>%s</td></tr>",
                                   entry->d_name, entry->d_name, stat_buf.st_size / 1024, time_str);
        }

        // Check if we need to expand the buffer
        if (content_len > BUFFER_SIZE * 7)
        {
            char *new_content = realloc(content, BUFFER_SIZE * 16);
            if (!new_content)
            {
                free(content);
                closedir(dir);
                send_error(client_socket, 500);
                return;
            }
            content = new_content;
        }
    }

    content_len += sprintf(content + content_len,
                           "</table></body></html>");

    // Send the directory listing
    send_response(client_socket, "text/html", content, content_len, 200);

    free(content);
    closedir(dir);
}

// Send error response
void send_error(int client_socket, int status_code)
{
    char *title, *message;

    switch (status_code)
    {
    case 404:
        title = "404 Not Found";
        message = "The requested resource could not be found on this server.";
        break;
    case 403:
        title = "403 Forbidden";
        message = "You don't have permission to access this resource.";
        break;
    case 414:
        title = "414 URI Too Long";
        message = "The requested URI is too long for the server to process.";
        break;
    case 500:
    default:
        title = "500 Internal Server Error";
        message = "The server encountered an internal error.";
        status_code = 500;
        break;
    }

    char content[BUFFER_SIZE];
    int content_len = snprintf(content, BUFFER_SIZE,
                               "<html><head><title>%s</title><style>"
                               "body{font-family:Arial,sans-serif;margin:40px;line-height:1.6;}"
                               "h1{color:#D32F2F;}</style></head>"
                               "<body><h1>%s</h1><p>%s</p>"
                               "<hr><p><i>C Multithreaded Web Server</i></p></body></html>",
                               title, title, message);

    send_response(client_socket, "text/html", content, content_len, status_code);
}

// Get MIME type based on file extension
const char *get_mime_type(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext)
        return mime_types[sizeof(mime_types) / sizeof(mime_types[0]) - 1].mime_type;

    for (int i = 0; mime_types[i].extension != NULL; i++)
    {
        if (strcmp(ext, mime_types[i].extension) == 0)
        {
            return mime_types[i].mime_type;
        }
    }

    return mime_types[sizeof(mime_types) / sizeof(mime_types[0]) - 1].mime_type;
}

// Signal handler for immediate shutdown
void signal_handler(int sig)
{
    if (sig == SIGINT)
    {
        printf("\nShutting down server gracefully...\n");
        server_running = 0;
        
        // Wake up all worker threads blocked on sem_wait
        for (int i = 0; i < THREAD_POOL_SIZE; i++) {
            sem_post(&queue_sem);
        }
        
        // No need to close server_fd here, we'll do it in the main cleanup
    }
}

int main()
{
    int server_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    // Set up signal handling
    signal(SIGINT, signal_handler);
    // Ignore SIGPIPE to prevent server crash on client disconnect
    signal(SIGPIPE, SIG_IGN);

    // Open log file
    log_file = fopen(LOG_FILE, "a");
    if (log_file == NULL)
    {
        perror("Could not open log file");
        log_file = stdout;
    }

    // Create server directory if it doesn't exist
    struct stat st = {0};
    if (stat(SERVER_ROOT, &st) == -1)
    {
        mkdir(SERVER_ROOT, 0755);
        printf("Created server root directory: %s\n", SERVER_ROOT);
    }

    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set socket options to reuse address
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("Setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // Set up address structure
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Bind socket to the address
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_fd, MAX_PENDING_CONNECTIONS) < 0)
    {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server started at http://localhost:%d\n", PORT);
    printf("Server root: %s\n", SERVER_ROOT);
    printf("Press Ctrl+C to stop the server\n");

    // Initialize semaphore with error check
    if (sem_init(&queue_sem, 0, 0) != 0) {
        perror("sem_init failed");
        exit(EXIT_FAILURE);
    }

    // Initialize thread pool with error check
    for (int i = 0; i < THREAD_POOL_SIZE; i++)
    {
        int ret = pthread_create(&thread_pool[i].thread_id, NULL, thread_function, (void *)(intptr_t)i);
        if (ret != 0) {
            fprintf(stderr, "pthread_create failed for thread %d: %s\n", i, strerror(ret));
            exit(EXIT_FAILURE);
        }
    }

    // Set socket to non-blocking mode for accept timeout
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
    
    // Main server loop
    while (server_running)
    {
        struct sockaddr_in client_address;
        socklen_t client_addrlen = sizeof(client_address);
        
        // Use select to wait for data with timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;  // 1 second timeout
        timeout.tv_usec = 0;
        
        int activity = select(server_fd + 1, &readfds, NULL, NULL, &timeout);
        
        if (activity < 0) {
            if (errno == EINTR) {
                // Interrupted by signal, check if we should continue
                if (!server_running) {
                    printf("Server shutting down, breaking accept loop\n");
                    break;
                }
                continue;
            }
            perror("Select error");
            continue;
        }
        
        // Timeout occurred, check if we should still be running
        if (activity == 0) {
            if (!server_running) {
                printf("Server shutting down after timeout\n");
                break;
            }
            continue;
        }
        
        // Accept connection if available
        if (FD_ISSET(server_fd, &readfds)) {
            int client_socket = accept(server_fd, (struct sockaddr *)&client_address, &client_addrlen);
            if (client_socket < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    // No pending connections
                    continue;
                }
                perror("Accept failed");
                continue;
            }
            enqueue_client(client_socket);
        }
    }

    // Wait for worker threads to finish
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_join(thread_pool[i].thread_id, NULL);
    }

    // Cleanup
    close(server_fd);
    sem_destroy(&queue_sem);
    pthread_mutex_destroy(&queue_mutex);
    pthread_mutex_destroy(&log_mutex);
    pthread_mutex_destroy(&thread_pool_mutex);
    pthread_mutex_destroy(&queue_number_mutex);
    if (log_file != NULL && log_file != stdout)
    {
        fclose(log_file);
    }

    printf("Server shutdown complete.\n");
    return 0;
}

// Thread pool worker function
void *thread_function(void *arg)
{
    int thread_id = (intptr_t)arg;
    printf("Thread %d started\n", thread_id);
    while (1) {
        int client_socket = dequeue_client();
        if (client_socket != -1) {
            handle_client(client_socket, thread_id);
        } else if (!server_running) {
            // If server is shutting down and queue is empty, exit thread
            printf("Thread %d exiting (server_running=0, queue empty)\n", thread_id);
            break;
        }
    }
    printf("Thread %d exited cleanly\n", thread_id);
    return NULL;
}