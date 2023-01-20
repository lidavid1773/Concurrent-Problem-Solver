#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "debug.h"
#include "polya.h"

// Global flags (which will be set by signal handlers)
static volatile sig_atomic_t sigchldFlag = 0;

// Signal Handler Functions
// The polya master process must install a SIGCHLD handler so that it can be notified when worker processes stop and continue.
void sigchldHandler(int sig){
    sigchldFlag = 1;
}

// Helper functions
// Writes prob into fd.
void giveProblem(struct problem* prob, int fd){
    if (write(fd, prob, prob->size) == -1){
        debug("Error calling write() in giveProblem\n");
    }
}

// Returns pointer to the result read from fd (allocated using malloc() so return value needs to be freed after processing)
struct result *getResult(int fd){
    // Read fixed-size result header (to get access to size)
    struct result *resultHeader = malloc(sizeof(struct result));
    if (read(fd, resultHeader, sizeof(struct result)) == -1) debug("Error calling read() in getResult");

    // Allocate memory for entire result
    struct result *res = malloc(resultHeader->size);

    // Copy over header into result
    memcpy(res, resultHeader, sizeof(struct result));

    // Copy over data into result
    size_t dataLength = res->size - sizeof(struct result);
    char *resultData = malloc(dataLength);
    if (read(fd, resultData, dataLength) == -1) debug("Error calling read()");
    char* currentData = resultData; // since resultData needs to be freed later
    int count = 0;
    while (count < dataLength){
        res->data[count] = *currentData;
        currentData++;
        count++;
    }

    free(resultHeader);
    free(resultData);

    return res;
}

struct worker {
    int id;
    pid_t pid;
    int state;
    int fd1[2]; // Pipe for sending results from worker to master. (fd1[0] is used for master to read)
    int fd2[2]; // Pipe for sending problems from master to work. (fd2[1] is used for master to write)
    struct problem *problemToSolve;
};

/*
 * master
 * (See polya.h for specification.)
 */
