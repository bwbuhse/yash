#include <fcntl.h>
#include <stdio.h>
#include <readline/readline.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Some macros used for keeping code nice
#define PROMPT "# "
#define OUTPUT_REDIR ">"
#define INPUT_REDIR "<"
#define ERR_REDIR "2>"
#define PIPE "|"

#define NUM_JOBIDS 20

// Used by the job struct
typedef enum status_t { RUNNING, STOPPED, DONE } status_t;
typedef int pgid_t;

// A struct used for holding parsed versions of commands
typedef struct process {
  char **argv; // Args for execvp
  char *output_file;
  char *input_file;
  char *error_file;
  bool isPipeArg1; // true when the command is on the left side of a pipe
  bool isPipeArg2; // true when the command is on the right side of a pipe
} process;

// A struct used to keep a list of active jobs
typedef struct job_t {
  int jobid;
  pgid_t pgid;
  char *jobstring;
  status_t status;
  struct job_t *next;
} job_t;

// A struct used for the setup_tok_cmd function
typedef struct setup_bools {
  bool *found_error;
  bool *redirect_found;
} setup_bools;

// A struct used for the setup_tok_cmd function
typedef struct setup_nums {
  int *num_tokens;
  int *start_index;
} setup_nums;

int tokenize(char **input, char **tokenized_input_ptr[]);
bool setup_tok_cmd(char **tokenized_input_ptr[], process *cmd, setup_nums *nums,
                   setup_bools *bools);
pgid_t create_child_proc(process *cmd, int pipefd[], int pgid_t);

bool add_job(volatile job_t *current_node, job_t *new_node);
job_t *remove_job(int jobid, job_t *current, job_t *previous);

void sighandler(int signo);

int find_next_jobid();

// This variable needs to be global so that the signal handler can go through it
volatile job_t *root;
bool job_ids[NUM_JOBIDS];

int main() {
  // Some important variables
  pid_t cpid1, cpid2;
  int status;
  int pipefd[2];

  // Set up signal handler(s)?
  signal(SIGCHLD, sighandler);
  signal(SIGTSTP, sighandler);
  signal(SIGINT, sighandler);

  // Main command loop
  while (true) {
    // Set up signal handler(S)
    signal(SIGCHLD, sighandler);
    signal(SIGTSTP, sighandler);

    // Read the input into the string and then tokenize it
    char *input = readline(PROMPT), *jobstring;
    char **tokenized_input;

    // Need this later but we have to dup because strtok garbifies its input
    if (input != NULL) {
      jobstring = strdup(input);
    }

    // Exit the shell if it's passed the EOF character
    if (input == NULL) {
      printf("\n");
      break;
    }

    // Nums used for looking for redirects
    int num_tokens = tokenize(&input, &tokenized_input);
    int start_index = 0;

    // Check for job control tokens
    bool isBackgroundJob = (tokenized_input[num_tokens - 1][0] == '&');
    bool isJobsCmd = !strcmp(tokenized_input[0], "jobs");
    bool isFgCmd = !strcmp(tokenized_input[0], "fg");
    bool isBgCmd = !strcmp(tokenized_input[0], "bg");

    // Do the work for fg
    if (isFgCmd) {
      volatile job_t *cnode = root;
      int pgid;

      while (cnode != NULL) {
        if (cnode->next == NULL) {
          pgid = cnode->pgid;
          kill(-(cnode->pgid), SIGCONT);
          kill(cnode->pgid, SIGCONT);

          cnode->status = RUNNING;

          printf("%s\n", cnode->jobstring);
          break;
        } else {
          cnode = cnode->next;
        }
      }

      // Blocking call to wait for the foreground job to finish
      waitpid(-pgid, &status, WUNTRACED);
      if (!WIFSTOPPED(status)) {
        remove_job(cnode->jobid, (job_t *)root, NULL);
      }

      continue;
    }

    // Do the work for bg
    if (isBgCmd) {
      volatile job_t *cnode = root;
      int pgid;

      while (cnode != NULL) {
        if (cnode->next == NULL) {
          pgid = cnode->pgid;
          kill(-(cnode->pgid), SIGCONT);
          kill(cnode->pgid, SIGCONT);

          cnode->status = RUNNING;

          printf("%s\n", cnode->jobstring);
          break;
        } else {
          cnode = cnode->next;
        }
      }

      waitpid(-cpid1, &status, WNOHANG | WUNTRACED);
      continue;
    }

    // Do the work for jobs
    if (isJobsCmd) {
      volatile job_t *cnode = root;

      // Go through all of the jobs
      while (cnode != NULL) {

        char *status_string;

        switch (cnode->status) {
        case RUNNING:
          status_string = "RUNNING";
          break;
        case STOPPED:
          status_string = "STOPPED";
          break;
        case DONE:
          status_string = "DONE";
          break;
        }

        printf("[%d] - %s\t\t%s\n", cnode->jobid, status_string,
               cnode->jobstring);

        cnode = cnode->next;
      }
      continue;
    }

    // If it's a background job we want to set the & to null because otherwise
    // it'll Confused the commands i.e. cat Makefile & would try to cat a file
    // named &
    if (isBackgroundJob) {
      tokenized_input[--num_tokens] = NULL;
    }

    // Check for any redirections in the command
    // cmd2 is only used if there's a pipe
    process cmd1 = {tokenized_input, NULL, NULL, NULL, false, false};
    process cmd2 = {NULL, NULL, NULL, NULL, false, false};

    // Flag used for if errors are found
    bool redirect_found = false;
    bool found_error = false;

    setup_nums nums = {&num_tokens, &start_index};
    setup_bools bools = {&found_error, &redirect_found};

    bool pipe_found = setup_tok_cmd(&tokenized_input, &cmd1, &nums, &bools);

    // If the user inputted a pipe, then parse for the second process
    if (pipe_found) {
      redirect_found = false;

      // Since this is the second command we have to change some things....
      cmd1.isPipeArg1 = true;
      cmd2.argv = tokenized_input + start_index;
      cmd2.isPipeArg2 = true;

      // Run the setup for the second process
      setup_tok_cmd(&tokenized_input, &cmd2, &nums, &bools);

      // Set up the pipe for use later
      pipe(pipefd);
    }

    // If I found an error then there's no point in trying
    // the command
    if (found_error) {
      continue;
    }

    // Create the child processes
    pgid_t cpid1 = create_child_proc(&cmd1, pipefd, -1), cpid2;
    setpgid(cpid1, 0);
    if (pipe_found) {
      cpid2 = create_child_proc(&cmd2, pipefd, cpid1);
    }

    // Set up the job struct for this input
    // TODO: Make this status accurate when BG jobs are added
    job_t *job = malloc(sizeof(job_t));
    job->jobid = find_next_jobid();
    job->pgid = cpid1;
    job->jobstring = jobstring;
    job->status = RUNNING;
    job->next = NULL;

    add_job(root, job);

    // Parent code
    free(tokenized_input);
    free(input);
    close(pipefd[0]);
    close(pipefd[1]);

    // Wait for the child processes to finish OR let them run in the background
    if (isBackgroundJob) {
      waitpid(-cpid1, &status, WNOHANG | WUNTRACED);
    } else {
      // Blocking call to wait for the foreground job to finish
      waitpid(-cpid1, &status, WUNTRACED);
      if (!WIFSTOPPED(status)) {
        remove_job(job->jobid, (job_t *)root, NULL);
      }
    }
  }

  return 0;
} // bottom of main

