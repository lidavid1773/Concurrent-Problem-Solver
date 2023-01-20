#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "debug.h"
#include "polya.h"

// Global flags (which will be set by signal handlers)
static volatile sig_atomic_t readAndSolve = 0;
static volatile sig_atomic_t cancel = 0;
static volatile sig_atomic_t terminate = 0;



// Signal Handler Functions

// Handler for SIGHUP.
void sighupHandler(int sig){
    cancel = 1;
}

// Handler for SIGTERM.
void sigtermHandler(int sig){
    terminate = 1;
}

// Handler for SIGCONT.
void sigcontHandler(int sig){
    readAndSolve = 1;
}



/*
 * worker
 * (See polya.h for specification.)
 */

int worker(void) {
    // Install necessary signal handlers (signal handlers should set flags)
    signal(SIGHUP, sighupHandler);
    signal(SIGTERM, sigtermHandler);
    signal(SIGCONT, sigcontHandler);

    // Self send SIGSTOP to initialize worker
    kill(getpid(), SIGSTOP);

    // While loop to handle received signals:
    struct problem *prob;
    while(1){
        if (terminate){
            terminate = 0;
            exit(EXIT_SUCCESS);
        }

        if (readAndSolve){
            readAndSolve = 0;

            // Read the fixed-size problem header (to get access to size)
            struct problem *problemHeader = malloc(sizeof(struct problem));
            ssize_t readHeaderResult = read(0, problemHeader, sizeof(struct problem));
            if (readHeaderResult == -1) perror("readHeaderResult");
            debug("Header length (from read): %li for problem id: %i", readHeaderResult, problemHeader->id);

            // Use malloc() to allocate storage for for our entire problem.
            prob = malloc(problemHeader->size);

            // Put entire problem into allocated storage (header + data):
            // Copy problemHeader over to prob
            memcpy(prob, problemHeader, sizeof(struct problem));

            // Read the problem data (# of bytes is equal to: size - sizeof(struct problem)) byte by byte using fgetc into prob
            size_t dataLength = prob->size - sizeof(struct problem);
            if (dataLength < 0) dataLength = 0;

            char *problemData = malloc(dataLength);
            ssize_t readDataResult = read(0, problemData, dataLength);
            if (readDataResult == -1) perror("readDataResult");
            debug("Data length (from read): %li for problem id: %i", readDataResult, prob->id);
            char *currentData = problemData; // because problemData needs to be freed later

            // Copy over data
            int count = 0;
            while (count < dataLength){
                prob->data[count] = *currentData;
                currentData++;
                count++;
            }
            debug("Problem %i is type %i and has size %li\n", prob->id, prob->type, prob->size);

            // Now that we have the problem, solve it:
            int type = prob->type;
            volatile sig_atomic_t canceledP = 0;
            struct result *res = solvers[type].solve(prob, &canceledP); // res is NULL if solver was canceled or otherwise failed

            // Either write the failed result, or the actual result
            if (res == NULL){
                cancel = 1;
                debug("Received failed result (due to cancellation) for problem %i\n", prob->id);
            }
            else if (res != NULL && res->failed != 0){  // Result failed from solver
                // Ensure that result was not cancelled in case a SIGUP signal comes after a failed result should be sent
                cancel = 0;
                res = malloc(sizeof(struct result));
                res->failed = 1;
                res->size = sizeof(struct result);
                res->id = prob->id;
                ssize_t writeResult = write(1, res, sizeof(struct result));
                if (writeResult == -1) perror("writeResult");
                debug("Sent failed result (due to solver failure) for problem %i\n", prob->id);
            }
            else{
                // Ensure that result was not cancelled in case a SIGHUP signal comes after nonfailed result should be sent
                cancel = 0;
                ssize_t writeResult = write(1, res, res->size);
                if (writeResult == -1) perror("writeResult");
                debug("Sent correct result with result size: %li for problem %i\n", res->size, prob->id);
            }

            // Free mallocd memory
            free(problemHeader);
            free(problemData);
            free(res);

            if (!cancel){
                free(prob);
                kill(getpid(), SIGSTOP);
            };

            if (res != NULL) cancel = 0;
        }

        if (cancel){
            cancel = 0;
            /*
                The SIGHUP signal is sent by the master process to notify a worker to cancel its current solution attempt.
                When a worker process receives SIGHUP, if the current solution attempt has not already succeeded or failed,
                    then it is abandoned and a result marked "failed" is sent to the master process before the worker stops by
                    sending itself a SIGSTOP signal.
            */

            struct result* res = malloc(sizeof(struct result));
            res->failed = 1;
            res->size = sizeof(struct result);
            res->id = prob->id;

            ssize_t writeResult = write(1, res, sizeof(struct result));
            if (writeResult == -1) perror("writeResult");
            debug("Sent failed result (due to cancellation) for problem %i\n", prob->id);
            free(res);
            free(prob);
            kill(getpid(), SIGSTOP);
        }
    }
}