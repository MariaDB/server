/******************************************************
Copyright (c) 2013 Percona LLC and/or its affiliates.

The xbcrypt format writer implementation.

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

#include "xbcrypt.h"

struct xb_wcrypt_struct {
	void				*userdata;
	xb_crypt_write_callback		*write;
};

xb_wcrypt_t *
xb_crypt_write_open(void *userdata, xb_crypt_write_callback *onwrite)
{
	xb_wcrypt_t	*crypt;

	xb_ad(onwrite);

	crypt = (xb_wcrypt_t *) my_malloc(sizeof(xb_wcrypt_t), MYF(MY_FAE));

	crypt->userdata = userdata;
	crypt->write = onwrite;

	return crypt;
}

int xb_crypt_write_chunk(xb_wcrypt_t *crypt, const void *buf, size_t olen,
			 size_t elen, const void *iv, size_t ivlen)
{
	uchar		tmpbuf[XB_CRYPT_CHUNK_MAGIC_SIZE + 8 + 8 + 8 + 4 + 8];
	uchar		*ptr;
	ulong		checksum;

	xb_ad(olen <= INT_MAX);
	if (olen > INT_MAX)
		return 0;

	xb_ad(elen <= INT_MAX);
	if (elen > INT_MAX)
		return 0;

	xb_ad(ivlen <= INT_MAX);
	if (ivlen > INT_MAX)
		return 0;

	ptr = tmpbuf;

	memcpy(ptr, XB_CRYPT_CHUNK_MAGIC_CURRENT, XB_CRYPT_CHUNK_MAGIC_SIZE);
	ptr += XB_CRYPT_CHUNK_MAGIC_SIZE;

	int8store(ptr, (ulonglong)0);	/* reserved */
	ptr += 8;

	int8store(ptr, (ulonglong)olen); /* original size */
	ptr += 8;

	int8store(ptr, (ulonglong)elen); /* encrypted (actual) size */
	ptr += 8;

	checksum = crc32(0, buf, elen);
	int4store(ptr, checksum);	/* checksum */
	ptr += 4;

	int8store(ptr, (ulonglong)ivlen); /* iv size */
	ptr += 8;

	xb_ad(ptr <= tmpbuf + sizeof(tmpbuf));

	if (crypt->write(crypt->userdata, tmpbuf, ptr-tmpbuf) == -1)
		return 1;

	if (crypt->write(crypt->userdata, iv, ivlen) == -1)
		return 1;

	if (crypt->write(crypt->userdata, buf, elen) == -1)
		return 1;

	return 0;
}

int xb_crypt_write_close(xb_wcrypt_t *crypt)
{
	my_free(crypt);

	return 0;
}


