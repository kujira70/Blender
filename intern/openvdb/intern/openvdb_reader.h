/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2015 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Kevin Dietrich
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifndef __OPENVDB_READER_H__
#define __OPENVDB_READER_H__

#include <openvdb/openvdb.h>

class OpenVDBFile {
	openvdb::io::File m_file;

public:
	OpenVDBFile(const std::string &name);
	openvdb::MetaMap::Ptr metamap() const;
};

class OpenVDBReader {
	openvdb::MetaMap::Ptr m_meta_map;
	openvdb::io::File m_file;

public:
	OpenVDBReader(const std::string &filename);
	~OpenVDBReader();

	void read(const std::string &filename);

	void floatMeta(const std::string &name, float &value);
	void intMeta(const std::string &name, int &value);
	void vec3sMeta(const std::string &name, float value[3]);
	void vec3IMeta(const std::string &name, int value[3]);
	void mat4sMeta(const std::string &name, float value[4][4]);

	openvdb::GridBase::Ptr getGrid(const std::string &name);
	size_t numGrids() const;
};

#endif /* __OPENVDB_READER_H__ */