// Used to tokenize the user's input
// Returns the total number of tokens
int tokenize(char **input, char **tokenized_input_ptr[]) {
  // I used 70 because 3000/20~=70 :)
  char **tokenized_input = (char **)malloc(sizeof(char *) * 70);

  // Create my variables
  int num_tokens = 0;
  char *token;

  token = strtok(*input, " ");
  while (token != NULL) {
    tokenized_input[num_tokens++] = token;
    token = strtok(NULL, " ");
  }
  tokenized_input[num_tokens] = NULL;

  *tokenized_input_ptr = tokenized_input;

  return num_tokens;
}

// Used to set up a tokenized_cmd
// nums {&num_tokens, &start_index}
// bools {&error_found, &redirect_found}
bool setup_tok_cmd(char **tokenized_input_ptr[], process *cmd, setup_nums *nums,
                   setup_bools *bools) {

  bool pipe_found = false;
  int i;

  for (i = *(nums->start_index); i < *(nums->num_tokens); i++) {
    // Check for pipe
    if (!strcmp((*tokenized_input_ptr)[i], PIPE)) {
      if (i + 1 < *(nums->num_tokens)) {
        pipe_found = true;
        *(nums->start_index) = i + 1;
        (*tokenized_input_ptr)[i] = NULL;
        break;
      } else {
        *(bools->found_error) = true;
        break;
      }
    }

    // Check for various file redirections
    if (!strcmp((*tokenized_input_ptr)[i], OUTPUT_REDIR)) {
      *(bools->redirect_found) = true;

      if (i + 1 < *(nums->num_tokens)) {
        cmd->output_file = (*tokenized_input_ptr)[i + 1];
      } else {
        // The input was invalid if i >= num_tokens here (ended cmd w >)
        *(bools->found_error) = true;
        break;
      }
    } else if (!strcmp((*tokenized_input_ptr)[i], INPUT_REDIR)) {
      *(bools->redirect_found) = true;

      if (i + 1 < *(nums->num_tokens)) {
        cmd->input_file = (*tokenized_input_ptr)[i + 1];
      } else {
        // The input was invalid if i >= num_tokens here (ended cmd w >)
        *(bools->found_error) = true;
        break;
      }
    } else if (!strcmp((*tokenized_input_ptr)[i], ERR_REDIR)) {
      *(bools->redirect_found) = true;

      if (i + 1 < *(nums->num_tokens)) {
        cmd->error_file = (*tokenized_input_ptr)[i + 1];
      } else {
        // The input was invalid if i >= num_tokens here (ended cmd w >)
        *(bools->found_error) = true;
        break;
      }
    }

    // If a redirect hasn't been found yet, we know we're still adding args
    if (*(bools->redirect_found)) {
      (*tokenized_input_ptr)[i] = NULL;
    }
  }

  return pipe_found;
}

