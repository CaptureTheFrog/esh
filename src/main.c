//
// Created by eddie on 05/05/24.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <malloc.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <libgen.h>
#include <sys/types.h>
#include <pwd.h>
#include <limits.h>

#define READLINE_CHUNK_SIZE 64

char* readline(const char* prompt);
int parse_command(char* command);
void tokenize(char* command, char*** tokens);

bool interactive = false;
char* path;
char* arg0;
char* shell_basename;
extern char **environ;
int exit_code = 0;
uint32_t path_components = 0;

int main(int argc, char** argv){
    path = getenv("PATH");
    if(path == NULL || path[0] == '\0'){
        // path not set, defaulting
        path = "/bin:/usr/bin";
    }

    arg0 = argv[0];
    arg0 = malloc(strlen(argv[0])+1);
    memcpy(arg0, argv[0], strlen(argv[0])+1);
    shell_basename = basename(arg0);

    struct passwd *p = getpwuid(getuid());  // Check for NULL!
    char hostname[HOST_NAME_MAX+1];
    gethostname(hostname, sizeof(hostname));  // Check the return value!

    while(true){
        char* cwd  = getcwd(NULL, 0);
        char* prompt = NULL;

        asprintf(&prompt, "%s@%s:\033[0;34m%s\033[0m$ ", p->pw_name, hostname, cwd);
        free(cwd);

        char* command = readline(prompt);
        free(prompt);

        if(command == NULL){
            // EOF
            fputs("\n", stderr);
            break;
        }

        int exit = parse_command(command);
        free(command);
        if(exit != 0){
            break;
        }
    }

    fputs("exit\n", stderr);

    free(arg0);

    return exit_code;
}

char* readline(const char* prompt) {
    if(interactive){
        int read;

        fputs(prompt, stderr);
        while((read = getchar()) != EOF){
            putchar(read);
        }
    }else{
        char* buf = malloc(READLINE_CHUNK_SIZE);

        fputs(prompt, stderr);
        if (fgets(buf, READLINE_CHUNK_SIZE, stdin) == NULL) {
            free(buf);
            return NULL;
        }

        // trim whitespace
        size_t leading_spaces = 0;
        while(isspace((unsigned char)*(buf + leading_spaces))) leading_spaces++;

        // the string was all spaces
        if(*(buf + leading_spaces) == '\0'){
            buf = realloc(buf, 1);
            buf[0] = '\0';
            return buf;
        }

        // get rid of leading spaces
        size_t sz = strlen(buf + leading_spaces) + 1;
        memcpy(buf, (buf + leading_spaces), sz);
        buf = realloc(buf, sz);

        // get rid of trailing spaces
        while(sz > 1 && isspace((unsigned char)*(buf + sz - 2))) sz--;
        buf[sz - 1] = '\0';
        buf = realloc(buf, sz);

        return buf;
    }
}

int parse_command(char* command){
    if(command == NULL || command[0] == '\0') return 0;

    int exit = 0;

    // tokenize
    char** tokens = NULL;
    tokenize(command, &tokens);
    if(tokens == NULL || tokens[0] == NULL){
        free(tokens);
        return 0;
    }

    if(strcmp(tokens[0], "cd") == 0) {
        if(tokens[2] != NULL){
            fprintf(stderr, "%s: cd: too many arguments\n", shell_basename);
            exit_code = 1;
        }else if(tokens[1] == NULL){
            // no dir given
            char* home = getenv("HOME");
            if(home == NULL || home[0] == '\0'){
                fprintf(stderr, "%s: cd: HOME not set\n", shell_basename);
                exit_code = 1;
            }else{
                if(chdir(home) == -1){
                    fprintf(stderr, "%s: cd: ", shell_basename);
                    perror(home);
                    exit_code = 1;
                }
            }
        }else{
            if(chdir(tokens[1]) == -1){
                fprintf(stderr, "%s: cd: ", shell_basename);
                perror(tokens[1]);
                exit_code = 1;
            }
        }
    }else if(strcmp(tokens[0], "exit") == 0) {
        if(tokens[1] != NULL && tokens[2] != NULL){
            fprintf(stderr, "%s: exit: too many arguments\n", shell_basename);
        }else{
            if(tokens[1] != NULL){
                exit_code = atoi(tokens[1]);
            }
            exit = 1;
        }
    }else{
        // search for executable in path
        char *path_component = path;
        int path_component_len;

        char *delim;
        char *best_guess = NULL;
        const char *reason = "command not found";

        do {
            char *file;
            delim = strchr(path_component, ':');
            if (delim == NULL) {
                path_component_len = strlen(path_component);
            } else {
                path_component_len = delim - path_component;
            }

            asprintf(&file, "%.*s/%s", path_component_len, path_component, tokens[0]);
            struct stat sb;
            if (stat(file, &sb) == 0 && S_ISREG(sb.st_mode)) {
                // found the file
                if (sb.st_mode & S_IXUSR) {
                    // executable
                    pid_t pid = fork();
                    if (pid == 0) {
                        if (execve(file, tokens, environ) == -1) {
                            perror("Error in execve\n");
                            // todo exit process
                        }
                    } else if (pid > 0) {
                        // parent
                        int status;
                        pid = wait(&status);
                        exit_code = WEXITSTATUS(status);
                    } else {
                        perror("Error forking\n");
                    }
                    free(file);
                    free(tokens);
                    return 0;
                } else {
                    free(best_guess);
                    best_guess = malloc(strlen(file) + 1);
                    memcpy(best_guess, file, strlen(file) + 1);
                    reason = "permission denied";
                }
            }

            free(file);
            path_component += path_component_len + 1;
        } while (delim != NULL);

        if (best_guess == NULL) {
            fprintf(stderr, "%s: %s\n", tokens[0], reason);
        } else {
            fprintf(stderr, "%s: %s: %s\n", shell_basename, best_guess, reason);
        }
    }
    free(tokens);
    return exit;
}

void tokenize(char* command, char*** tokens){
    size_t current_token = 0;
    char* last_token = command;
    char quote = '\0';
    *tokens = malloc(sizeof(char*));
    size_t m = strlen(command) + 1;
    for(size_t i = 0; i < m; i++){
        if((command[i] == '"' || command[i] == '\'') && (i == 0 || command[i-1] != '\\')){
            if(quote == '\0') quote = command[i];
            else if(quote == command[i]) quote = '\0';
        }

        if(quote) continue;

        if(isspace(command[i]) || command[i] == '\0'){
            command[i] = '\0';
            for(char* s = last_token; *s != '\0'; s++){
                if(!isspace(*s)){
                    *tokens = realloc(*tokens, (current_token + 1) * sizeof(char*));
                    (*tokens)[current_token] = last_token;
                    current_token++;
                    break;
                }
            }
            last_token = (command + i + 1);
        }
    }
    *tokens = realloc(*tokens, (current_token + 1) * sizeof(char*));
    (*tokens)[current_token] = NULL;
}