#define _GNU_SOURCE

#include "bash_funcs.h"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job_list.h"
#include "string_vector.h"

#define MAX_ARGS 10

int tokenize(char *s, strvec_t *tokens) {

    // Check if inputs are null
    if (s == NULL || tokens == NULL) {
        return -1;
    }

    // First call the strtok with delim of " "
    char *token = strtok(s, " ");
    while (token != NULL) {
        // Add token to the token vec
        if (strvec_add(tokens, token) == -1) {
            return -1;
        }
        // Get next token
        token = strtok(NULL, " ");
    }
    return 0;
}

int run_command(strvec_t *tokens) {

    // Init sig
    struct sigaction sac;
    sac.sa_handler = SIG_DFL;
    if (sigfillset(&sac.sa_mask) == -1) {
        perror("sigfillset");
        return -1;
    }
    sac.sa_flags = 0;
    if (sigaction(SIGTTIN, &sac, NULL) == -1 || sigaction(SIGTTOU, &sac, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    pid_t pid = getpid();
    if (setpgid(pid, pid) == -1) {
        perror("setpgid");
        return -1;
    }

    char *args[tokens->length + 1];
    int arg_count = 0;

    // Process tokens and prepare args for exec
    for (int i = 0; i < tokens->length; i++) {
        if ((args[arg_count] = strvec_get(tokens, i)) == NULL) {
            printf("strvec_get");
            return -1;
        } else if (strcmp(args[arg_count], ">") == 0 || strcmp(args[arg_count], ">>") == 0 ||
                   strcmp(args[arg_count], "<") == 0) {
            // Terminate args array before redirection operators
            args[arg_count] = NULL;
            break;
        }
        arg_count++;
    }


    // Handle output redirection
    int redir_index;
    if ((redir_index = strvec_find(tokens, ">")) != -1) {
        char *file_name = strvec_get(tokens, redir_index + 1);
        int fd;

        // Open file and duplicate it to the STDOUT
        if ((fd = open(file_name, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) == -1) {
            perror("Failed to open output file");
            return -1;
        }

        if (dup2(fd, STDOUT_FILENO) == -1) {
            perror("dup2");
            close(fd);
            return -1;
        }

        // Close out file
        if (close(fd) == -1) {
            perror("close");
            return -1;
        }

    // Handle append redirection
    } else if ((redir_index = strvec_find(tokens, ">>")) != -1) {
        char *file_name = strvec_get(tokens, redir_index + 1);
        int fd;

        // Open file in append mode and duplicate it to the STDOUT
        if ((fd = open(file_name, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR)) == -1) {
            perror("Failed to open output file");
            return -1;
        }

        if (dup2(fd, STDOUT_FILENO) == -1) {
            perror("dup2");
            close(fd);
            return -1;
        }

        // Close out file
        if (close(fd) == -1) {
            perror("close");
            return -1;
        }
    }
    // Handle input redirection
    if ((redir_index = strvec_find(tokens, "<")) != -1) {
        char *file_name = strvec_get(tokens, redir_index + 1);
        int fd;

        // Open for reading
        if ((fd = open(file_name, O_RDONLY)) == -1) {
            perror("Failed to open input file");
            return -1;
        }

        // Redirect stdin to the file
        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2");
            close(fd);
            return -1;
        }

        // Close out file
        if (close(fd) == -1) {
            perror("close");
            return -1;
        }
    }

    // Exec
    args[arg_count] = NULL;
    if (execvp(args[0], args) == -1) {
        perror("exec");
        return -1;
    }

    return 0;
}

int resume_job(strvec_t *tokens, job_list_t *jobs, int is_foreground) {
    // Parse the job index from tokens[1]
    // Get the job token from the index
    char *job_token_char = strvec_get(tokens, 1);
    if (job_token_char == NULL) {
        fprintf(stderr, "Failed to get job token\n");
        return -1;
    }

    // Convert the job index to an integer
    int job_token = atoi(job_token_char);
    if (job_token == 0 && strcmp(job_token_char, "0") != 0) {
        fprintf(stderr, "Invalid job index\n");
        return -1;
    }

    // Retrieve the job from the jobs list
    job_t *job = job_list_get(jobs, job_token);
    if (job == NULL) {
        fprintf(stderr, "Job index out of bounds\n");
        return -1;
    }

    // Move the job's process group to the foreground
    if (is_foreground) {
        if (tcsetpgrp(STDIN_FILENO, job->pid) == -1) {
            perror("tcsetpgrp");
            return -1;
        }

        // Send the SIGCONT signal to the process
        if (kill(job->pid, SIGCONT) == -1) {
            perror("Failed to send SIGCONT");
            return -1;
        }

        // Wait for the process to continue (or terminate)
        int status;
        waitpid(job->pid, &status, WUNTRACED);

        // Check if the job terminated (either normally or via a signal)
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (job_list_remove(jobs, job_token) == -1) {
                fprintf(stderr, "Failed to remove job from list\n");
            }
        }

        // Restore the shell to the foreground
        pid_t shell_pid = getpid();
        if (tcsetpgrp(STDIN_FILENO, shell_pid) == -1) {
            perror("tcsetpgrp");
            return -1;
        }
    } else { // background move
        job->status = BACKGROUND;
        // Send the SIGCONT signal to the process
        if (kill(job->pid, SIGCONT) == -1) {
            perror("Failed to send SIGCONT");
            return -1;
        }
    }

    return 0;
}

int await_background_job(strvec_t *tokens, job_list_t *jobs) {
    // Get the job token from the index
    char *job_token_char = strvec_get(tokens, 1);
    if (job_token_char == NULL) {
        fprintf(stderr, "Failed to get job token\n");
        return -1;
    }

    // Convert the job index to an integer
    int job_token = atoi(job_token_char);
    if (job_token == 0 && strcmp(job_token_char, "0") != 0) {
        fprintf(stderr, "Invalid job index\n");
        return -1;
    }

    // Retrieve the job from the jobs list
    job_t *job = job_list_get(jobs, job_token);
    if (job == NULL) {
        fprintf(stderr, "Job index out of bounds\n");
        return -1;
    }

    if (job->status != BACKGROUND) {
        fprintf(stderr, "Job index is for stopped process not background process\n");
        return -1;
    }

    // Wait for the job to terminate
    int status;
    waitpid(job->pid, &status, WUNTRACED);

    // Remove job from the list of jobs
    if (WIFEXITED(status)) {
        if (job_list_remove(jobs, job_token) == -1) {
            fprintf(stderr, "Failed to remove job from list\n");
        }
    }

    return 0;
}

int await_all_background_jobs(job_list_t *jobs) {

    // Init job
    job_t *job = jobs->head;
    int status;

    // Wait for each job
    for (int i = 0; i < jobs->length; i++) {
        if (job->status == BACKGROUND) {
            // Does not let me error check this
            waitpid(job->pid, &status, WUNTRACED);
            if (WIFSTOPPED(status)) {
                job->status = STOPPED;
            }
        }
    }

    // Remove all background jobs from list of jobs
    job_list_remove_by_status(jobs, BACKGROUND);

    return 0;
}
