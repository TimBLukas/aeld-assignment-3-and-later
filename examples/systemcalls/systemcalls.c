#include "systemcalls.h"
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{

/*
 * TODO  add your code here
 *  Call the system() function with the command set in the cmd
 *   and return a boolean true if the system() call completed with success
 *   or false() if it returned a failure
*/
	openlog(NULL, 0, LOG_USER);
	int returnCode = system(cmd);

	if ( returnCode == -1 ) {
		int saved_errno = errno;
		// systme cannot call specified module --> returned value -1
		fprintf(stderr, "system function was unable to call the specified command: %s\n", strerror(saved_errno));
		syslog(LOG_ERR, "system function was unable to call the specified command: %s\n", strerror(saved_errno));
		
		closelog();
		return false;
	
	} 
	
	bool success = WIFEXITED(returnCode) && WEXITSTATUS(returnCode) == 0;
	if (!success) {
		fprintf(stderr, "Command execution failed\n");
		syslog(LOG_ERR, "Command execution failed");
	}
	closelog();
	return success;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    va_end(args);
    command[count] = NULL;
	
/*
 * TODO:
 *   Execute a system command by calling fork, execv(),
 *   and wait instead of system (see LSP page 161).
 *   Use the command[0] as the full path to the command to execute
 *   (first argument to execv), and use the remaining arguments
 *   as second argument to the execv() command.
 *
*/
    pid_t pid = fork();
    
    if (pid < 0) {
    	fprintf(stderr, "Unable to fork a process: %s\n", strerror(errno));
    	return false;
    }
    
    if ( pid == 0 ) {
    	/* Child process */
    	execv(command[0], command);
    	
    	// Reachable only if execv() failes
    	fprintf(stderr, "execv() failed: %s\n", strerror(errno));
    	_exit(127);
    }
    
    // Parent process
    int status;
    pid_t result;
    
    do {
    	result = waitpid(pid, &status, 0);
    } while ( result == -1 && errno == EINTR);
    
    if ( result == -1) {
    	fprintf(stderr, "waitpid() failed: %s\n", strerror(errno));
    	return false;
    }
    
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }

    va_end(args);
    command[count] = NULL;

/*
 * TODO
 *   Call execv, but first using https://stackoverflow.com/a/13784315/1446624 as a refernce,
 *   redirect standard out to a file specified by outputfile.
 *   The rest of the behaviour is same as do_exec()
 *
*/

    int fd = open(outputfile, O_WRONLY|O_TRUNC|O_CREAT, 0644);

    if ( fd < 0 ) {
        fprintf(stderr, "Unable to open or create outputfile: %s\n", strerror(errno));
      	return false;
    }
    
    pid_t pid;
    switch ( pid = fork()) {
	case -1:
	    fprintf(stderr, "Unable to fork child process: %s\n", strerror(errno));
	    close(fd);
	    return false;
	case 0:
	    if ( dup2(fd, 1) < 0 ) {
		fprintf(stderr, "Unale to redirect stdout: %s\n", strerror(errno));
		close(fd);
		_exit(127);
	    }
	    close(fd);

	    execv(command[0], command);
	    
	    // Reachable only if execv() failes
	    fprintf(stderr, "execv() failed: %s\n", strerror(errno));
	    _exit(127);

	default:
	    close(fd);


	    // Parent process
	    int status;
	    pid_t result;
	    
	    do {
		result = waitpid(pid, &status, 0);
	    } while ( result == -1 && errno == EINTR);
	    
	    if ( result == -1) {
		fprintf(stderr, "waitpid() failed: %s\n", strerror(errno));
		return false;
	    }
	    
	    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

}