// Used to call fork and create child processes
// Returns the child's pid ??
pgid_t create_child_proc(process *cmd, int pipefd[], pgid_t pgid) {
  // Fork
  pid_t cpid = fork();
  if (cpid == 0) {
    // We don't care about users messing up I/O
    signal(SIGTTOU, SIG_IGN);

    // child code
    // Set pgid
    if (pgid != -1) {
      int id = setpgid(0, pgid);
    }

    // Do any pipe redirects
    if (cmd->isPipeArg1) {
      close(pipefd[0]);               // Close the unused read end
      dup2(pipefd[1], STDOUT_FILENO); // Redirect output to the pipe
    } else if (cmd->isPipeArg2) {
      close(pipefd[1]);              // Close the unused write end
      dup2(pipefd[0], STDIN_FILENO); // Redirect input to the pipe
    }

    // Do any file redirects
    if (cmd->output_file) {
      int ofd = open(cmd->output_file, O_CREAT | O_WRONLY | O_TRUNC,
                     S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
      dup2(ofd, STDOUT_FILENO);
    }
    if (cmd->input_file) {
      int ifd = open(cmd->input_file, O_RDONLY);

      // If the file doesn't exist then just skip this
      // command and go to the top
      if (ifd == -1) {
        return -1;
      }
      dup2(ifd, STDIN_FILENO);
    }
    if (cmd->error_file) {
      int ofd = open(cmd->error_file, O_CREAT | O_WRONLY | O_TRUNC,
                     S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
      dup2(ofd, STDERR_FILENO);
    }

    // Create a new process group
    // If this is the right child of a pipe, it will get moved to the left
    // child's group soon
    setpgid(0, 0);

    // validate is used to make sure that the command worked
    int validate = execvp(cmd->argv[0], cmd->argv);
    if (validate == -1) {
      exit(-1);
    }
  }

  return cpid;
}

// These two functions are used for adding/removed job_nodes to the job_node
// linked list
// returns true if succesfully added to the list, else false
bool add_job(volatile job_t *current_node, job_t *new_node) {
  if (current_node == NULL) {
    root = new_node;
    return true;
  } else if (current_node->next == NULL) {
    current_node->next = new_node;
    return true;
  } else {
    return add_job(current_node->next, new_node);
  }

  return false;
}

// returns a pointer to job_node with jobid = jobid param, else NULL
job_t *remove_job(int jobid, job_t *current, job_t *previous) {
  if (current == NULL) {
    // The jobid doesn't exist
    return NULL;
  } else if (current->jobid == jobid) {
    // We found the job!
    if (previous != NULL) {
      previous->next = current->next;
    } else {
      // If we get into this branch, we know that the job we found was the
      // root
      root = current->next;
    }
    current->next = NULL;                // Just so we don't accidentally use it
    job_ids[current->jobid - 1] = false; // Free up the job id!
    return current;
  } else {
    return remove_job(jobid, current->next, current);
  }
}

// signal handler
void sighandler(int signo) {
  volatile job_t *cnode = root;

  if (signo == SIGCHLD) {
    // Reap all the dead children lmao
    while (cnode != NULL) {
      // TODO: It doesn't know how to deal with stuff that dies in the
      // foreground? I think
      int pgid = waitpid(-1 * (cnode->pgid), 0, WNOHANG);

      if (pgid == cnode->pgid) {
        job_t *old = remove_job(cnode->jobid, (job_t *)root, NULL);
        printf("[%d] - Done\t\t%s\n", cnode->jobid, cnode->jobstring);
        free(old);
      }

      cnode = cnode->next;
    }
  } else if (signo == SIGTSTP) {
    // Look for the fg job to send to background
    while (cnode != NULL) {
      if (cnode->next == NULL) {
        kill(-(cnode->pgid), SIGTSTP);
        cnode->status = STOPPED;
        waitpid(-1 * (cnode->pgid), 0, WNOHANG | WUNTRACED);
        break;
      } else {
        cnode = cnode->next;
      }
    }
  } else if (signo == SIGINT) {
    // Look for the fg job to send SIGINT
    while (cnode != NULL) {
      if (cnode->next == NULL) {
        kill(-(cnode->pgid), SIGINT);
        waitpid(-1 * (cnode->pgid), 0, WNOHANG | WUNTRACED);
        break;
      } else {
        cnode = cnode->next;
      }
    }
  }
}

// Returns the next free job id
int find_next_jobid() {
  int id = -1;

  for (int i = 0; i < NUM_JOBIDS; i++) {
    if (!(job_ids[i])) {
      job_ids[i] = true;
      id = i;
      break;
    }
  }

  return id + 1;
}
