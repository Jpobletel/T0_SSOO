#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/time.h>
#include "../input_manager/manager.h"

#define MAX_PROCESSES 10
#define MAX_ARGS 20
#define MAX_EXECUTABLE_LEN 256

typedef struct {
    pid_t pid;
    char executable[MAX_EXECUTABLE_LEN];
    time_t start_time;
    int exit_code;
    int signal_value;
    bool terminated;
    bool timeout_sent;
} ProcessInfo;

ProcessInfo processes[MAX_PROCESSES];
int process_count = 0;
int time_max = 0;
volatile sig_atomic_t signal_received = 0;

// Function prototypes
void execute_command(char **input);
void start_process(char *executable, char **args);
void show_info();
void handle_timeout(int timeout_secs);
void handle_quit();
void check_time_max();
void sigchld_handler(int sig);
void sigint_handler(int sig);
void print_process_stats(ProcessInfo *process);
void terminate_process(int index, int signal);

int main(int argc, char *argv[]) {
    if (argc > 1) {
        time_max = atoi(argv[1]);
        if (time_max < 0) {
            fprintf(stderr, "Error: time_max debe ser un valor positivo\n");
            exit(EXIT_FAILURE);
        }
    }

    // Configurar manejadores de señales
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    sa.sa_handler = sigint_handler;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    printf("DCCAdmin iniciado. time_max=%d\n", time_max);
    printf("Ingrese comandos (start, info, timeout, quit):\n");

    while (1) {
        if (time_max > 0) {
            check_time_max();
        }
        
        printf("> ");
        fflush(stdout);
        
        char **input = read_user_input();
        if (input[0] == NULL) {
            free_user_input(input);
            continue;
        }
        
        execute_command(input);
        free_user_input(input);
    }
    return EXIT_SUCCESS;
}

void execute_command(char **input) {
    if (strcmp(input[0], "start") == 0) {
        if (input[1] == NULL) {
            printf("Error: Falta el nombre del ejecutable\n");
            return;
        }
        start_process(input[1], &input[1]);
    }
    else if (strcmp(input[0], "info") == 0) {
        show_info();
    }
    else if (strcmp(input[0], "timeout") == 0) {
        if (input[1] == NULL) {
            printf("Error: Falta el tiempo para timeout\n");
            return;
        }
        int time = atoi(input[1]);
        if (time <= 0) {
            printf("Error: El tiempo debe ser positivo\n");
            return;
        }
        handle_timeout(time);
    }
    else if (strcmp(input[0], "quit") == 0) {
        handle_quit();
        exit(EXIT_SUCCESS);
    }
    else {
        printf("Comando no reconocido: %s\n", input[0]);
    }
}

void start_process(char *executable, char **args) {
    if (process_count >= MAX_PROCESSES) {
        printf("Error: Se ha alcanzado el máximo de procesos concurrentes\n");
        return;
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        perror("Error al crear proceso hijo");
        return;
    }
    if (pid == 0) {
        execvp(executable, args);
        perror("Error al ejecutar el programa");
        exit(EXIT_FAILURE);
    } else {
        processes[process_count].pid = pid;
        strncpy(processes[process_count].executable, executable, MAX_EXECUTABLE_LEN - 1);
        processes[process_count].executable[MAX_EXECUTABLE_LEN - 1] = '\0';
        processes[process_count].start_time = time(NULL);
        processes[process_count].terminated = false;
        processes[process_count].timeout_sent = false;
        processes[process_count].exit_code = -1;
        processes[process_count].signal_value = -1;
        process_count++;
        printf("Proceso iniciado con PID: %d\n", pid);
    }
}

void show_info() {
    printf("Procesos en ejecución:\n");
    printf("PID\tEjecutable\tTiempo\tExit\tSignal\n");
    time_t now = time(NULL);
    bool any_process = false;
    
    for (int i = 0; i < process_count; i++) {
        if (!processes[i].terminated) {
            any_process = true;
            double elapsed = difftime(now, processes[i].start_time);
            printf("%d\t%s\t%.0f\t%d\t%d\n", 
                   processes[i].pid, 
                   processes[i].executable, 
                   elapsed,
                   processes[i].exit_code,
                   processes[i].signal_value);
        }
    }
    
    if (!any_process) {
        printf("No hay procesos en ejecución\n");
    }
}

