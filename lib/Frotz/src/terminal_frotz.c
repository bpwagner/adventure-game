/* Freenove terminal interface for Frotz. SPDX-License-Identifier: GPL-2.0-or-later */
#include "frotz.h"
#include "terminal_frotz.h"

#include <ctype.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum { TERMINAL_COLUMNS = 38, TERMINAL_ROWS = 17, STORY_PATH_SIZE = 256 };
typedef struct {
    long end;
    long next;
} IffChunk;

static FrotzStory active_story;
static const FrotzTerminal *terminal;
static char story_path[STORY_PATH_SIZE];
static char error_text[128];
static jmp_buf run_boundary;
static unsigned int next_seed;
static unsigned int ticks;
static int cursor_row, cursor_column;
static FILE *save_stream;
static unsigned char *restore_backup;

f_setup_t f_setup;
extern void interpret(void);
extern void init_memory(void);
extern void init_undo(void);
extern void reset_memory(void);
extern void set_more_prompts(bool);
extern int frotz_restore_game(void);
extern zword save_quetzal(FILE *, FILE *);
extern zword restore_quetzal(FILE *, FILE *);

static uint32_t be32(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static int read_at(FILE *file, long offset, unsigned char *bytes, size_t length)
{
    return fseek(file, offset, SEEK_SET) == 0 && fread(bytes, 1, length, file) == length;
}

static long form_end(FILE *file, const char *type)
{
    unsigned char header[12];
    if (fseek(file, 0, SEEK_END) != 0)
        return 0;
    const long length = ftell(file);
    if (length < (long)sizeof(header) || !read_at(file, 0, header, sizeof(header)) ||
        memcmp(header, "FORM", 4) || memcmp(header + 8, type, 4))
        return 0;
    const uint32_t size = be32(header + 4);
    if (size < 4 || size > (unsigned long)(length - 8))
        return 0;
    return (long)size + 8;
}

static int chunk_at(FILE *file, long start, long limit, unsigned char *header, IffChunk *chunk)
{
    if (start < 12 || start > limit - 8 || !read_at(file, start, header, 8))
        return 0;
    const uint32_t size = be32(header + 4);
    if ((uint64_t)size + (size & 1U) > (unsigned long)(limit - start - 8))
        return 0;
    chunk->end = start + 8 + (long)size;
    chunk->next = chunk->end + (size & 1U);
    return 1;
}

static int blorb_executable(FILE *file, FrotzStory *story, const char **error)
{
    const long end = form_end(file, "IFRS");
    unsigned char bytes[12];
    IffChunk index;
    if (!end || !chunk_at(file, 12, end, bytes, &index) || memcmp(bytes, "RIdx", 4) ||
        index.end < 24 || !read_at(file, 20, bytes, 4) ||
        be32(bytes) != (uint32_t)(index.end - 24) / 12 || (index.end - 24) % 12) {
        *error = "Invalid or truncated Blorb index.";
        return 0;
    }

    for (long entry = 24; entry < index.end; entry += 12) {
        if (!read_at(file, entry, bytes, sizeof(bytes)))
            break;
        if (memcmp(bytes, "Exec", 4) || be32(bytes + 4) != 0)
            continue;

        const uint32_t offset = be32(bytes + 8);
        IffChunk executable;
        if (offset > (unsigned long)end || (offset & 1U) || offset < (unsigned long)index.next ||
            !chunk_at(file, (long)offset, end, bytes, &executable)) {
            *error = "Invalid Blorb executable location.";
            return 0;
        }
        if (memcmp(bytes, "ZCOD", 4)) {
            *error = "This Blorb contains no Z-machine game.";
            return 0;
        }
        story->offset = (long)offset + 8;
        story->length = executable.end - story->offset;
        return 1;
    }

    *error = "No executable in the Blorb file.";
    return 0;
}

int terminal_probe_story(const char *path, FrotzStory *story, const char **error)
{
    *error = "Could not open the story file.";
    memset(story, 0, sizeof(*story));
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;

    unsigned char header[64];
    int okay = 0;
    *error = "Invalid or truncated story file.";
    if (!read_at(file, 0, header, 4) || fseek(file, 0, SEEK_END) != 0)
        goto done;
    story->length = ftell(file);
    if (memcmp(header, "FORM", 4) == 0 && !blorb_executable(file, story, error))
        goto done;
    if (story->length < (long)sizeof(header) || !read_at(file, story->offset, header, sizeof(header)))
        goto done;

    story->version = header[0];
    if (story->version < 1 || story->version == 6 || story->version > 8) {
        *error = "Unsupported Z-machine story version.";
        goto done;
    }
    const unsigned int length_scale = story->version <= 3 ? 2 : story->version <= 5 ? 4 : 8;
    const long declared = ((header[26] << 8) | header[27]) * (long)length_scale;
    const long dynamic = (header[14] << 8) | header[15];
    if ((declared && (declared < 64 || declared > story->length)) ||
        dynamic < 64 || dynamic > (declared ? declared : story->length))
        goto done;
    if (declared)
        story->length = declared;
    memcpy(story->identity, header + 2, 2);
    memcpy(story->identity + 2, header + 18, 6);
    memcpy(story->identity + 8, header + 28, 2);
    okay = 1;
done:
    fclose(file);
    return okay;
}

static int bibliographic_title(const char *xml, char *title, size_t capacity)
{
    const char *section = strstr(xml, "<bibliographic>");
    const char *limit = section ? strstr(section, "</bibliographic>") : NULL;
    const char *start = section ? strstr(section, "<title>") : NULL;
    const char *end = start ? strstr(start, "</title>") : NULL;
    if (!end || (limit && end >= limit))
        return 0;

    static const struct { const char *name; char value; } entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}
    };
    size_t length = 0;
    int space = 0;
    for (const char *p = start + sizeof("<title>") - 1; p < end && length + 1 < capacity; ++p) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '&') {
            for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); ++i) {
                const size_t count = strlen(entities[i].name);
                if ((size_t)(end - p) >= count && memcmp(p, entities[i].name, count) == 0) {
                    ch = (unsigned char)entities[i].value;
                    p += count - 1;
                    break;
                }
            }
        }
        if (isspace(ch)) {
            space = length > 0;
        } else {
            if (space && length + 2 < capacity)
                title[length++] = ' ';
            space = 0;
            title[length++] = (char)ch;
        }
    }
    title[length] = '\0';
    return length != 0;
}

