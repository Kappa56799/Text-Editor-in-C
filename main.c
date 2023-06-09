/*
Name: Kacper Palka

Info:
A Program written in C that creates a text editor allowing us to read,edit and save files using a Bash terminal.
It is inspired from kilo text editor as it is lightweight 
This was created for fun as I  love using C  and systems so I want to eventually create many programs like this in C/possibly create an OS system

Some Things to note:
"\x1b[H" = is an escape sequence to repostion the cursor back to the top left corner and is commonly used
https://vt100.net/docs/vt100-ug/chapter3.html#CPR all of the escape sequences can be found here (Yes there is a lot of reading)
https://github.com/antirez/kilo Based on this project

*/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

//Defines
#define KAPPA_VERSION "0.0.1"
#define TAB_STOP 8 
#define QUIT_TIMES 3 //how many times you need to press the quit button to quit
#define ABUF_INIT {NULL, 0} //Declare an empty Buffer  used for abuf type
#define CTRL_KEY(k) ((k) & 0x1f) //sets it to 00011111 in binary to mirror what it does in the terminal (sets the upper 3 bits to 0)

//Enums to allow other keys to work
enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

//Data used throughout the program
typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

struct editorConfig {
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios;
};

struct editorConfig E;

/*** append buffer ***/
struct abuf {
  char *b;
  int len;
};


//Function protypes 
void SetStatusMessage(const char *fmt, ...);
void ClearScreen();
char *Prompt(char *prompt);

//Terminal Settings
//Closes the program and returns an error
void Close(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  //prints a decriptive error message and exits the program
  perror(s);
  exit(1);
}

//Re enables all the terminal flags we turned off after program ends
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    Close("tcsetattr");
}

//We need to remove most of the flags to allow this program to work as a text editor in the terminal
void enableRawMode() {

  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) Close("tcgetattr");
  atexit(disableRawMode);

  /*
  We set these flags to allow us to prevent stuff like ctrl + c exiting the program, echoing which is useful normally, turning off
  canonical mode (ICANON) to allow us to read byte by  byte instead of line by line, ISIG allows us to read the bytes from ctrl-c (3) nd ctrl-z (26),
  IXON allows us to read bytes from the input of Ctrl-S (19) and ctrl-Q (17),
  IEXTEN prevents waiting from another input when ctrl-v is pressed this allows us to read ctrl-V as 22 bytes and Ctrl-o as 15 bytes
  ICRNL Turns off the feature of translating carraige returns and allows us to read CTRL + M as 13 bytes instead of 10
  We turn off OPOST flag
  BRKINT is turnedd off to stop ctrl+ c working
  INPCK enables parity checking (not needed these days)
  ISTRIP chages every 8th bit to a 0
  CS8 sets the character size to 8 bits per byte
  */
  struct termios raw = E.orig_termios;

  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  //Timeout for read() so it returns if no input is given
  raw.c_cc[VMIN] = 0; //min num of bytes too be read
  raw.c_cc[VTIME] = 1; //100ms timeout

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) Close("tcsetattr");
}


//reads a character presssed from STDIN  by the user
int ReadKey() {
  int nread;
  char c;
  
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) Close("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    /* ESC [ sequences. */
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } 

      else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } 

    else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

//Keeps Track of where the cursor  while the program is running
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  //reports the cursors location
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  //reads  the response ESC [ rows ; cols R 
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }

  buf[i] = '\0';

  //parses the value
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

//We get the size of the terminal window and display according to the display
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } 
  else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

//Row Stuff
/*
converts chars index into a render index
we use rx %  TAB_STOP  to find out how many columns we are to the right of the last tab stop, and then subtract that from KILO_TAB_STOP - 1 to find out how many columns we are to the left of the next tab stop
*/
int RowCXToRX(erow *row, int cx) {
  int rx = 0;
  int j;

  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (TAB_STOP - 1) - (rx % TAB_STOP);
    rx++;
  }

  return rx;
}

//Displays the new postion and allocates memory 
void UpdateRow(erow *row) {
  int tabs = 0;
  int j;

  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;

  /* Create a version of the row we can directly print on the screen,
   respecting tabs, substituting non printable characters with '?'. */
  free(row->render);
  row->render = malloc(row->size + tabs*(TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
    } 

    else {
      row->render[idx++] = row->chars[j];
    }
  }

  row->render[idx] = '\0';
  row->rsize = idx;
}

//insert a new row and allocate memory based on the amount of rows present
void InsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  UpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

//Free the memory used by the row if deleted
void FreeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

//Deletes a row and we free the memory using the function we made
void DelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  FreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

//Inserts the inputted column in the correct place in the row and column
void RowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  UpdateRow(row);
  E.dirty++;
}

