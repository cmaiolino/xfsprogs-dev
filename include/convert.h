/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#ifndef __CONVERT_H__
#define __CONVERT_H__

extern int64_t	cvt_s64(char *s, int base);
extern int32_t	cvt_s32(char *s, int base);
extern int16_t	cvt_s16(char *s, int base);

extern uint64_t	cvt_u64(char *s, int base);
extern uint32_t	cvt_u32(char *s, int base);
extern uint16_t	cvt_u16(char *s, int base);

extern long long cvtnum(size_t blocksize, size_t sectorsize, char *s);
extern void cvtstr(double value, char *str, size_t sz);
extern unsigned long cvttime(char *s);

extern uid_t	uid_from_string(char *user);
extern gid_t	gid_from_string(char *group);
extern prid_t	prid_from_string(char *project);

#endif	/* __CONVERT_H__ */