int terminal_read_title(const char *path, char *title, size_t capacity)
{
    if (!capacity)
        return 0;
    title[0] = '\0';
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;

    const long end = form_end(file, "IFRS");
    int found = 0;
    for (long offset = 12; offset < end;) {
        unsigned char header[8];
        IffChunk chunk;
        if (!chunk_at(file, offset, end, header, &chunk))
            break;
        if (memcmp(header, "IFmd", 4) == 0) {
            const size_t length = chunk.end - offset - 8;
            const size_t count = length < 16384 ? length : 16384;
            char *xml = malloc(count + 1);
            if (xml && read_at(file, offset + 8, (unsigned char *)xml, count)) {
                xml[count] = '\0';
                found = bibliographic_title(xml, title, capacity);
            }
            free(xml);
            break;
        }
        offset = chunk.next;
    }
    fclose(file);
    return found;
}

static void save_path(char *path, size_t capacity, const char *story, const char *suffix)
{
    snprintf(path, capacity, "%s%s", story, suffix);
}

static int valid_save(const char *path, const FrotzStory *story)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    unsigned char bytes[13];
    unsigned int present = 0;
    const long end = form_end(file, "IFZS");
    for (long offset = 12; offset < end;) {
        IffChunk chunk;
        if (!chunk_at(file, offset, end, bytes, &chunk)) {
            present = 0;
            break;
        }
        if (memcmp(bytes, "IFhd", 4) == 0) {
            if (chunk.end - offset < 21 || !read_at(file, offset + 8, bytes, sizeof(bytes)) ||
                memcmp(bytes, story->identity, sizeof(story->identity)) ||
                (be32(bytes + 9) & 0xFFFFFFU) >= (unsigned long)story->length) {
                present = 0;
                break;
            }
            present |= 1;
        } else if (memcmp(bytes, "Stks", 4) == 0) {
            present |= 2;
        } else if (!memcmp(bytes, "CMem", 4) || !memcmp(bytes, "UMem", 4)) {
            present |= 4;
        }
        offset = chunk.next;
    }
    fclose(file);
    return present == 7;
}

