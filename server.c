#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 8080
#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024
#define LOGFILE "chat_log.txt"

typedef struct {
    struct sockaddr_in address;
    int sockfd;
    int uid;
} client_t;

client_t *clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned int clients_count = 0;
static int uid = 10;

void str_trim_lf(char* arr, int length) {
    for (int i = 0; i < length; i++) {
        if (arr[i] == '\n') {
            arr[i] = '\0';
            break;
        }
    }
}

void log_message(char *message) {
    FILE *file = fopen(LOGFILE, "a");
    if (file) {
        fprintf(file, "%s\n", message);
        fclose(file);
    }
}

void send_message(char *s, int uid) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            if (clients[i]->uid != uid) {
                if (write(clients[i]->sockfd, s, strlen(s)) < 0) {
                    perror("ERROR: write to descriptor failed");
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void send_log_to_client(int sockfd) {
    FILE *file = fopen(LOGFILE, "r");
    if (file) {
        char line[BUFFER_SIZE];
        while (fgets(line, sizeof(line), file)) {
            write(sockfd, line, strlen(line));
        }
        fclose(file);
    }
}

char* get_line_from_log(int line_number) {
    FILE *file = fopen(LOGFILE, "r");
    if (!file) return NULL;

    char *line = malloc(BUFFER_SIZE);
    int current_line = 0;
    while (fgets(line, BUFFER_SIZE, file)) {
        if (++current_line == line_number) {
            fclose(file);
            str_trim_lf(line, strlen(line));
            return line;
        }
    }

    fclose(file);
    free(line);
    return NULL;
}

void queue_add(client_t *cl) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i]) {
            clients[i] = cl;
            clients_count++;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void queue_remove(int uid) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            if (clients[i]->uid == uid) {
                clients[i] = NULL;
                clients_count--;
                break;
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void *handle_client(void *arg) {
    char buff_out[BUFFER_SIZE];
    int leave_flag = 0;

    client_t *cli = (client_t *)arg;

    // Send log file to the client
    send_log_to_client(cli->sockfd);

    sprintf(buff_out, "Client %d has joined\n", cli->uid);
    printf("%s", buff_out);
    log_message(buff_out);
    send_message(buff_out, cli->uid);

    bzero(buff_out, BUFFER_SIZE);

    while (1) {
        if (leave_flag) {
            break;
        }
        int receive = recv(cli->sockfd, buff_out, BUFFER_SIZE, 0);
        if (receive > 0) {
            str_trim_lf(buff_out, strlen(buff_out));
            if (strlen(buff_out) > 0) {
                if (strncmp(buff_out, "/r ", 3) == 0) {
                    // Process reply command
                    int line_number = atoi(buff_out + 3);
                    char *message_start = strchr(buff_out + 3, ' ');
                    if (message_start) {
                        message_start++; // Move past the space
                        char *line_message = get_line_from_log(line_number);
                        if (line_message) {
                            sprintf(buff_out, "Replied to: %s -> %s", line_message, message_start);
                            free(line_message);
                        } else {
                            sprintf(buff_out, "Invalid line number: %d", line_number);
                        }
                    }
                }
                send_message(buff_out, cli->uid);
                printf("Client %d -> %s\n", cli->uid, buff_out);
                log_message(buff_out);
            }
        } else if (receive == 0 || strcmp(buff_out, "exit") == 0) {
            sprintf(buff_out, "Client %d has left\n", cli->uid);
            printf("%s", buff_out);
            log_message(buff_out);
            send_message(buff_out, cli->uid);
            leave_flag = 1;
        } else {
            printf("ERROR: -1\n");
            leave_flag = 1;
        }

        bzero(buff_out, BUFFER_SIZE);
    }

    close(cli->sockfd);
    queue_remove(cli->uid);
    free(cli);
    pthread_detach(pthread_self());

    return NULL;
}

int main() {
    int sockfd, new_sockfd;
    struct sockaddr_in server_addr, client_addr;
    pthread_t tid;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));

    if (listen(sockfd, 10) < 0) {
        perror("ERROR: listen");
        return 1;
    }

    printf("=== WELCOME TO THE CHAT SERVER ===\n");

    while (1) {
        socklen_t clilen = sizeof(client_addr);
        new_sockfd = accept(sockfd, (struct sockaddr*)&client_addr, &clilen);

        if ((new_sockfd) < 0) {
            perror("ERROR: accept");
            return 1;
        }

        if ((clients_count + 1) == MAX_CLIENTS) {
            printf("Max clients reached. Rejected: ");
            printf("%d.%d.%d.%d",
                client_addr.sin_addr.s_addr & 0xff,
                (client_addr.sin_addr.s_addr & 0xff00) >> 8,
                (client_addr.sin_addr.s_addr & 0xff0000) >> 16,
                (client_addr.sin_addr.s_addr & 0xff000000) >> 24);
            printf(":%d\n", client_addr.sin_port);
            close(new_sockfd);
            continue;
        }

        client_t *cli = (client_t *)malloc(sizeof(client_t));
        cli->address = client_addr;
        cli->sockfd = new_sockfd;
        cli->uid = uid++;

        queue_add(cli);
        pthread_create(&tid, NULL, &handle_client, (void*)cli);

        sleep(1);
    }

    return 0;
}
