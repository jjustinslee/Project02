#define _GNU_SOURCE

#include "swish_funcs.h"

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
  char *token = strtok(s, " ");
  while (token) {
    strvec_add(tokens, token);
    token = strtok(NULL, " ");
  }
  return 0;
}

int run_command(strvec_t *tokens) {
  char *args[MAX_ARGS + 1];
  int i;
  for (i = 0; i < tokens->length && i < MAX_ARGS; i++) {
    args[i] = tokens->data[i];
  }
  args[i] = NULL;

  // Handle output redirection (existing code)
  for (i = 0; i < tokens->length; i++) {
    if (strcmp(tokens->data[i], ">") == 0 ||
        strcmp(tokens->data[i], ">>") == 0) {
      int flags = O_WRONLY | O_CREAT |
                  (strcmp(tokens->data[i], ">>") == 0 ? O_APPEND : O_TRUNC);
      int fd = open(tokens->data[i + 1], flags, 0644);
      if (fd < 0) {
        perror("open");
        return -1;
      }
      dup2(fd, STDOUT_FILENO);
      close(fd);
      args[i] = NULL;
      break;
    }
  }

  // Handle input redirection: If '<' is found, redirect input
  for (i = 0; i < tokens->length; i++) {
    if (strcmp(tokens->data[i], "<") == 0) {
      int fd = open(tokens->data[i + 1], O_RDONLY);
      if (fd < 0) {
        fprintf(stderr,
                "Failed to open input file: No such file or directory\n");
        return -1;
      }
      dup2(fd, STDIN_FILENO); // Redirect standard input to file
      close(fd);
      args[i] = NULL; // Nullify argument after redirection
      break;
    }
  }

  // Restore signal handlers
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTTOU, &sa, NULL);
  sigaction(SIGTTIN, &sa, NULL);

  // Change process group
  pid_t pid = getpid();
  setpgid(pid, pid);

  execvp(args[0], args);
  return -1;
}

int resume_job(strvec_t *tokens, job_list_t *jobs, int is_foreground) {
  if (tokens->length < 2) {
    fprintf(stderr, "Error: No job index provided\n");
    return -1;
  }

  int job_index = atoi(tokens->data[1]);

  // Check if job_index is valid
  if (job_index < 0 || job_index >= jobs->length) {
    fprintf(stderr, "Job index out of bounds\n"); // Adjusted error message
    return -1;
  }

  job_t *job = job_list_get(jobs, job_index);
  if (!job) {
    fprintf(stderr, "Error: No job found at index %d\n", job_index);
    return -1;
  }

  if (is_foreground) {
    if (tcsetpgrp(STDIN_FILENO, job->pid) < 0) {
      perror("tcsetpgrp");
      return -1;
    }

    if (kill(job->pid, SIGCONT) < 0) {
      perror("kill");
      return -1;
    }

    int status;
    waitpid(job->pid, &status, WUNTRACED);

    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      job_list_remove(jobs, job_index);
    }

    if (tcsetpgrp(STDIN_FILENO, getpid()) < 0) {
      perror("tcsetpgrp (reset)");
      return -1;
    }
  } else {
    if (kill(job->pid, SIGCONT) < 0) {
      perror("kill");
      return -1;
    }
    job->status = BACKGROUND;
  }

  return 0;
}

int await_background_job(strvec_t *tokens, job_list_t *jobs) {
  if (tokens->length < 2)
    return -1;
  int job_index = atoi(tokens->data[1]);
  job_t *job = job_list_get(jobs, job_index);
  if (!job || job->status != BACKGROUND)
    return -1;

  int status;
  waitpid(job->pid, &status, WUNTRACED);
  if (WIFEXITED(status) || WIFSIGNALED(status)) {
    job_list_remove(jobs, job_index);
  }
  return 0;
}

int await_all_background_jobs(job_list_t *jobs) {
  int i = 0;
  while (i < jobs->length) {
    job_t *job = job_list_get(jobs, i);
    if (job->status == BACKGROUND) {
      int status;
      waitpid(job->pid, &status, WUNTRACED);
      if (WIFSTOPPED(status)) {
        job->status = STOPPED;
      }
    }
    i++;
  }
  job_list_remove_by_status(jobs, BACKGROUND);
  return 0;
}