int terminal_has_save(const char *path, const FrotzStory *story)
{
    char name[STORY_PATH_SIZE + 16];
    save_path(name, sizeof(name), path, ".qzl");
    if (valid_save(name, story))
        return 1;
    save_path(name, sizeof(name), path, ".qzl.bak");
    return valid_save(name, story);
}

long terminal_story_length(void)
{
    return active_story.length;
}

FILE *os_load_story(void)
{
    FILE *file = fopen(story_path, "rb");
    if (file && fseek(file, active_story.offset, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    return file;
}

int os_storyfile_seek(FILE *file, long offset, int whence)
{
    long base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = ftell(file) - active_story.offset; break;
    case SEEK_END: base = active_story.length; break;
    default: return -1;
    }
    if (base < 0 || base > active_story.length || offset < -base ||
        offset > active_story.length - base)
        return -1;
    return fseek(file, active_story.offset + base + offset, SEEK_SET);
}

int os_storyfile_tell(FILE *file)
{
    return (int)(ftell(file) - active_story.offset);
}

int terminal_save_game(FILE *story_file)
{
    char newest[STORY_PATH_SIZE + 16], backup[sizeof(newest)], temporary[sizeof(newest)];
    save_path(newest, sizeof(newest), story_path, ".qzl");
    save_path(backup, sizeof(backup), story_path, ".qzl.bak");
    save_path(temporary, sizeof(temporary), story_path, ".qzl.tmp");

    save_stream = fopen(temporary, "wb");
    if (!save_stream)
        return 0;
    const int saved = save_quetzal(save_stream, story_file) == 1 && !ferror(story_file);
    const int closed = fclose(save_stream) == 0;
    save_stream = NULL;
    if (!saved || !closed || !valid_save(temporary, &active_story)) {
        remove(temporary);
        return 0;
    }

    // Keep a usable previous save if the write or either rename fails.
    if (valid_save(newest, &active_story)) {
        remove(backup);
        if (rename(newest, backup) != 0) {
            remove(temporary);
            return 0;
        }
    } else {
        remove(newest);
    }
    if (rename(temporary, newest) != 0) {
        remove(temporary);
        return 0;
    }
    return 1;
}

int terminal_restore_game(FILE *story_file)
{
    restore_backup = malloc(h_dynamic_size + sizeof(stack));
    if (!restore_backup)
        return 0;

    memcpy(restore_backup, zmp, h_dynamic_size);
    memcpy(restore_backup + h_dynamic_size, stack, sizeof(stack));
    const long old_pc = pcp;
    zword *const old_sp = sp;
    zword *const old_fp = fp;
    const zword old_frames = frame_count;
    char name[STORY_PATH_SIZE + 16];
    const char *suffixes[] = {".qzl", ".qzl.bak"};
    int restored = 0;

    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        save_path(name, sizeof(name), story_path, suffixes[i]);
        if (!valid_save(name, &active_story))
            continue;
        save_stream = fopen(name, "rb");
        if (!save_stream)
            continue;
        restored = restore_quetzal(save_stream, story_file) == 2;
        fclose(save_stream);
        save_stream = NULL;
        if (restored)
            break;
        memcpy(zmp, restore_backup, h_dynamic_size);
        memcpy(stack, restore_backup + h_dynamic_size, sizeof(stack));
        pcp = old_pc;
        sp = old_sp;
        fp = old_fp;
        frame_count = old_frames;
    }
    free(restore_backup);
    restore_backup = NULL;
    return restored;
}

