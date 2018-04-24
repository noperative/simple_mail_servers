#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024
#define MAX_MESSAGE_LENGTH 65535

static void handle_client(int fd);

int main(int argc, char *argv[]) {
  
  if (argc != 2) {
    fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
    return 1;
  }
  
  run_server(argv[1], handle_client);
  
  return 0;
}

void handle_client(int fd) {
  /* We will keep track of the conversation using a variable that indicates
   * the state of the SMTP conversation.
   *
   * 0: no conversation
   * 1: HELO received
   * 2: MAIL received
   * 3: at least one RCPT received, ready for additional RCPT or DATA
   * 4: DATA received, now accepting message data
   * 5: finished receiving message data
   */
  int state = 0;
   
  user_list_t fromList = create_user_list();
  user_list_t toList = create_user_list();
  
  // The email message itself
  char messageBuffer[MAX_MESSAGE_LENGTH] = "";
  int messageBufferPointer = 0;

  // This server's hostname
  char hostname[255];
  gethostname(hostname, sizeof(hostname));

  // Send first greeting
  send_string(fd, "220 %s\r\n", hostname);

  char out[MAX_LINE_LENGTH];

  net_buffer_t nb = nb_create(fd, MAX_LINE_LENGTH);

  while (nb_read_line(nb, out) > 2) {
    // First get the 4 character command
    char command[4] = "";
    memcpy(command, out, 4);
	
    // message body supersedes all commands
    if (state == 4) {
      if (strncmp(out, ".\r\n", 3) == 0) {
        // Save the contents of the message buffer to a file
        char filename[] = "XXXXXX";
        int tempfd;

        tempfd = mkstemp(filename);
        write(tempfd, messageBuffer, strlen(messageBuffer));
        close(tempfd);
        
        save_user_mail(filename, toList);

        unlink(filename);

        send_string(fd, "250 OK\r\n");
        
        state = 5;
      }
      else if (messageBufferPointer < MAX_MESSAGE_LENGTH) {
        int remainingSize = MAX_MESSAGE_LENGTH - messageBufferPointer;
        int dataLength = strlen(out);
        int sizeToCopy = remainingSize < dataLength ? remainingSize : dataLength;
        
        memcpy(&messageBuffer[messageBufferPointer], out, sizeToCopy);     
        messageBufferPointer += sizeToCopy;
      }
      else {
        send_string(fd, "550 Exceeded maximum message length\r\n");
      }
    }
  
    // HELO
	  else if (strncmp(command, "HELO", 4) == 0) {
      if (strlen(out) < 8 || strncmp(&out[4], " ", 1) != 0 || strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0) {
        send_string(fd, "501 Syntax error in parameters or arguments\r\n");
      }
      else if (state != 0) {
        send_string(fd, "503 Bad sequence of commands\r\n");
      }
      else {
        // Respond to HELO by echoing the domain
        char domain[255] = "";
        memcpy(domain, &out[5], strlen(out) - 7);
        
        send_string(fd, "250 %s\r\n", domain);
        
        state = 1;
      }
    }
    // MAIL
    else if (strncmp(command, "MAIL", 4) == 0) {
      if (strlen(out) < 15 || strncmp(&out[4], " FROM:<", 7) != 0 || strncmp(&out[strlen(out) - 3], ">\r\n", 3) != 0) {
        send_string(fd, "501 Syntax error in parameters or arguments\r\n");
      }
      else if (state != 1) {
        send_string(fd, "503 Bad sequence of commands\r\n");
      }
      else {
        // Save the user
        char user[255] = "";
        memcpy(user, &out[11], strlen(out) - 14);
        add_user_to_list(&fromList, user);

        send_string(fd, "250 OK\r\n");
        
        state = 2;
      }
    }
    // RCPT
    else if (strncmp(command, "RCPT", 4) == 0) {
      if (strlen(out) < 13 || strncmp(&out[4], " TO:<", 5) != 0 || strncmp(&out[strlen(out) - 3], ">\r\n", 3) != 0) {
        send_string(fd, "501 Syntax error in parameters or arguments\r\n");
      }
      else if (state != 2 && state != 3) {
        send_string(fd, "503 Bad sequence of commands\r\n");
      }
      else {
        // Save the user, if the user is valid
        char user[255] = "";
        memcpy(user, &out[9], strlen(out) - 12);
          
        if (is_valid_user(user, NULL) != 0) {
          add_user_to_list(&toList, user);
            
          send_string(fd, "250 OK\r\n");
          
          state = 3;
        }
        else {
          send_string(fd, "550 Invalid user\r\n");
        } 
      }
    }
    // DATA
    else if (strncmp(command, "DATA", 4) == 0) {
      if (strlen(out) != 6 && strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0) {
        send_string(fd, "501 Syntax error in parameters or arguments\r\n");
      }
      else if (state != 3) {
        send_string(fd, "503 Bad sequence of commands\r\n");
      }
      else {
        send_string(fd, "354 Start mail input; end with <CRLF>.<CRLF>\r\n");
        
        state = 4;
      }
    }
    // NOOP
    else if (strncmp(command, "NOOP", 4) == 0) {
      send_string(fd, "250 OK\r\n");
    }
    // QUIT
    else if (strncmp(command, "QUIT", 4) == 0) {
      if (strlen(out) != 6) {
        send_string(fd, "501 Syntax error in parameters or arguments\r\n");
      }
      else {
        send_string(fd, "221 OK\r\n");
        break;
      }
    }
    // Unimplemented commands
    else if (strncmp(command, "EHLO", 4) == 0
      || strncmp(command, "RSET", 4) == 0
      || strncmp(command, "VRFY", 4) == 0
      || strncmp(command, "EXPN", 4) == 0
      || strncmp(command, "HELP", 4) == 0) {
      send_string(fd, "502 Command not implemented\r\n");
    }
    // Everything else
    else {
      send_string(fd, "500 Command unrecognized\r\n");
    }
  }
}
