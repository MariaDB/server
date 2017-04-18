/******************************************************
Copyright (c) 2013 Percona LLC and/or its affiliates.

Encryption configuration file interface for XtraBackup.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

#include <my_base.h>
#include "common.h"
#include "xbcrypt.h"

#if GCC_VERSION >= 4002
/* Workaround to avoid "gcry_ac_* is deprecated" warnings in gcrypt.h */
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <gcrypt.h>

#if GCC_VERSION >= 4002
#  pragma GCC diagnostic warning "-Wdeprecated-declarations"
#endif

my_bool
xb_crypt_read_key_file(const char *filename, void** key, uint *keylength)
{
	FILE *fp;

	if (!(fp = my_fopen(filename, O_RDONLY, MYF(0)))) {
		msg("%s:%s: unable to open config file \"%s\", errno(%d)\n",
			my_progname, __FUNCTION__, filename, my_errno);
		return FALSE;
	}

	fseek(fp, 0 , SEEK_END);
	*keylength = ftell(fp);
	rewind(fp);
	*key = my_malloc(*keylength, MYF(MY_FAE));
	*keylength = fread(*key, 1, *keylength, fp);
	my_fclose(fp, MYF(0));
	return TRUE;
}

void
xb_crypt_create_iv(void* ivbuf, size_t ivlen)
{
	gcry_create_nonce(ivbuf, ivlen);
}