FrotzResult terminal_run_story(const char *path, const FrotzStory *story, const FrotzTerminal *ui,
                               unsigned int seed, int resume)
{
    error_text[0] = '\0';
    if (strlen(path) >= sizeof(story_path)) {
        snprintf(error_text, sizeof(error_text), "Story filename is too long.");
        return FROTZ_ERROR;
    }
    strcpy(story_path, path);
    active_story = *story;
    terminal = ui;
    next_seed = seed;
    ticks = 0;
    memset(&f_setup, 0, sizeof(f_setup));
    f_setup.story_file = story_path;
    f_setup.undo_slots = 1;
    f_setup.err_report_mode = ERR_DEFAULT_REPORT_MODE;
    ostream_screen = TRUE;
    ostream_script = ostream_memory = ostream_record = istream_replay = message = FALSE;
    reset_memory_output();

    FrotzResult result;
    switch (setjmp(run_boundary)) {
    case 0:
        init_buffer();
        init_err();
        init_memory();
        init_process();
        init_sound();
        os_init_screen();
        init_undo();
        z_restart();
        set_more_prompts(FALSE);
        if (resume && !frotz_restore_game())
            os_fatal("Could not load a valid save.");
        if (resume) {
            const char message_text[] = "Saved game restored.\n";
            terminal->output(message_text, sizeof(message_text) - 1);
        }
        interpret();
        flush_buffer();
        result = FROTZ_FINISHED;
        break;
    case 2:
        result = FROTZ_MENU;
        break;
    default:
        result = FROTZ_ERROR;
        break;
    }

    if (save_stream) {
        fclose(save_stream);
        save_stream = NULL;
    }
    free(restore_backup);
    restore_backup = NULL;
    reset_memory();
    terminal = NULL;
    return result;
}

const char *terminal_story_error(void)
{
    return error_text;
}

void os_fatal(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(error_text, sizeof(error_text), format, args);
    va_end(args);
    longjmp(run_boundary, 1);
}

void os_init_screen(void)
{
    if (h_version <= V3) {
        h_config |= CONFIG_SPLITSCREEN;
        h_config &= ~CONFIG_NOSTATUSLINE;
    } else {
        h_config &= ~(CONFIG_COLOUR | CONFIG_PICTURES | CONFIG_BOLDFACE |
                      CONFIG_EMPHASIS | CONFIG_SOUND);
        h_config |= CONFIG_FIXED | CONFIG_TIMEDINPUT;
    }
    h_flags &= ~(GRAPHICS_FLAG | SOUND_FLAG | MOUSE_FLAG | MENU_FLAG | COLOUR_FLAG);
    h_screen_rows = h_screen_height = TERMINAL_ROWS;
    h_screen_cols = h_screen_width = TERMINAL_COLUMNS;
    h_font_height = h_font_width = 1;
    h_default_foreground = WHITE_COLOUR;
    h_default_background = BLACK_COLOUR;
    h_interpreter_number = INTERP_MSDOS;
    h_interpreter_version = 'F';
}

void terminal_new_line(void)
{
    terminal->output("\n", 1);
}

void os_set_cursor(int row, int column)
{
    cursor_row = row;
    cursor_column = column;
}