int master(int workers) {
    sf_start();

    // Install necessary signal handlers (signal handlers should set flags)
    signal(SIGPIPE, SIG_IGN); // ignore SIGPIPE, so that it is not inadvertently terminated by the premature exit of a worker process.
    signal(SIGCHLD, sigchldHandler);

    // Perform required initialization by creating a number of worker processes (and associated pipes) as specified by the workers parameter
    struct worker arr[workers];

    for (int i=0; i<workers; i++){
        arr[i].id = i;
        // Each time the master process creates a worker process, it creates two pipes for communicating w/ that worker process
        // One pipe for sending results from the worker to the master
        if (pipe(arr[i].fd1) == -1){
            debug("Error: pipe() returned -1\n");
        }
        // One pipe for sending problems from the master to the worker
        if (pipe(arr[i].fd2) == -1){
            debug("Error: pipe() returned -1\n");
        }

        int childPid = fork();
        if (childPid == -1) debug("Error: fork() returned -1\n");

        // Child process (which will be the worker)
        if (childPid == 0){
            // Redirect workers standard input so that worker always reads problems from its stdin (fd2[1] is used for master to write)
            dup2(arr[i].fd2[0], 0);
            close(arr[i].fd2[1]);

            // Redirect workers standard output so that worker always writres results to its stdout (fd1[0] is used for master to read)
            dup2(arr[i].fd1[1], 1);
            close(arr[i].fd1[0]);

            // Worker program itself is executed using one of the system calls in the exec(3) family
            execl("bin/polya_worker", "polya_worker", NULL);
        }
        else{
            arr[i].pid = childPid;
        }
    }

    // Set each worker to started
    for (int i=0; i<workers; i++){
        arr[i].state = WORKER_STARTED;
    }

    debug("all workers started\n");

    // Once a worker receives SIGSTOP (STOPPED), they go from started -> idle (every worker should do this before continuing)
    int idleCounter = 0;
    while (idleCounter < workers){
        pid_t wpid = waitpid(-1, NULL, WUNTRACED);
        // Update state
        for (int i=0; i<workers; i++){
            if (arr[i].pid == wpid){
                arr[i].state = WORKER_IDLE;
                sf_change_state(arr[i].pid, WORKER_STARTED, WORKER_IDLE);
            }
        }

        idleCounter++;
    }

    debug("all workers idle\n");

    // Now that every worker is IDLE, repeatedly assign problems to idle workers and post results received from workers,
    // until finally all of the worker processes have become idle and a NULL return from the get_problem_variant function
    // indicates that there are no further problems to be solved.

    int moreProblems = 1;
    while(moreProblems){
        // Assign problem variant to each worker
        int variant = 0;
        for (int i=0; i<workers; i++){
            if (moreProblems){
                struct problem *currentProblemVariant = get_problem_variant(workers, variant);
                if (currentProblemVariant == NULL){
                    moreProblems = 0;
                }
                else{
                    variant++;
                    // Signal worker process to continue
                    kill(arr[i].pid, SIGCONT);

                    // Update state
                    arr[i].state = WORKER_CONTINUED;
                    sf_change_state(arr[i].pid, WORKER_IDLE, WORKER_CONTINUED);
                    // use waitpid to confirm that the worker process is no longer stopped
                    // once this happens, change state from CONTINUED -> RUNNING
                    waitpid(arr[i].pid, NULL, WCONTINUED);
                    arr[i].state = WORKER_RUNNING;
                    sf_change_state(arr[i].pid, WORKER_CONTINUED, WORKER_RUNNING);

                    // Send problem to worker
                    arr[i].problemToSolve = currentProblemVariant;
                    sf_send_problem(arr[i].pid, currentProblemVariant);
                    giveProblem(currentProblemVariant, arr[i].fd2[1]);
                }
            }
        }

        debug("all workers running\n");

        if (moreProblems){
            // Continuously wait for child processes to stop until one of them sends a correct result
            int currentProblemNotSolved = 1;
            while (currentProblemNotSolved){
                pid_t wpid = waitpid(-1, NULL, WUNTRACED);

                // Find worker that has stopped
                int workerIndex;
                for (int i=0; i<workers; i++){
                    if (arr[i].pid == wpid) workerIndex = i;
                }

                // Update state
                arr[workerIndex].state = WORKER_STOPPED;
                sf_change_state(arr[workerIndex].pid, WORKER_RUNNING, WORKER_STOPPED);
                arr[workerIndex].state = WORKER_IDLE;
                sf_change_state(arr[workerIndex].pid, WORKER_STOPPED, WORKER_IDLE);
                debug("worker %i stopped\n", workerIndex);

                // Read result
                struct result *res = getResult(arr[workerIndex].fd1[0]);
                sf_recv_result(arr[workerIndex].pid, res);

                // If result solves problem, currentProblemNotSolved = 0
                int postResult = post_result(res, arr[workerIndex].problemToSolve);
                free(res); // function uses malloc to allocate space for a result, so must free.
                if (postResult == 0){
                    debug("current problem solved, will cancel running workers\n");
                    currentProblemNotSolved = 0;
                }
            }

            // Once a correct solution has been found, send SIGHUP signal to cancel necessary workers
            for (int i=0; i<workers; i++){
                if (arr[i].state != WORKER_IDLE){
                    sf_cancel(arr[i].pid);
                    kill(arr[i].pid, SIGHUP);
                    arr[i].state = WORKER_STOPPED;
                    sf_change_state(arr[i].pid, WORKER_RUNNING, WORKER_STOPPED);
                    waitpid(arr[i].pid, NULL, WUNTRACED);
                    debug("worker %i has been canceled. now stopped\n", i);
                    arr[i].state = WORKER_IDLE;
                    sf_change_state(arr[i].pid, WORKER_STOPPED, WORKER_IDLE);
                }
            }
        }
    }

    // Close necessary file descriptors
    for (int i=0; i<workers; i++){
        close(arr[i].fd2[0]);
        close(arr[i].fd1[1]);
    }

    // Send a SIGTERM signal to each of the worker processes, which catch this signal and then exit normally.
    for (int i=0; i<workers; i++){
        kill(arr[i].pid, SIGTERM);
        debug("sent sigterm\n");
        kill(arr[i].pid, SIGCONT);
    }

    // When all of the worker processes have terminated, the master process itself also terminates.
    int child_status;
    for (int i=0; i<workers; i++){
        pid_t wpid = wait(&child_status); // Wait for any worker to terminate
        // If not all of the worker processes have terminated normally with EXIT_SUCCESS, master process exits with status EXIT_FAILURE
        if (!WIFEXITED(child_status)){
            for (int i=0; i<workers; i++){
                if (arr[i].pid == wpid){
                    arr[i].state = WORKER_ABORTED;
                    sf_change_state(arr[i].pid, WORKER_IDLE, WORKER_ABORTED);
                }
            }
            sf_end();
            exit(EXIT_FAILURE);
        }
        else{
            for (int i=0; i<workers; i++){
                if (arr[i].pid == wpid){
                    arr[i].state = WORKER_EXITED;
                    sf_change_state(arr[i].pid, WORKER_IDLE, WORKER_EXITED);
                }
            }
        }
    }

    sf_end();
    exit(EXIT_SUCCESS);
}



// Notes

// To research:
// wait, waitpid
// can use waitpid with a WNOHANG to check for all children that stopped to process them one at a time
// WUNTRACED?
// Event functions
// Use val grind to see any memory leaks when done
// Verify closing appropriate file descriptors