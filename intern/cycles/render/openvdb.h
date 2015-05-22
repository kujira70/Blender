/*
 * Copyright 2015 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __OPENVDBMANAGER_H__
#define __OPENVDBMANAGER_H__

#include "util_openvdb.h"
#include "util_string.h"
#include "util_types.h"

CCL_NAMESPACE_BEGIN

class Device;
class DeviceScene;
class Progress;
class Scene;

class OpenVDBManager {	
	struct GridDescription {
		string filename;
		string name;
		int sampling;
		int slot;
	};

#ifdef WITH_OPENVDB
	vector<GridDescription> current_grids;
	vector<openvdb::FloatGrid::Ptr> scalar_grids;
	vector<openvdb::Vec3SGrid::Ptr> vector_grids;

	size_t add_scalar_grid(openvdb::FloatGrid::Ptr grid, int sampling);
	size_t add_vector_grid(openvdb::Vec3SGrid::Ptr grid, int sampling);
	void delete_sampler(int grid_type, int sampling, size_t slot);

	void add_grid_description(const string &filename, const string &name, int sampling, int slot);
	int find_existing_slot(const string &filename, const string &name, int sampling, int grid_type);
#endif

public:
	OpenVDBManager();
	~OpenVDBManager();

	int add_volume(const string &filename, const string &name, int sampling, int grid_type);

	void device_update(Device *device, DeviceScene *dscene, Scene *scene, Progress& progress);
	void device_free(Device *device, DeviceScene *dscene);

	bool need_update;

#ifdef WITH_OPENVDB
	vector<vdb_fsampler_p*> float_samplers_p;
	vector<vdb_fsampler_b*> float_samplers_b;
	vector<vdb_vsampler_p*> float3_samplers_p;
	vector<vdb_vsampler_b*> float3_samplers_b;
#endif
};

CCL_NAMESPACE_END

#endif /* __OPENVDBMANAGER_H__ */