void handle_timeout(int timeout_secs) {
    bool any_process = false;
    for (int i = 0; i < process_count; i++) {
        if (!processes[i].terminated) {
            any_process = true;
            break;
        }
    }
    
    if (!any_process) {
        printf("No hay procesos en ejecución. Timeout no se puede ejecutar.\n");
        return;
    }
    
    printf("Esperando %d segundos...\n", timeout_secs);
    sleep(timeout_secs);
    
    printf("Timeout cumplido!\n");
    time_t now = time(NULL);
    
    for (int i = 0; i < process_count; i++) {
        if (!processes[i].terminated) {
            double elapsed = difftime(now, processes[i].start_time);
            printf("%d %s %.0f %d %d\n", 
                   processes[i].pid, 
                   processes[i].executable, 
                   elapsed,
                   processes[i].exit_code,
                   processes[i].signal_value);
            
            terminate_process(i, SIGTERM);
        }
    }
}

void handle_quit() {
    printf("Terminando DCCAdmin...\n");
    
    // Primero enviar SIGINT a todos los procesos
    for (int i = 0; i < process_count; i++) {
        if (!processes[i].terminated) {
            kill(processes[i].pid, SIGINT);
            processes[i].signal_value = SIGINT;
        }
    }
    
    // Esperar 10 segundos
    sleep(10);
    
    // Enviar SIGKILL a los procesos que aún no terminaron
    for (int i = 0; i < process_count; i++) {
        if (!processes[i].terminated) {
            kill(processes[i].pid, SIGKILL);
            processes[i].signal_value = SIGKILL;
        }
    }
    
    // Mostrar estadísticas finales
    printf("DCCAdmin finalizado\n");
    for (int i = 0; i < process_count; i++) {
        print_process_stats(&processes[i]);
    }
}

void check_time_max() {
    time_t now = time(NULL);
    
    for (int i = 0; i < process_count; i++) {
        if (!processes[i].terminated && !processes[i].timeout_sent) {
            double elapsed = difftime(now, processes[i].start_time);
            if (elapsed >= time_max) {
                printf("Proceso %d (%s) alcanzó time_max (%d segundos)\n", 
                       processes[i].pid, processes[i].executable, time_max);
                terminate_process(i, SIGTERM);
                processes[i].timeout_sent = true;
            }
        }
    }
}

void sigchld_handler(int sig) {
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < process_count; i++) {
            if (processes[i].pid == pid) {
                processes[i].terminated = true;
                
                if (WIFEXITED(status)) {
                    processes[i].exit_code = WEXITSTATUS(status);
                    processes[i].signal_value = -1;
                } else if (WIFSIGNALED(status)) {
                    processes[i].exit_code = -1;
                    processes[i].signal_value = WTERMSIG(status);
                }
                
                printf("Proceso terminado: PID=%d, Ejecutable=%s\n", pid, processes[i].executable);
                break;
            }
        }
    }
}

void sigint_handler(int sig) {
    printf("\nRecibida señal SIGINT (Ctrl+C)\n");
    handle_quit();
    exit(EXIT_SUCCESS);
}

void print_process_stats(ProcessInfo *process) {
    time_t now = time(NULL);
    double elapsed = difftime(now, process->start_time);
    
    printf("%d %s %.0f %d %d\n", 
           process->pid, 
           process->executable, 
           elapsed,
           process->exit_code,
           process->signal_value);
}

void terminate_process(int index, int signal) {
    if (!processes[index].terminated) {
        kill(processes[index].pid, signal);
        processes[index].signal_value = signal;
        
        if (signal == SIGTERM) {
            // Programar SIGKILL para 5 segundos después si se envió SIGTERM
            pid_t pid = fork();
            if (pid == 0) {
                sleep(5);
                if (!processes[index].terminated) {
                    kill(processes[index].pid, SIGKILL);
                }
                exit(0);
            }
        }
    }
}