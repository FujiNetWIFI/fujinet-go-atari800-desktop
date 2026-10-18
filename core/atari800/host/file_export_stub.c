/* No screenshots or recordings from the core: the frontends own output. */
#include "config.h"
#include <stdio.h>
#include "atari.h"
#include "file_export.h"

int File_Export_Initialise(int *argc, char *argv[])
{
	(void)argc;
	(void)argv;
	return TRUE;
}

int File_Export_ReadConfig(char *string, char *ptr)
{
	(void)string;
	(void)ptr;
	return FALSE;
}

void File_Export_WriteConfig(FILE *fp) { (void)fp; }