static char terminal_character(zchar ch)
{
    if (ch >= 32 && ch <= 126)
        return (char)ch;
    // The fixed-width display font is ASCII. Approximate common Latin-1 letters.
    static const char latin[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (ch >= 192 && (unsigned int)(ch - 192) < sizeof(latin) - 1)
        return latin[ch - 192];
    switch (ch) {
    case 0xA0: return ' ';
    case 0xA1: return '!';
    case 0xAB: case 0xBB: return '"';
    case 0xBF: return '?';
    default: return '?';
    }
}

void os_display_char(zchar ch)
{
    const int width = os_char_width(ch);
    const char value = ch == ZC_INDENT || ch == ZC_GAP ? ' ' : terminal_character(ch);
    for (int i = 0; i < width; ++i) {
        if (cwin == 0)
            terminal->output(&value, 1);
        else
            terminal->upper_char(cursor_row, cursor_column, value);
        ++cursor_column;
    }
}

void os_display_string(const zchar *text)
{
    while (*text) {
        if (*text == ZC_NEW_STYLE || *text == ZC_NEW_FONT) {
            ++text;
            if (!*text)
                break;
        } else {
            os_display_char(*text);
        }
        ++text;
    }
}

int os_char_width(zchar ch)
{
    return ch == ZC_INDENT ? 3 : ch == ZC_GAP ? 2 : 1;
}

int os_string_width(const zchar *text)
{
    int width = 0;
    while (*text) {
        if (*text == ZC_NEW_STYLE || *text == ZC_NEW_FONT) {
            ++text;
            if (!*text)
                break;
        } else {
            width += os_char_width(*text);
        }
        ++text;
    }
    return width;
}

void os_erase_area(int top, int left, int bottom, int right, int window)
{
    if (window == -1 && cwin == 0)
        return;  // Cursor-relative edits do not clear the scrolling transcript.
    terminal->erase(top, left, bottom, right, window == -1 ? cwin : window);
}

zchar os_read_line(int limit, zchar *buffer, int timeout, int width, int continued)
{
    (void)width;
    (void)continued;
    const int key = terminal->read_line((char *)buffer, (size_t)limit, (unsigned)timeout * 100);
    if (key < 0)
        longjmp(run_boundary, 2);
    return (zchar)key;
}

zchar os_read_key(int timeout, int cursor)
{
    (void)cursor;
    const int key = terminal->read_key((unsigned)timeout * 100);
    if (key < 0)
        longjmp(run_boundary, 2);
    return (zchar)key;
}

int os_random_seed(void)
{
    next_seed = 1664525U * next_seed + 1013904223U;
    return (int)(next_seed >> 17);
}

void os_tick(void)
{
    if ((++ticks & 2047U) == 0)
        terminal->idle();
}

int os_font_data(int font, int *height, int *width)
{
    *height = *width = 1;
    return font == 0 || font == TEXT_FONT || font == FIXED_WIDTH_FONT;
}

int os_read_file_name(char *name, const char *default_name, int type)
{
    (void)name; (void)default_name; (void)type;
    return 0;  // Manual game saves go through the per-story Quetzal storage.
}

int os_peek_colour(void) { return BLACK_COLOUR; }
int os_picture_data(int picture, int *height, int *width) { (void)picture; *height = *width = 0; return 0; }
void os_beep(int tone) { (void)tone; }
void os_draw_picture(int picture, int y, int x) { (void)picture; (void)y; (void)x; }
void os_finish_with_sample(int sample) { (void)sample; }
void os_init_sound(void) {}
void os_more_prompt(void) {}
void os_prepare_sample(int sample) { (void)sample; }
void os_restart_game(int stage) { (void)stage; }
void os_scroll_area(int top, int left, int bottom, int right, int units)
{ (void)top; (void)left; (void)bottom; (void)right; (void)units; }
void os_set_colour(int foreground, int background) { (void)foreground; (void)background; }
void os_set_font(int font) { (void)font; }
void os_set_text_style(int style) { (void)style; }
void os_start_sample(int sample, int volume, int repeats, zword eos)
{ (void)sample; (void)volume; (void)repeats; (void)eos; }
void os_stop_sample(int sample) { (void)sample; }
bool os_repaint_window(int win, int old_y, int new_y, int x, int height, int width)
{ (void)win; (void)old_y; (void)new_y; (void)x; (void)height; (void)width; return FALSE; }
