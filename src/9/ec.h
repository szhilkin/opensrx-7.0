int	ecream(int, char *, int, uvlong, uvlong, int, int);
int	ecattach(int, char *, uvlong, int,
	long (*)(Chan *, void *, long, vlong, int), long (*)(int, void *, long, vlong, int, int));
int	ecpeek(int, int, uvlong, int);
uvlong	ecage(int, int, uvlong, int);
int	ecwrite(int, uvlong, void *, int, int);
int	ecwritel(int, int, uvlong, void *, int, int);
int	ecread(int, uvlong, void *, int, int);
int	ecreadl(int, int, uvlong, void *, int, int);
void	ecreadopen(int);
int	ecrdback(int, int *, uvlong *, void *, int, int *, ulong *);
int	ecclosedev(int, char *);
void	ecclose(int);
void	ecinval(int, int);
void ecreclaim(int,int);
void	ecpoison(int);
void	ecpriority(int, int, int, int);
char	*eccheck(void);
int	ecgetbsize(int level, vlong length);
