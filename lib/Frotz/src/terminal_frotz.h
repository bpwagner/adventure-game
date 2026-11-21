#ifndef TERMINAL_FROTZ_H
#define TERMINAL_FROTZ_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    long offset;
    long length;
    unsigned char version;
    unsigned char identity[10];  /* Release, serial number, checksum. */
} FrotzStory;

typedef struct {
    void (*output)(const char *, size_t);
    void (*upper_char)(int row, int column, char value);
    void (*erase)(int top, int left, int bottom, int right, int window);
    /* Return -1 for the game picker, 0 for timeout, or the key. */
    int (*read_line)(char *line, size_t limit, unsigned timeout_ms);
    int (*read_key)(unsigned timeout_ms);
    void (*idle)(void);
} FrotzTerminal;

int terminal_probe_story(const char *path, FrotzStory *story, const char **error);
int terminal_read_title(const char *path, char *title, size_t capacity);
int terminal_has_save(const char *path, const FrotzStory *story);
typedef enum {
    FROTZ_ERROR = -1,
    FROTZ_FINISHED,
    FROTZ_MENU
} FrotzResult;
FrotzResult terminal_run_story(const char *path, const FrotzStory *story, const FrotzTerminal *ui,
                               unsigned int seed, int resume);
const char *terminal_story_error(void);

long terminal_story_length(void);
void terminal_new_line(void);
int terminal_save_game(FILE *story_file);
int terminal_restore_game(FILE *story_file);

#ifdef __cplusplus
}
#endif

#endif
