// Copyright © 2013 Coraid, Inc.
// All rights reserved.

enum {
	Maxargs = 256,
	Maxbuf	= 8192,
};

enum {
    	RespondYes,	 /* ask respond one      */
       	RespondAll, 	/* ask respond all      */
         	RespondQuit,	/* quit */
	RespondNo	/* no */

};

/* structure used in CLI spares to sort drives in numerical order */
typedef struct spareStr_e {
	char *name;
	char *size;
}spareStr;

char *mustsmprint(char *fmt, ...);
vlong fstrtoll(char *str);		/* formatted str to llong */

int srxwritefile(char *file, char *fmt, va_list arg);
int srxtruncatefile(char *file);

/* funcs to feed into qsort */
int dirintcmp(void *a, void *b);		/* cmp for Dir names that are int */
int sparestrcmp(void *sa, void *sb);

/* funcs to get lun or drive state/status */
int getshelf(void);			/* return shelf or -1 if unset	*/
int parsedrive(char *name, int checkstate, char **rret);
int isif(char *interface);
int iscm(void);
int isslot(char *slot);
int islun(char *lun);
int islunonline(char *lun);
int parselundotpartdotdrive(char *lpd, char **lun, char **part, char **drive, void (*usage)(void));
char *parseshelfdotslot(char *shelfdotslot, void (*usage)(void), char **strsuffix, int checkstate);

/* functions to access name space */
int cmctlwrite(char *fmt, ...);
int lunctlwrite(char *lun, char *fmt, ...);		/* Write msg into lun's  ctl file	*/
int lunlabelwrite(char *lun, char *name); 	/* write a label to lun's label file */
int rmlunlabel(char *lun); 			/* remove a lebel from lun's label file */
int drivectlwrite(char *drive, char *fmt, ...);	/* Write msg into lun's  ctl file	*/
int makelun(char *options, char *lun, char *raidtype, int argc, char **argv, int clean, int noprompt);

/* confirmation on action */
int ask(char *s, int quit);
void askhdr(int c, char **v);

int shellcmd(char *);
int isvalidlunrange(int);
int isvalidshelfrange(int);
