#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024

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
   * the state of the POP3 conversation.
   *
   * 0: no conversation
   * 1: valid USER received
   * 2: valid PASS received, now in TRANSACTION state
   */
  int state = 0;

  // This server's hostname
  char hostname[255];
  gethostname(hostname, sizeof(hostname));

  char user[255] = "";
  char pass[255] = "";
  mail_list_t mailList;
  int initialMailCount;

  // Send first greeting
  send_string(fd, "+OK POP3 %s server ready\r\n", hostname);
  
  char out[MAX_LINE_LENGTH];

  net_buffer_t nb = nb_create(fd, MAX_LINE_LENGTH);

  while (nb_read_line(nb, out) > 2) {
    // First get the 4 character command
    char command[4] = "";
    memcpy(command, out, 4);

    // USER
    if (strncmp(command, "USER", 4) == 0) {
      if (strlen(out) < 8 || strncmp(&out[4], " ", 1) != 0 || strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0) {
        send_string(fd, "-ERR Syntax error\r\n");
      }
      else if (state != 0) {
        send_string(fd, "-ERR Bad sequence of commands\r\n");
      }
      else {
        char temp[255] = "";
        memcpy(temp, &out[5], strlen(out) - 7);

        if (is_valid_user(temp, NULL) != 0) {
          memcpy(user, temp, strlen(temp));
          send_string(fd, "User %s accepted\n", user);
  
          state = 1;
        }
        else {
          send_string(fd, "-ERR No user %s found\r\n", temp);
        }
      }
    }
    // PASS
    else if (strncmp(command, "PASS", 4) == 0) {
      if (strlen(out) < 8 || strncmp(&out[4], " ", 1) != 0 || strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0) {
        send_string(fd, "-ERR Syntax error\r\n");
      }
      else if (state != 1) {
        send_string(fd, "-ERR Bad sequence of commands\r\n");
      }
      else {
        char temp[255] = "";
        memcpy(temp, &out[5], strlen(out) - 7);

        if (is_valid_user(user, temp) != 0) {
          memcpy(temp, pass, strlen(temp));
          mailList = load_user_mail(user);
          initialMailCount = get_mail_count(mailList);

          send_string(fd, "+OK Mailbox open\r\n");

          state = 2;
        }
        else {
          send_string(fd, "-ERR Invalid user/password combination\r\n");
        }
      }
    }
    // STAT
    else if (strncmp(command, "STAT", 4) == 0) {
      if (strlen(out) != 6 || strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0) {
        send_string(fd, "-ERR Syntax error\r\n");
      }
      else if (state != 2) {
        send_string(fd, "-ERR Bad sequence of commands\r\n");
      }
      else {
        int mailCount = get_mail_count(mailList);
        size_t mailListSize = get_mail_list_size(mailList);

        send_string(fd, "+OK %d %d\r\n", mailCount, (int) mailListSize);
      }
    }
    // LIST
    else if (strncmp(command, "LIST", 4) == 0) {
      if ((strlen(out) != 6 && !( strlen(out) >= 8 && strncmp(&out[4], " ", 1) == 0))
        || strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0 ) {
        send_string(fd, "-ERR Syntax error\r\n");
      }
      if (state != 2) {
        send_string(fd, "-ERR Bad sequence of commands\r\n");
      }
      else {
        if (strlen(out) == 6) {
          send_string(fd, "+OK scan listing follows\r\n");

          int i;
          for (i = 0; i < initialMailCount; i++) {
            mail_item_t mailItem = get_mail_item(mailList, i);

            if (mailItem != NULL) {
              size_t mailItemSize = get_mail_item_size(mailItem);
              send_string(fd, "%d %d\r\n", i+1, (int) mailItemSize);
            }
          }
          send_string(fd, ".\r\n");
        }
        else {
          char argument[255] = "";
          memcpy(argument, &out[5], strlen(out) - 7);
          int messageNumber = atoi(argument);

	  if (messageNumber == 0) {
            send_string(fd, "-ERR no such message\r\n");
          }
          else {
            mail_item_t mailItem = get_mail_item(mailList, messageNumber - 1); // get_mail_item expects a 0 based index

            if (mailItem != NULL) {
              size_t mailItemSize = get_mail_item_size(mailItem);
              send_string(fd, "+OK %d %d\r\n", messageNumber, (int) mailItemSize);
            }
            else {
              send_string(fd, "-ERR no such message\r\n");
            }
          }
        }
      }      
    }
    // RETR
    else if (strncmp(command, "RETR", 4) == 0) {
      if (strlen(out) < 8 || strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0) {
        send_string(fd, "-ERR Syntax error\r\n");
      }
      else if (state != 2) {
        send_string(fd, "-ERR Bad sequence of commands\r\n");
      }
      else {
        char argument[255] = "";
        memcpy(argument, &out[5], strlen(out) - 7);
        int messageNumber = atoi(argument);

        if (messageNumber == 0) {
          send_string(fd, "-ERR no such message\r\n");
        }
        else {
          mail_item_t mailItem = get_mail_item(mailList, messageNumber - 1); // get_mail_item expects a 0 based index

          if (mailItem != NULL) {
            FILE *file = fopen(get_mail_item_filename(mailItem), "r");
            
            char line[1024];

            if (file) {
              while(fgets(line, sizeof(line), file)) {
                send_string(fd, "%s", line);
              }
              send_string(fd, ".\r\n");
              fclose(file);
            } 
          }
          else {
            send_string(fd, "-ERR no such message\r\n");
          }
        }
      }
    }
    else if (strncmp(command, "DELE", 4) == 0) {
      if (strlen(out) < 8 || strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0) {
        send_string(fd, "-ERR Syntax error\r\n");
      }
      else if (state != 2) {
        send_string(fd, "-ERR Bad sequence of commands\r\n");
      }
      else {
        char argument[255] = "";
        memcpy(argument, &out[5], strlen(out) - 7);
        int messageNumber = atoi(argument);

        if (messageNumber == 0) {
          send_string(fd, "-ERR no such message\r\n");
        }
        else {
          mail_item_t mailItem = get_mail_item(mailList, messageNumber - 1); // get_mail_item expects a 0 based index

          if (mailItem != NULL) {
            mark_mail_item_deleted(mailItem);

            send_string(fd, "+OK message deleted\r\n");
          }
          else {
            send_string(fd, "-ERR no such message\r\n");
          }
        }
      }
    }
    // RSET
    else if (strncmp(command, "RSET", 4) == 0) {
      if (strlen(out) != 6 || strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0) {
        send_string(fd, "-ERR Syntax error\r\n");
      }
      else if (state != 2) {
        send_string(fd, "-ERR Bad sequence of commands\r\n");
      }
      else {
         int recoveredMailCount = reset_mail_list_deleted_flag(mailList);

         send_string(fd, "+OK Recovered %d messages.\r\n", recoveredMailCount);
      }
    }
    // NOOP
    else if (strncmp(command, "NOOP", 4) == 0) {
      if (strlen(out) != 6 || strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0) {
        send_string(fd, "-ERR Syntax error\r\n");
      }
      else if (state != 2) {
        send_string(fd, "-ERR Bad sequence of commands\r\n");
      }
      else {
        send_string(fd, "+OK\r\n");
      }
    }
    // QUIT
    else if (strncmp(command, "QUIT", 4) == 0) {
      if (strlen(out) != 6 || strncmp(&out[strlen(out) - 2], "\r\n", 2) != 0) {
        send_string(fd, "-ERR Syntax error\r\n");
      }
      else if (state == 2) {
        destroy_mail_list(mailList); // How do we know if there was an error?

        send_string(fd, "+OK Goodbye!\r\n");
        break;
      }
      else {
        send_string(fd, "+OK Goodbye!\r\n");
        break;
      }
    }
    // Unimplemented commands
    else if (strncmp(command, "TOP", 3) == 0
      || strncmp(command, "UIDL", 4) == 0
      || strncmp(command, "APOP", 4) == 0 ) {
      send_string(fd, "-ERR Command not implemented\r\n");
    }
    // Everything else
    else {
      send_string(fd, "-ERR Command unrecognized\r\n");
    }
  }
}
