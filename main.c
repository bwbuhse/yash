#include <fcntl.h>
#include <readline/readline.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// TODO: Add process groups     [ ]
// TODO: Add file redirects     [x]
// TODO: Add pipes              [x]
// TODO: Add signal handling    [ ]
// TODO: Create bools_t & nums_t[x]

// Some macros used for keeping code nice
#define PROMPT "# "
#define OUTPUT_REDIR ">"
#define INPUT_REDIR "<"
#define ERR_REDIR "2>"
#define PIPE "|"

// A struct used for holding parsed versions of commands
typedef struct process {
  char **argv; // Args for execvp
  char *output_file;
  char *input_file;
  char *error_file;
  bool isPipeArg1; // true when the command is on the left side of a pipe
  bool isPipeArg2; // true when the command is on the right side of a pipe

} process;

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
int create_child_proc(process *cmd, int pipefd[]);

int main() {
  pid_t cpid1, cpid2;
  int status;
  int pipefd[2];

  // Main command loop
  while (true) {
    // Read the input into the string and then tokenize it
    char *input = readline(PROMPT);
    char **tokenized_input;

    // Exit the shell if it's passed the EOF character
    if (input == NULL) {
      printf("\n");
      break;
    }

    // Nums used for looking for redirects
    int num_tokens = tokenize(&input, &tokenized_input);
    int start_index = 0;

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

    int cpid1 = create_child_proc(&cmd1, pipefd), cpid2;
    if (pipe_found) {
      cpid2 = create_child_proc(&cmd2, pipefd);
    }

    // Parent code
    free(tokenized_input);
    free(input);
    close(pipefd[0]);
    close(pipefd[1]);

    // Wait for the child processes to finish
    // TODO: Update this whenever I add background processes
    waitpid(cpid1, &status, 0);
    if (pipe_found) {
      waitpid(cpid2, &status, 0);
    }
  }

  return 0;
}

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
        (*cmd).output_file = (*tokenized_input_ptr)[i + 1];
      } else {
        // The input was invalid if i >= num_tokens here (ended cmd w >)
        *(bools->found_error) = true;
        break;
      }
    } else if (!strcmp((*tokenized_input_ptr)[i], INPUT_REDIR)) {
      *(bools->redirect_found) = true;

      if (i + 1 < *(nums->num_tokens)) {
        (*cmd).input_file = (*tokenized_input_ptr)[i + 1];
      } else {
        // The input was invalid if i >= num_tokens here (ended cmd w >)
        *(bools->found_error) = true;
        break;
      }
    } else if (!strcmp((*tokenized_input_ptr)[i], ERR_REDIR)) {
      *(bools->redirect_found) = true;

      if (i + 1 < *(nums->num_tokens)) {
        (*cmd).error_file = (*tokenized_input_ptr)[i + 1];
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

  // Used to return the value, needed for when a pipe is found
  // *(nums->start_index) = i;

  return pipe_found;
}

// Used to call fork and create child processes
// Returns the child's pid ??
int create_child_proc(process *cmd, int pipefd[]) {
  // Fork
  int cpid = fork();
  if (cpid == 0) {
    // child code
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

    // validate is used to make sure that the command worked
    int validate = execvp(cmd->argv[0], cmd->argv);
    if (validate == -1) {
      exit(-1);
    }
  }

  return cpid;
}
