# Concurrent-Problem-Solver

A program which manages a collection of worker processes that concurrently engage in solving computationally intensive problems.

The problems are obtained from a problem source, and the type of problems that are being solved are not important to the core logic of the program.

When a worker processes receives a problem from the master process, it begins trying to solve that problem, until:

(1) A solution is found.\
(2) The solution procedure fails.\
(3) The master process notifies the worker to cancel the solution process.
