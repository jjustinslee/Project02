#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job_list.h"
#include "string_vector.h"
#include "swish_funcs.h"

#define CMD_LEN 512
#define PROMPT "@> "

int main(int argc, char **argv) {
  // Task 4: Set up shell to ignore SIGTTIN, SIGTTOU when put in background
  // You should adapt this code for use in run_command().
  struct sigaction sac;
  sac.sa_handler = SIG_IGN;
  if (sigfillset(&sac.sa_mask) == -1) {
    perror("sigfillset");
    return 1;
  }
  sac.sa_flags = 0;
  if (sigaction(SIGTTIN, &sac, NULL) == -1 ||
      sigaction(SIGTTOU, &sac, NULL) == -1) {
    perror("sigaction");
    return 1;
  }

  strvec_t tokens;
  strvec_init(&tokens);
  job_list_t jobs;
  job_list_init(&jobs);
  char cmd[CMD_LEN];

  printf("%s", PROMPT);
  while (fgets(cmd, CMD_LEN, stdin) != NULL) {
    // Need to remove trailing '\n' from cmd. There are fancier ways.
    int i = 0;
    while (cmd[i] != '\n') {
      i++;
    }
    cmd[i] = '\0';

    if (tokenize(cmd, &tokens) != 0) {
      printf("Failed to parse command\n");
      strvec_clear(&tokens);
      job_list_free(&jobs);
      return 1;
    }
    if (tokens.length == 0) {
      printf("%s", PROMPT);
      continue;
    }
    const char *first_token = strvec_get(&tokens, 0);

    if (strcmp(first_token, "pwd") == 0) {
      char cwd[CMD_LEN];
      if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s\n", cwd);
      } else {
        perror("getcwd");
      }
    }

    else if (strcmp(first_token, "cd") == 0) {
      if (tokens.length > 1) {
        if (chdir(strvec_get(&tokens, 1)) == -1) {
          perror("chdir");
        }
      } else {
        const char *home = getenv("HOME");
        if (home == NULL) {
          printf("HOME not set\n");
        } else {
          if (chdir(home) == -1) {
            perror("chdir");
          }
        }
      }
    }

    else if (strcmp(first_token, "exit") == 0) {
      strvec_clear(&tokens);
      break;
    }

    // Task 5: Print out current list of pending jobs
    else if (strcmp(first_token, "jobs") == 0) {
      int i = 0;
      job_t *current = jobs.head;
      while (current != NULL) {
        char *status_desc;
        if (current->status == BACKGROUND) {
          status_desc = "background";
        } else {
          status_desc = "stopped";
        }
        printf("%d: %s (%s)\n", i, current->name, status_desc);
        i++;
        current = current->next;
      }
    }

    // Task 5: Move stopped job into foreground
    else if (strcmp(first_token, "fg") == 0) {
      if (resume_job(&tokens, &jobs, 1) == -1) {
        printf("Failed to resume job in foreground\n");
      }
    }

    // Task 6: Move stopped job into background
    else if (strcmp(first_token, "bg") == 0) {
      if (resume_job(&tokens, &jobs, 0) == -1) {
        printf("Failed to resume job in background\n");
      }
    }

    // Task 6: Wait for a specific job identified by its index in job list
    else if (strcmp(first_token, "wait-for") == 0) {
      if (await_background_job(&tokens, &jobs) == -1) {
        printf("Failed to wait for background job\n");
      }
    }

    // Task 6: Wait for all background jobs
    else if (strcmp(first_token, "wait-all") == 0) {
      if (await_all_background_jobs(&jobs) == -1) {
        printf("Failed to wait for all background jobs\n");
      }
    }

    else {
      pid_t pid = fork();
      if (pid == 0) {
        // Child process: Run the command
        run_command(&tokens);

        // Step 2: If execvp() fails, print error and exit child process
        perror("execvp");
        exit(1);
      } else if (pid > 0) {
        // Parent process: Handle foreground and background execution

        // Step 4: Set the child process as the target of signals sent to the
        // terminal via the keyboard
        tcsetpgrp(STDIN_FILENO, pid);

        // Step 6: If the last token is "&", run the command in the background
        int is_background =
            (strcmp(strvec_get(&tokens, tokens.length - 1), "&") == 0);
        if (is_background) {
          // Remove "&" from the token list
          strvec_take(&tokens, tokens.length - 1);

          // Step 5: In the parent, don't wait for the child to exit, just add
          // it to jobs
          job_t job;
          job.pid = pid;
          job.status = BACKGROUND;
          strncpy(job.name, strvec_get(&tokens, 0),
                  CMD_LEN); // Store command name
          job_list_add(&jobs, pid, strvec_get(&tokens, 0), BACKGROUND);
        } else {
          // Step 5: Wait for the child process if it's a foreground process
          int status;
          waitpid(pid, &status, WUNTRACED);

          // After waitpid(), set the shell as the target of terminal signals
          tcsetpgrp(STDIN_FILENO, getpid());

          // If the child was stopped, add it to jobs list
          if (WIFSTOPPED(status)) {
            job_t job;
            job.pid = pid;
            job.status = STOPPED;
            strncpy(job.name, strvec_get(&tokens, 0),
                    CMD_LEN); // Store command name
            job_list_add(&jobs, pid, strvec_get(&tokens, 0), STOPPED);
          }
        }
      } else {
        perror("fork");
      }
    }

    strvec_clear(&tokens);
    printf("%s", PROMPT);
  }

  job_list_free(&jobs);
  return 0;
}
