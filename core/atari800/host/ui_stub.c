/* atari800's ui.c is replaced by the native frontends. */
#include "config.h"
#include "atari.h"
#include "ui.h"

int UI_is_active = FALSE;
int UI_alt_function = -1;
int UI_current_function = -1;
int UI_crash_code = 0;
UWORD UI_crash_address = 0;
UWORD UI_crash_afterCIM = 0;
char UI_atari_files_dir[UI_MAX_DIRECTORIES][FILENAME_MAX];
char UI_saved_files_dir[UI_MAX_DIRECTORIES][FILENAME_MAX];
int UI_n_atari_files_dir = 0;
int UI_n_saved_files_dir = 0;
int UI_show_hidden_files = FALSE;

int UI_Initialise(int *argc, char *argv[])
{
	(void)argc;
	(void)argv;
	return TRUE;
}

void UI_Run(void) {}

/* The session picks the type itself (a800session_cart_insert). */
int UI_SelectCartType(int k)
{
	(void)k;
	return 0;
}
