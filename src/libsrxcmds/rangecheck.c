/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>

int
isvalidlunrange(int lun)
{
	return (lun >= 0 && lun < 255) ? 1 : 0;
}

int
isvalidshelfrange(int shelf)
{
	return (shelf >= 0 && shelf < 65535) ? 1 : 0;
}