//Append the string 's' at the end of a row 
void RowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  UpdateRow(row);
  E.dirty++;
}

//Deletes character from a row depending on its position  in the row and column
void RowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  UpdateRow(row);
  E.dirty++;
}

/*** editor operations ***/
//Inserts character into our text editor  based on what row and column we are on
void InsertChar(int c) {
  if (E.cy == E.numrows) {
    InsertRow(E.numrows, "", 0);
  }

  RowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

/* Inserting a newline is slightly complex as we have to handle inserting a
  newline in the middle of a line, splitting the line as needed */
void InsertNewline() {
  if (E.cx == 0) {
    InsertRow(E.cy, "", 0);
  } 

  else {
    // We are in the middle of a line. Split it between two rows. 
    erow *row = &E.row[E.cy];
    InsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    UpdateRow(row);
  }

  E.cy++;
  E.cx = 0;
}

//Deletes the character based on the cursors position at that time
void DelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    RowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    RowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    DelRow(E.cy);
    E.cy--;
  }
}


/* Turn the editor rows into a single heap-allocated string.
    Returns the pointer to the heap-allocated string and populate the
    integer pointed by 'buflen' with the size of the string, escluding
    the final nulterm. */
char *RowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

//opens a file that the user wants to edit
void OpenFile(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) Close("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
      linelen--;
    }
    InsertRow(E.numrows, line, linelen);
  }

  free(line);
  fclose(fp);
  E.dirty = 0;
}

//Save the current file on disk. Return 0 on success, 1 on error. 
void SaveFile() {
  if (E.filename == NULL) {
    E.filename = Prompt("Save as: %s (ESC to cancel)");
    if (E.filename == NULL) {
      SetStatusMessage("Save aborted");
      return;
    }
  }
  int len;
  char *buf = RowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        SetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  SetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/* We define a very simple "append buffer" structure, that is an heap
    allocated string where we can append to. This is useful in order to
    write all the escape sequences in a buffer and flush them to the standard
    output in a single call, to avoid flickering effects. */

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

void Scroll() {
  E.rx = 0;

  if (E.cy < E.numrows) {
    E.rx = RowCXToRX(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }

  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }

  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }

  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

void DrawRows(struct abuf *ab) {
  int y;

  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;

    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "KAPPA editor -- version %s", KAPPA_VERSION);

        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }

        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } 

      else {
        abAppend(ab, "~", 1);
      }
    } 
    else {
      int len = E.row[filerow].rsize - E.coloff;

      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;

      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

//Displays the status bar
void DrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];

  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.numrows,
    E.dirty ? "(modified)" : "");

  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cy + 1, E.numrows);

  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);

  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } 
    else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void DrawMessagebar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);

  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)

    abAppend(ab, E.statusmsg, msglen);
}

/* This function writes the whole screen using VT100 escape characters
    starting from the logical state of the editor in the global state 'E'. */
void ClearScreen() {
  Scroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  DrawRows(&ab);
  DrawStatusBar(&ab);
  DrawMessagebar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

//Status  Message
void SetStatusMessage(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);

  E.statusmsg_time = time(NULL);
}

/*** input ***/
char *Prompt(char *prompt) {
  size_t bufsize = 128;

  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    SetStatusMessage(prompt, buf);
    ClearScreen();
    int c = ReadKey();

    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } 

    else if (c == '\x1b') {
      SetStatusMessage("");
      free(buf);
      return NULL;
    } 

    else if (c == '\r') {
      if (buflen != 0) {
        SetStatusMessage("");
        return buf;
      }
    } 

    else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }

      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
  }
}

//Allows us to move the cursor using arrow keys in the program
void MoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
        E.cy++;
      }
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

//Processes the character/key the user inputted and displays the relevant action
void ProcessKey() {
  static int quit_times = QUIT_TIMES;

  int c = ReadKey();

  switch (c) {
    case '\r':
      /* TODO */
      break;

    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        SetStatusMessage("WARNING!!! File has unsaved changes. "
          "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      SaveFile();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;

    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY) MoveCursor(ARROW_RIGHT);
      DelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
          E.cy = E.rowoff + E.screenrows - 1;
          if (E.cy > E.numrows) E.cy = E.numrows;
        }

        int times = E.screenrows;
        while (times--)
          MoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      MoveCursor(c);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;

    default:
      InsertChar(c);
      break;
  }

  quit_times = QUIT_TIMES;
}

//Init of the program
void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) Close("getWindowSize");
  E.screenrows -= 2;
}

//Main of the program
int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    OpenFile(argv[1]);
  }

  SetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

  while (1) {
    ClearScreen();
    ProcessKey();
  }

  return 0;
}